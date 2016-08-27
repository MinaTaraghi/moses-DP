//-*- c++ -*-

#ifndef __ug_bitext_h
#define __ug_bitext_h
// Implementations of word-aligned bitext.
// Written by Ulrich Germann
// 
// mmBitext: static, memory-mapped bitext
// imBitext: dynamic, in-memory bitext
//

// things we can do to speed up things:
// - set up threads at startup time that force the 
//   data in to memory sequentially
//
// - use multiple agendas for better load balancing and to avoid 
//   competition for locks
// 


#define UG_BITEXT_TRACK_ACTIVE_THREADS 0

#include <string>
#include <vector>
#include <cassert>
#include <iomanip>
#include <algorithm>

#include <boost/unordered_map.hpp>
#include <boost/foreach.hpp>
#include <boost/thread.hpp>
#include <boost/random.hpp>
#include <boost/format.hpp>

#include "moses/TranslationModel/UG/generic/sorting/VectorIndexSorter.h"
#include "moses/TranslationModel/UG/generic/sampling/Sampling.h"
#include "moses/TranslationModel/UG/generic/file_io/ug_stream.h"
#include "moses/TranslationModel/UG/generic/threading/ug_thread_safe_counter.h"
#include "moses/Util.h"
#include "moses/StaticData.h"

#include "util/exception.hh"
// #include "util/check.hh"

#include "ug_typedefs.h"
#include "ug_mm_ttrack.h"
#include "ug_im_ttrack.h"
#include "ug_mm_tsa.h"
#include "ug_im_tsa.h"
#include "tpt_tokenindex.h"
#include "ug_corpus_token.h"
#include "tpt_pickler.h"
#include "ug_lexical_phrase_scorer2.h"
#include "ug_phrasepair.h"
#include "ug_lru_cache.h"

#define PSTATS_CACHE_THRESHOLD 50

using namespace ugdiss;
using namespace std;
namespace Moses {
  class Mmsapt;
  namespace bitext
  {
    template<typename TKN> class Bitext;
    template<typename TKN> class PhrasePair;
    using namespace ugdiss;

    template<typename TKN> class Bitext;

    enum PhraseOrientation 
    {
      po_first,
      po_mono,
      po_jfwd,
      po_swap,
      po_jbwd,
      po_last,
      po_other
    };

    PhraseOrientation 
    find_po_fwd(vector<vector<ushort> >& a1,
		vector<vector<ushort> >& a2,
		size_t b1, size_t e1,
		size_t b2, size_t e2);

    PhraseOrientation 
    find_po_bwd(vector<vector<ushort> >& a1,
		vector<vector<ushort> >& a2,
		size_t b1, size_t e1,
		size_t b2, size_t e2);

    template<typename sid_t, typename off_t, typename len_t>
    void 
    parse_pid(::uint64_t const pid, sid_t & sid, 
	      off_t & off, len_t& len)
    {
      static ::uint64_t two32 = ::uint64_t(1)<<32;
      static ::uint64_t two16 = ::uint64_t(1)<<16;
      len = pid%two16;
      off = (pid%two32)>>16;
      sid = pid>>32;
    }

    float 
    lbop(size_t const tries, size_t const succ, 
	 float const confidence);

    // "joint" (i.e., phrase pair) statistics
    class
    jstats
    {
      boost::mutex lock;
      uint32_t my_rcnt; // unweighted count
      float    my_wcnt; // weighted count 
      uint32_t my_cnt2;
      vector<pair<size_t, vector<uchar> > > my_aln; 
      uint32_t ofwd[7], obwd[7];
    public:
      jstats();
      jstats(jstats const& other);
      uint32_t rcnt() const;
      uint32_t cnt2() const; // raw target phrase occurrence count
      float    wcnt() const;
      
      vector<pair<size_t, vector<uchar> > > const & aln() const;
      void add(float w, vector<uchar> const& a, uint32_t const cnt2,
	       uint32_t fwd_orient, uint32_t bwd_orient);
      void invalidate();
      void validate();
      bool valid();
      uint32_t dcnt_fwd(PhraseOrientation const idx) const;
      uint32_t dcnt_bwd(PhraseOrientation const idx) const;
    };

    struct 
    pstats
    {

#if UG_BITEXT_TRACK_ACTIVE_THREADS
      static ThreadSafeCounter active;
#endif
      boost::mutex lock;               // for parallel gathering of stats
      boost::condition_variable ready; // consumers can wait for this data structure to be ready.
      
      size_t raw_cnt;    // (approximate) raw occurrence count 
      size_t sample_cnt; // number of instances selected during sampling
      size_t good;       // number of selected instances with valid word alignments
      size_t sum_pairs;
      size_t in_progress; // keeps track of how many threads are currently working on this

      uint32_t ofwd[po_other+1], obwd[po_other+1];

      // typedef typename boost::unordered_map<typename ::uint64_t, jstats> trg_map_t;
      typedef std::map<typename ::uint64_t, jstats> trg_map_t;
      trg_map_t trg;
      pstats();
      ~pstats();
      void release();
      void register_worker();
      size_t count_workers() { return in_progress; } 

      bool 
      add(::uint64_t const pid, 
	  float    const w, 
	  vector<uchar> const& a, 
	  uint32_t      const cnt2,
	  uint32_t fwd_o, uint32_t bwd_o);
    };
    

    template<typename Token>
    string 
    toString(TokenIndex const& V, Token const* x, size_t const len)
    {
      if (!len) return "";
      UTIL_THROW_IF2(!x, HERE << ": Unexpected end of phrase!");
      ostringstream buf; 
      buf << V[x->id()];
      size_t i = 1;
      for (x = x->next(); x && i < len; ++i, x = x->next())
	buf << " " << V[x->id()];
      UTIL_THROW_IF2(i != len, HERE << ": Unexpected end of phrase!");
      return buf.str();
    }

    template<typename Token>
    class 
    PhrasePair
    {
    public:
      class Scorer { public: virtual float operator()(PhrasePair& pp) const = 0; };
      Token const* start1;
      Token const* start2;
      uint32_t len1;
      uint32_t len2;
      ::uint64_t p1, p2;
      uint32_t raw1,raw2,sample1,sample2,good1,good2,joint;
      vector<float> fvals;
      float dfwd[po_other+1]; // distortion counts // counts or probs?
      float dbwd[po_other+1]; // distortion counts
      vector<uchar> aln;
      float score;
      bool inverse;
      PhrasePair() { };
      PhrasePair(PhrasePair const& o);

      PhrasePair const& operator+=(PhrasePair const& other);

      bool operator<(PhrasePair const& other) const;
      bool operator>(PhrasePair const& other) const;
      bool operator<=(PhrasePair const& other) const; 
      bool operator>=(PhrasePair const& other) const;

      void init();
      void init(::uint64_t const pid1, bool is_inverse, 
		Token const* x,   uint32_t const len,
		pstats const* ps = NULL, size_t const numfeats=0);
      
      // void init(::uint64_t const pid1, pstats const& ps,  size_t const numfeats);
      // void init(::uint64_t const pid1, pstats const& ps1, pstats const& ps2, 
      // size_t const numfeats);

      // PhrasePair const&
      // update(::uint64_t const pid2, size_t r2 = 0);

      PhrasePair const& 
      update(::uint64_t const pid2, Token const* x, 
	     uint32_t const len, jstats const& js);
      
      // PhrasePair const& 
      // update(::uint64_t const pid2, jstats   const& js1, jstats   const& js2);

      // PhrasePair const& 
      // update(::uint64_t const pid2, size_t const raw2extra, jstats const& js);

      // float 
      // eval(vector<float> const& w);

      class SortByTargetIdSeq
      {
      public:
	int cmp(PhrasePair const& a, PhrasePair const& b) const;
	bool operator()(PhrasePair const& a, PhrasePair const& b) const;
      };
    };

    template<typename Token>
    void
    PhrasePair<Token>::
    init(::uint64_t const pid1, bool is_inverse, Token const* x, uint32_t const len, 
	 pstats const* ps, size_t const numfeats)
    {
      inverse = is_inverse;
      start1 = x; len1 = len;
      p1     = pid1;
      p2     = 0;
      if (ps)
	{
	  raw1    = ps->raw_cnt;
	  sample1 = ps->sample_cnt;
	  good1   = ps->good;
	}
      else raw1 = sample1 = good1 = 0;
      joint   = 0;
      good2   = 0;
      sample2 = 0;
      raw2    = 0;
      fvals.resize(numfeats);
    }

    template<typename Token>
    PhrasePair<Token> const&
    PhrasePair<Token>::
    update(::uint64_t const pid2, 
	   Token const* x, uint32_t const len, jstats const& js)   
    {
      p2    = pid2;
      start2 = x; len2 = len;
      raw2  = js.cnt2();
      joint = js.rcnt();
      assert(js.aln().size());
      if (js.aln().size()) 
	aln = js.aln()[0].second;
      float total_fwd = 0, total_bwd = 0;
      for (int i = po_first; i <= po_other; i++)
	{
	  PhraseOrientation po = static_cast<PhraseOrientation>(i);
	  total_fwd += js.dcnt_fwd(po)+1;
	  total_bwd += js.dcnt_bwd(po)+1;
	}

      // should we do that here or leave the raw counts?
      for (int i = po_first; i <= po_other; i++)
	{
	  PhraseOrientation po = static_cast<PhraseOrientation>(i);
	  dfwd[i] = float(js.dcnt_fwd(po)+1)/total_fwd;
	  dbwd[i] = float(js.dcnt_bwd(po)+1)/total_bwd;
	}

      return *this;
    }

    template<typename Token>
    bool 
    PhrasePair<Token>::
    operator<(PhrasePair const& other) const 
    { return this->score < other.score; }
    
    template<typename Token>
    bool 
    PhrasePair<Token>::
    operator>(PhrasePair const& other) const
    { return this->score > other.score; }

    template<typename Token>
    bool 
    PhrasePair<Token>::
    operator<=(PhrasePair const& other) const 
    { return this->score <= other.score; }
    
    template<typename Token>
    bool 
    PhrasePair<Token>::
    operator>=(PhrasePair const& other) const
    { return this->score >= other.score; }

    template<typename Token>
    PhrasePair<Token> const&
    PhrasePair<Token>::
    operator+=(PhrasePair const& o) 
    { 
      raw1 += o.raw1;
      raw2 += o.raw2;
      sample1 += o.sample1;
      sample2 += o.sample2;
      good1 += o.good1;
      good2 += o.good2;
      joint += o.joint;
      return *this;
    }

    template<typename Token>
    PhrasePair<Token>::
    PhrasePair(PhrasePair<Token> const& o) 
      : start1(o.start1)
      , start2(o.start2)
      , len1(o.len1)
      , len2(o.len2)
      , p1(o.p1) 
      , p2(o.p2)
      , raw1(o.raw1) 
      , raw2(o.raw2) 
      , sample1(o.sample1)
      , sample2(o.sample2)
      ,	good1(o.good1)
      , good2(o.good2)
      , joint(o.joint)
      , fvals(o.fvals)
      , aln(o.aln)
      , score(o.score)
      , inverse(o.inverse)
    {
      for (size_t i = 0; i <= po_other; ++i)
	{
	  dfwd[i] = o.dfwd[i];
	  dbwd[i] = o.dbwd[i];
	}
    }
    
    template<typename Token>
    int
    PhrasePair<Token>::
    SortByTargetIdSeq::
    cmp(PhrasePair const& a, PhrasePair const& b) const
    {
      size_t i = 0;
      Token const* x = a.start2;
      Token const* y = b.start2;
      while (i < a.len2 && i < b.len2 && x->id() == y->id()) 
	{
	  x = x->next();
	  y = y->next();
	  ++i;
	}
      if (i == a.len2 && i == b.len2) return 0;
      if (i == a.len2) return -1;
      if (i == b.len2) return  1;
      return x->id() < y->id() ? -1 : 1;
    }
    
    template<typename Token>
    bool
    PhrasePair<Token>::
    SortByTargetIdSeq::
    operator()(PhrasePair const& a, PhrasePair const& b) const
    {
      return this->cmp(a,b) < 0;
    }

    template<typename Token>
    void 
    PhrasePair<Token>::
    init()
    {
      inverse = false;
      len1 = len2 = raw1 = raw2 = sample1 = sample2 = good1 = good2 = joint = 0;
      start1 = start2 = NULL;
      p1 = p2 = 0;
    }

    template<typename TKN>
    class Bitext 
    {
      friend class Moses::Mmsapt;
    protected:
      mutable boost::mutex lock;
      mutable boost::mutex cache_lock;
    public:
      typedef TKN Token;
      typedef typename TSA<Token>::tree_iterator iter;

      class agenda;
      // stores the list of unfinished jobs;
      // maintains a pool of workers and assigns the jobs to them
      
      // to be done: work with multiple agendas for faster lookup
      // (multiplex jobs); not sure if an agenda having more than 
      // four or so workers is efficient, because workers get into 
      // each other's way. 
      mutable sptr<agenda> ag; 
      
      sptr<Ttrack<char> >  Tx; // word alignments
      sptr<Ttrack<Token> > T1; // token track
      sptr<Ttrack<Token> > T2; // token track
      sptr<TokenIndex>     V1; // vocab
      sptr<TokenIndex>     V2; // vocab
      sptr<TSA<Token> >    I1; // indices
      sptr<TSA<Token> >    I2; // indices
      
      /// given the source phrase sid[start:stop]
      //  find the possible start (s1 .. s2) and end (e1 .. e2) 
      //  points of the target phrase; if non-NULL, store word
      //  alignments in *core_alignment. If /flip/, source phrase is 
      //  L2.
      bool 
      find_trg_phr_bounds
      (size_t const sid, size_t const start, size_t const stop, 
       size_t & s1, size_t & s2, size_t & e1, size_t & e2, 
       int& po_fwd, int& po_bwd,
       vector<uchar> * core_alignment, 
       bitvector* full_alignment,
       bool const flip) const;
      
#if 1
      typedef boost::unordered_map<typename ::uint64_t,sptr<pstats> > pcache_t;
#else
      typedef map<typename ::uint64_t,sptr<pstats> > pcache_t;
#endif
      mutable pcache_t cache1,cache2;
    protected:
      typedef typename 
      lru_cache::LRU_Cache<typename ::uint64_t, vector<PhrasePair<Token> > >  
      pplist_cache_t;

      size_t default_sample_size;
      size_t num_workers;
      size_t m_pstats_cache_threshold;
      mutable pplist_cache_t m_pplist_cache1, m_pplist_cache2;
    private:
      sptr<pstats> 
      prep2(iter const& phrase, size_t const max_sample,
	    vector<float> const* const bias) const;
    public:
      Bitext(size_t const max_sample =1000, 
	     size_t const xnum_workers =16);

      Bitext(Ttrack<Token>* const t1, 
	     Ttrack<Token>* const t2, 
	     Ttrack<char>*  const tx,
	     TokenIndex*    const v1, 
	     TokenIndex*    const v2,
	     TSA<Token>* const i1, 
	     TSA<Token>* const i2,
	     size_t const max_sample=1000,
	     size_t const xnum_workers=16);
	     
      virtual void open(string const base, string const L1, string const L2) = 0;
      
      // sptr<pstats> lookup(Phrase const& phrase, size_t factor) const;
      sptr<pstats> lookup(iter const& phrase,vector<float> const* const bias=NULL) const;
      sptr<pstats> lookup(iter const& phrase, size_t const max_sample,
			  vector<float> const* const bias) const;

      void
      lookup(vector<Token> const& snt, TSA<Token>& idx, 
	     vector<vector<sptr<vector<PhrasePair<Token> > > > >& dest,
	     vector<vector<typename ::uint64_t> >* pidmap = NULL,
	     typename PhrasePair<Token>::Scorer* scorer=NULL, 
	     vector<float> const* const bias=NULL,
	     bool multithread=true) const;

      void prep(iter const& phrase, vector<float> const* const bias) const;

      void   setDefaultSampleSize(size_t const max_samples);
      size_t getDefaultSampleSize() const;

      string toString(::uint64_t pid, int isL2) const;

      virtual size_t revision() const { return 0; }
    };

    template<typename Token>
    string
    Bitext<Token>::
    toString(::uint64_t pid, int isL2) const
    {
      ostringstream buf;
      uint32_t sid,off,len; parse_pid(pid,sid,off,len);
      Token const* t = (isL2 ? T2 : T1)->sntStart(sid) + off;
      Token const* x = t + len;
      TokenIndex const& V = isL2 ? *V2 : *V1;
      while (t < x) 
	{
	  buf << V[t->id()];
	  if (++t < x) buf << " ";
	}
      return buf.str();
    }
    
    

    template<typename Token>
    size_t 
    Bitext<Token>::
    getDefaultSampleSize() const 
    { 
      return default_sample_size; 
    }
    template<typename Token>
    void 
    Bitext<Token>::
    setDefaultSampleSize(size_t const max_samples)
    { 
      boost::lock_guard<boost::mutex> guard(this->lock);
      if (max_samples != default_sample_size) 
	{
	  cache1.clear();
	  cache2.clear();
	  default_sample_size = max_samples; 
	}
    }

    template<typename Token>
    Bitext<Token>::
    Bitext(size_t const max_sample, size_t const xnum_workers)
      : default_sample_size(max_sample)
      , num_workers(xnum_workers)
      , m_pstats_cache_threshold(PSTATS_CACHE_THRESHOLD)
    { }

    template<typename Token>
    Bitext<Token>::
    Bitext(Ttrack<Token>* const t1, 
	   Ttrack<Token>* const t2, 
	   Ttrack<char>*  const tx,
	   TokenIndex*    const v1, 
	   TokenIndex*    const v2,
	   TSA<Token>* const i1, 
	   TSA<Token>* const i2,
	   size_t const max_sample,
	   size_t const xnum_workers)
      : Tx(tx), T1(t1), T2(t2), V1(v1), V2(v2), I1(i1), I2(i2)
      , default_sample_size(max_sample)
      , num_workers(xnum_workers)
      , m_pstats_cache_threshold(PSTATS_CACHE_THRESHOLD)
    { }

    // agenda is a pool of jobs 
    template<typename Token>
    class 
    Bitext<Token>::
    agenda
    {
      boost::mutex lock; 
      class job 
      {
#if UG_BITEXT_TRACK_ACTIVE_THREADS
	static ThreadSafeCounter active;
#endif
	boost::mutex lock; 
	friend class agenda;
	boost::taus88 rnd;  // every job has its own pseudo random generator 
	double rnddenom;    // denominator for scaling random sampling
	size_t min_diverse; // minimum number of distinct translations
      public:
	size_t         workers; // how many workers are working on this job?
	sptr<TSA<Token> const> root; // root of the underlying suffix array
	char const*       next; // next position to read from 
	char const*       stop; // end of index range
	size_t     max_samples; // how many samples to extract at most
	size_t             ctr; /* # of phrase occurrences considered so far
				 * # of samples chosen is stored in stats->good 
				 */
	size_t             len; // phrase length
	bool               fwd; // if true, source phrase is L1 
	sptr<pstats>     stats; // stores statistics collected during sampling
	vector<float> const* bias; // sentence-level bias for sampling
	float bias_total;
	bool step(::uint64_t & sid, ::uint64_t & offset); // select another occurrence
	bool done() const;
	job(typename TSA<Token>::tree_iterator const& m, 
	    sptr<TSA<Token> > const& r, size_t maxsmpl, bool isfwd, 
	    vector<float> const* const bias);
	~job();
      };
    public:      
      class 
      worker
      {
	agenda& ag;
      public:
	worker(agenda& a) : ag(a) {}
	void operator()();
      };
    private:
      list<sptr<job> > joblist;
      vector<sptr<boost::thread> > workers;
      bool shutdown;
      size_t doomed;
    public:
      Bitext<Token>   const& bt;
      agenda(Bitext<Token> const& bitext);
      ~agenda();
      void add_workers(int n);

      sptr<pstats> 
      add_job(typename TSA<Token>::tree_iterator const& phrase, 
	      size_t const max_samples, 
	      vector<float> const* const bias);

      sptr<job> get_job();
    };
    
    template<typename Token>
    bool
    Bitext<Token>::
    agenda::
    job::
    step(::uint64_t & sid, ::uint64_t & offset)
    {
      boost::lock_guard<boost::mutex> jguard(lock);
      bool ret = (max_samples == 0) && (next < stop);
      if (ret)
	{
	  next = root->readSid(next,stop,sid);
	  next = root->readOffset(next,stop,offset);
	  boost::lock_guard<boost::mutex> sguard(stats->lock);
	  if (stats->raw_cnt == ctr) ++stats->raw_cnt;
	  if (bias && bias->at(sid) == 0)
	    return false;
	  stats->sample_cnt++;
	}
      else 
	{
	  while (next < stop && (stats->good < max_samples || 
				 stats->trg.size() < min_diverse))
	    {
	      next = root->readSid(next,stop,sid);
	      next = root->readOffset(next,stop,offset);
	      { // brackets required for lock scoping; see sguard immediately below
		boost::lock_guard<boost::mutex> sguard(stats->lock); 
		if (stats->raw_cnt == ctr) ++stats->raw_cnt;
		size_t scalefac = (stats->raw_cnt - ctr++);
		size_t rnum = scalefac * (rnd()/(rnd.max()+1.));
		size_t th = (bias_total 
			     ? bias->at(sid)/bias_total * bias->size() * max_samples
			     : max_samples);
#if 0
		cerr << rnum << "/" << scalefac << " vs. " 
		     << max_samples - stats->good << " ("
		     << max_samples << " - " << stats->good << ")" 
		     << " th=" << th;
		if (bias) 
		  cerr << " with bias " << bias->at(sid) 
		       << " => " << bias->at(sid) * bias->size();
		else cerr << " without bias";
		cerr << endl;
#endif
		if (rnum + stats->good < th)
		  {
		    stats->sample_cnt++;
		    ret = true;
		    break;
		  }
	      }
	    }
	}
      
      // boost::lock_guard<boost::mutex> sguard(stats->lock); 
      // abuse of lock for clean output to cerr
      // cerr << stats->sample_cnt++;
      return ret;
    }

    template<typename Token>
    void
    Bitext<Token>::
    agenda::
    add_workers(int n)
    {
      static boost::posix_time::time_duration nodelay(0,0,0,0); 
      boost::lock_guard<boost::mutex> guard(this->lock);

      int target  = max(1, int(n + workers.size() - this->doomed));
      // house keeping: remove all workers that have finished
      for (size_t i = 0; i < workers.size(); )
	{
	  if (workers[i]->timed_join(nodelay))
	    {
	      if (i + 1 < workers.size())
		workers[i].swap(workers.back());
	      workers.pop_back();
	    }
	  else ++i;
	}
      // cerr << workers.size() << "/" << target << " active" << endl;
      if (int(workers.size()) > target)
	this->doomed = workers.size() - target;
      else 
	while (int(workers.size()) < target)
	  {
	    sptr<boost::thread> w(new boost::thread(worker(*this)));
	    workers.push_back(w);
	  }
    }

    template<typename Token>
    void
    Bitext<Token>::
    agenda::
    worker::
    operator()()
    {
      // things to do:
      // - have each worker maintain their own pstats object and merge results at the end;
      // - ensure the minimum size of samples considered by a non-locked counter that is only 
      //   ever incremented -- who cares if we look at more samples than required, as long
      //   as we look at at least the minimum required
      // This way, we can reduce the number of lock / unlock operations we need to do during 
      // sampling. 
      size_t s1=0, s2=0, e1=0, e2=0;
      ::uint64_t sid=0, offset=0; // of the source phrase
      while(sptr<job> j = ag.get_job())
	{
	  j->stats->register_worker();
	  vector<uchar> aln;
	  bitvector full_alignment(100*100);
	  while (j->step(sid,offset))
	    {
	      aln.clear();
	      int po_fwd=po_other,po_bwd=po_other;
	      if (j->fwd)
		{
		  if (!ag.bt.find_trg_phr_bounds
		      (sid,offset,offset+j->len,s1,s2,e1,e2,po_fwd,po_bwd,
		       &aln,&full_alignment,false))
		    continue;
		}
	      else if (!ag.bt.find_trg_phr_bounds
		       (sid,offset,offset+j->len,s1,s2,e1,e2,po_fwd,po_bwd,
			&aln,NULL,true)) // NULL,NULL,true))
		continue;
	      j->stats->lock.lock(); 
	      j->stats->good += 1; 
	      j->stats->sum_pairs += (s2-s1+1)*(e2-e1+1);
	      ++j->stats->ofwd[po_fwd];
	      ++j->stats->obwd[po_bwd];
	      j->stats->lock.unlock();
	      // for (size_t k = j->fwd ? 1 : 0; k < aln.size(); k += 2) 
	      for (size_t k = 1; k < aln.size(); k += 2) 
		aln[k] += s2 - s1;
	      Token const* o = (j->fwd ? ag.bt.T2 : ag.bt.T1)->sntStart(sid);
	      float sample_weight = 1./((s2-s1+1)*(e2-e1+1));

	      vector<typename ::uint64_t> seen; 
	      seen.reserve(100);
	      // It is possible that the phrase extraction extracts the same
	      // phrase twice, e.g., when word a co-occurs with sequence b b b
	      // but is aligned only to the middle word. We can only count
	      // each phrase pair once per source phrase occurrence, or else
	      // run the risk of having more joint counts than marginal
	      // counts.

	      for (size_t s = s1; s <= s2; ++s)
		{
		  sptr<iter> b = (j->fwd ? ag.bt.I2 : ag.bt.I1)->find(o+s,e1-s);
		  if (!b || b->size() < e1 -s)
		    UTIL_THROW(util::Exception, "target phrase not found");
		  // assert(b);
		  for (size_t i = e1; i <= e2; ++i)
		    {
		      ::uint64_t tpid = b->getPid();
		      size_t s = 0;
		      while (s < seen.size() && seen[s] != tpid) ++s;
		      if (s < seen.size())
			{
#if 0
			  size_t sid, off, len;
			  parse_pid(tpid,sid,off,len);
			  cerr << "HA, gotcha! " << sid << ":" << off << " at " << HERE << endl;
			  for (size_t z = 0; z < len; ++z)
			    {
			      id_type tid = ag.bt.T2->sntStart(sid)[off+z].id();
			      cerr << (*ag.bt.V2)[tid] << " "; 
			    }
			  cerr << endl;
#endif
			  continue;
			}
		      seen.push_back(tpid);
		      if (! j->stats->add(tpid,sample_weight,aln,
					  b->approxOccurrenceCount(),
					  po_fwd,po_bwd))
			{
			  cerr << "FATAL ERROR AT " << __FILE__ 
			       << ":" << __LINE__ << endl;
			  assert(0);
			  ostringstream msg;
			  for (size_t z = 0; z < j->len; ++z)
			    {
			      id_type tid = ag.bt.T1->sntStart(sid)[offset+z].id();
			      cerr << (*ag.bt.V1)[tid] << " "; 
			    }
			  cerr << endl;
			  for (size_t z = s; z <= i; ++z)
			    cerr << (*ag.bt.V2)[(o+z)->id()] << " "; 
			  cerr << endl;
			  assert(0);
			  UTIL_THROW(util::Exception,"Error in sampling.");
			}
		      if (i < e2)
			{
#ifndef NDEBUG
			  bool ok = b->extend(o[i].id());
			  assert(ok);
#else
			  b->extend(o[i].id());
			  // cerr << "boo" << endl;
#endif 
			}
		    }
		  // if (j->fwd && s < s2) 
		  // for (size_t k = j->fwd ? 1 : 0; k < aln.size(); k += 2) 
		  if (s < s2)
		    for (size_t k = 1; k < aln.size(); k += 2) 
		      --aln[k];
		}
	      // j->stats->lock.unlock();
	    }
	  j->stats->release();
	}
    }

    template<typename Token>
    Bitext<Token>::
    agenda::
    job::
    ~job()
    {
      if (stats) stats.reset();
#if UG_BITEXT_TRACK_ACTIVE_THREADS
      try { --active; } catch (...) {} 
#endif
      // counter may not exist any more at destruction time
    }

    template<typename Token>
    Bitext<Token>::
    agenda::
    job::
    job(typename TSA<Token>::tree_iterator const& m, 
	sptr<TSA<Token> > const& r, size_t maxsmpl, 
	bool isfwd, vector<float> const* const sntbias)
      : rnd(0)
      , rnddenom(rnd.max() + 1.)
      , min_diverse(10)
      , workers(0)
      , root(r)
      , next(m.lower_bound(-1))
      , stop(m.upper_bound(-1))
      , max_samples(maxsmpl)
      , ctr(0)
      , len(m.size())
      , fwd(isfwd)
      , bias(sntbias)
    {
      stats.reset(new pstats());
      stats->raw_cnt = m.approxOccurrenceCount();
      bias_total = 0; // needed for renormalization
      if (bias)
	{
	  for (char const* x = m.lower_bound(-1); x < stop;)
	    {
	      uint32_t sid; ushort offset;
	      next = root->readSid(next,stop,sid);
	      next = root->readOffset(next,stop,offset);
	      bias_total += bias->at(sid);
	    }
	}
#if UG_BITEXT_TRACK_ACTIVE_THREADS
      // if (++active%5 == 0) 
      ++active;
      // cerr << size_t(active) << " active jobs at " << __FILE__ << ":" << __LINE__ << endl;
#endif
    }

    template<typename Token>
    sptr<pstats> 
    Bitext<Token>::
    agenda::
    add_job(typename TSA<Token>::tree_iterator const& phrase, 
	    size_t const max_samples, vector<float> const* const bias)
    {
      boost::unique_lock<boost::mutex> lk(this->lock);
      static boost::posix_time::time_duration nodelay(0,0,0,0); 
      bool fwd = phrase.root == bt.I1.get();
      sptr<job> j(new job(phrase, fwd ? bt.I1 : bt.I2, max_samples, fwd, bias));
      j->stats->register_worker();
      
      joblist.push_back(j);
      if (joblist.size() == 1)
	{
	  size_t i = 0;
	  while (i < workers.size())
	    {
	      if (workers[i]->timed_join(nodelay))
		{
		  if (doomed)
		    {
		      if (i+1 < workers.size())
			workers[i].swap(workers.back());
		      workers.pop_back();
		      --doomed;
		    }
		  else
		    workers[i++] = sptr<boost::thread>(new boost::thread(worker(*this)));
		}
	      else ++i;
	    }
	}
      return j->stats;
    }
    
    template<typename Token>
    sptr<typename Bitext<Token>::agenda::job>
    Bitext<Token>::
    agenda::
    get_job()
    {
      // cerr << workers.size() << " workers on record" << endl;
      sptr<job> ret;
      if (this->shutdown) return ret;
      boost::unique_lock<boost::mutex> lock(this->lock);
      if (this->doomed) 
	{
	  --this->doomed;
	  return ret;
	}
      typename list<sptr<job> >::iterator j = joblist.begin();
      while (j != joblist.end())
	{
	  if ((*j)->done()) 
	    {
	      (*j)->stats->release();
	      joblist.erase(j++);
	    } 
	  else if ((*j)->workers >= 4) 
	    {
	      ++j;
	    }
	  else break;
	}
      if (joblist.size())
	{
	  ret = j == joblist.end() ? joblist.front() : *j;
	  boost::lock_guard<boost::mutex> jguard(ret->lock);
	  ++ret->workers;
	}
      return ret;
    }

   
    template<typename TKN>
    class mmBitext : public Bitext<TKN>
    {
    public:
      void open(string const base, string const L1, string L2);
      mmBitext();
    };

    template<typename TKN>
    mmBitext<TKN>::
    mmBitext()
      : Bitext<TKN>(new mmTtrack<TKN>(),
		    new mmTtrack<TKN>(),
		    new mmTtrack<char>(),
		    new TokenIndex(),
		    new TokenIndex(),
		    new mmTSA<TKN>(),
		    new mmTSA<TKN>())
    {};
    
    template<typename TKN>
    void
    mmBitext<TKN>::
    open(string const base, string const L1, string L2)
    {
      mmTtrack<TKN>& t1 = *reinterpret_cast<mmTtrack<TKN>*>(this->T1.get());
      mmTtrack<TKN>& t2 = *reinterpret_cast<mmTtrack<TKN>*>(this->T2.get());
      mmTtrack<char>& tx = *reinterpret_cast<mmTtrack<char>*>(this->Tx.get());
      t1.open(base+L1+".mct");
      t2.open(base+L2+".mct");
      tx.open(base+L1+"-"+L2+".mam");
      this->V1->open(base+L1+".tdx"); this->V1->iniReverseIndex();
      this->V2->open(base+L2+".tdx"); this->V2->iniReverseIndex();
      mmTSA<TKN>& i1 = *reinterpret_cast<mmTSA<TKN>*>(this->I1.get());
      mmTSA<TKN>& i2 = *reinterpret_cast<mmTSA<TKN>*>(this->I2.get());
      i1.open(base+L1+".sfa", this->T1);
      i2.open(base+L2+".sfa", this->T2);
      assert(this->T1->size() == this->T2->size());
    }

   
    template<typename TKN>
    class imBitext : public Bitext<TKN>
    {
      sptr<imTtrack<char> > myTx;
      sptr<imTtrack<TKN> >  myT1;
      sptr<imTtrack<TKN> >  myT2;
      sptr<imTSA<TKN> >     myI1; 
      sptr<imTSA<TKN> >     myI2;
      static ThreadSafeCounter my_revision;
    public:
      size_t revision() const { return my_revision; }
      void open(string const base, string const L1, string L2);
      imBitext(sptr<TokenIndex> const& V1,
	       sptr<TokenIndex> const& V2,
	       size_t max_sample = 5000);
      imBitext(size_t max_sample = 5000);
      imBitext(imBitext const& other);
      
      // sptr<imBitext<TKN> > 
      // add(vector<TKN> const& s1, vector<TKN> const& s2, vector<ushort> & a);

      sptr<imBitext<TKN> > 
      add(vector<string> const& s1, 
	  vector<string> const& s2, 
	  vector<string> const& a) const;

    };

    template<typename TKN>
    ThreadSafeCounter 
    imBitext<TKN>::my_revision;

    template<typename TKN>
    imBitext<TKN>::
    imBitext(size_t max_sample)
    { 
      this->default_sample_size = max_sample;
      this->V1.reset(new TokenIndex());
      this->V2.reset(new TokenIndex());
      this->V1->setDynamic(true);
      this->V2->setDynamic(true);
      ++my_revision;
    }
    
    template<typename TKN>
    imBitext<TKN>::
    imBitext(sptr<TokenIndex> const& v1,
	     sptr<TokenIndex> const& v2,
	     size_t max_sample)
    { 
      this->default_sample_size = max_sample;
      this->V1 = v1;
      this->V2 = v2;
      this->V1->setDynamic(true);
      this->V2->setDynamic(true);
      ++my_revision;
    }
    

    template<typename TKN>
    imBitext<TKN>::
    imBitext(imBitext<TKN> const& other)
    { 
      this->myTx = other.myTx;
      this->myT1 = other.myT1;
      this->myT2 = other.myT2;
      this->myI1 = other.myI1;
      this->myI2 = other.myI2;
      this->Tx = this->myTx;
      this->T1 = this->myT1;
      this->T2 = this->myT2;
      this->I1 = this->myI1;
      this->I2 = this->myI2;
      this->V1 = other.V1;
      this->V2 = other.V2;
      this->default_sample_size = other.default_sample_size;
      this->num_workers = other.num_workers;
      ++my_revision;
    }
    
    template<typename TKN> class snt_adder;
    template<>             class snt_adder<L2R_Token<SimpleWordId> >;

    template<>     
    class snt_adder<L2R_Token<SimpleWordId> >
    {
      typedef L2R_Token<SimpleWordId> TKN;
      vector<string> const & snt;
      TokenIndex           & V;
      sptr<imTtrack<TKN> > & track;
      sptr<imTSA<TKN > >   & index;
    public:
      snt_adder(vector<string> const& s, TokenIndex& v, 
    		sptr<imTtrack<TKN> >& t, sptr<imTSA<TKN> >& i);
      
      void operator()();
    };

    // template<typename TKN>
    // class snt_adder
    // {
    //   vector<string> const & snt;
    //   TokenIndex           & V;
    //   sptr<imTtrack<TKN> > & track;
    //   sptr<imTSA<TKN > >   & index;
    // public:
    //   snt_adder(vector<string> const& s, TokenIndex& v, 
    //  		sptr<imTtrack<TKN> >& t, sptr<imTSA<TKN> >& i);

    //   template<typename T>
    //   void operator()();
    // };

    // // template<>
    // void
    // snt_adder<L2R_Token<SimpleWordId> >::
    // operator()();

    //  template<>
    //  void
    //  snt_adder<char>::
    //  operator()()
    //  {
    // 	vector<id_type> sids;
    // 	sids.reserve(snt.size());
    // 	BOOST_FOREACH(string const& s, snt)
    // 	  {
    // 	    sids.push_back(track ? track->size() : 0);
    // 	    istringstream buf(s);
    // 	    string w;
    // 	    vector<char> s;
    // 	    s.reserve(100);
    // 	    while (buf >> w) 
    // 	      s.push_back(vector<char>(V[w]));
    // 	    track = append(track,s);
    // 	  }
    // 	index.reset(new imTSA<char>(*index,track,sids,V.tsize()));
    // }
    
    // template<typename TKN>
    // snt_adder<TKN>::
    // snt_adder(vector<string> const& s, TokenIndex& v, 
    //  	      sptr<imTtrack<TKN> >& t, sptr<imTSA<TKN> >& i)
    //   : snt(s), V(v), track(t), index(i) 
    // {
    //   throw "Not implemented yet.";
    // }

    template<>
    sptr<imBitext<L2R_Token<SimpleWordId> > > 
    imBitext<L2R_Token<SimpleWordId> >::
    add(vector<string> const& s1, 
	vector<string> const& s2, 
	vector<string> const& aln) const;

    template<typename TKN>
    sptr<imBitext<TKN> > 
    imBitext<TKN>::
    add(vector<string> const& s1, 
	vector<string> const& s2, 
	vector<string> const& aln) const
    {
      throw "Not yet implemented";
    }
    // template<typename TKN>
    // sptr<imBitext<TKN> > 
    // imBitext<TKN>::
    // add(vector<TKN> const& s1, vector<TKN> const& s2, vector<ushort> & a)
    // {
    //   boost::lock_guard<boost::mutex> guard(this->lock);
    //   sptr<imBitext<TKN> > ret(new imBitext<TKN>());
    //   vector<id_type> sids(1,this->myT1.size()-1);
    //   ret->myT1 = add(this->myT1,s1);
    //   ret->myT2 = add(this->myT2,s2);
    //   size_t v1size = this->V1.tsize();
    //   size_t v2size = this->V2.tsize();
    //   BOOST_FOREACH(TKN const& t, s1) { if (t->id() >= v1size) v1size = t->id() + 1; }
    //   BOOST_FOREACH(TKN const& t, s2) { if (t->id() >= v2size) v2size = t->id() + 1; }
    //   ret->myI1.reset(new imTSA<TKN>(*this->I1,ret->myT1,sids,v1size));
    //   ret->myI2.reset(new imTSA<TKN>(*this->I2,ret->myT2,sids,v2size));
    //   ostringstream abuf; 
    //   BOOST_FOREACH(ushort x, a) binwrite(abuf,x);
    //   vector<char> foo(abuf.str().begin(),abuf.str().end());
    //   ret->myTx = add(this->myTx,foo);
    //   ret->T1 = ret->myT1;
    //   ret->T2 = ret->myT2;
    //   ret->Tx = ret->myTx;
    //   ret->I1 = ret->myI1;
    //   ret->I2 = ret->myI2;
    //   ret->V1 = this->V1;
    //   ret->V2 = this->V2; 
    //   return ret;
    // }


    // template<typename TKN>
    // imBitext<TKN>::
    // imBitext()
    //   : Bitext<TKN>(new imTtrack<TKN>(),
    // 		    new imTtrack<TKN>(),
    // 		    new imTtrack<char>(),
    // 		    new TokenIndex(),
    // 		    new TokenIndex(),
    // 		    new imTSA<TKN>(),
    // 		    new imTSA<TKN>())
    //   {}
    

    template<typename TKN>
    void
    imBitext<TKN>::
    open(string const base, string const L1, string L2)
    {
      mmTtrack<TKN>& t1 = *reinterpret_cast<mmTtrack<TKN>*>(this->T1.get());
      mmTtrack<TKN>& t2 = *reinterpret_cast<mmTtrack<TKN>*>(this->T2.get());
      mmTtrack<char>& tx = *reinterpret_cast<mmTtrack<char>*>(this->Tx.get());
      t1.open(base+L1+".mct");
      t2.open(base+L2+".mct");
      tx.open(base+L1+"-"+L2+".mam");
      this->V1->open(base+L1+".tdx"); this->V1->iniReverseIndex();
      this->V2->open(base+L2+".tdx"); this->V2->iniReverseIndex();
      mmTSA<TKN>& i1 = *reinterpret_cast<mmTSA<TKN>*>(this->I1.get());
      mmTSA<TKN>& i2 = *reinterpret_cast<mmTSA<TKN>*>(this->I2.get());
      i1.open(base+L1+".sfa", this->T1);
      i2.open(base+L2+".sfa", this->T2);
      assert(this->T1->size() == this->T2->size());
    }

    template<typename Token>
    bool
    Bitext<Token>::
    find_trg_phr_bounds
    (size_t const sid, 
     size_t const start, size_t const stop,
     size_t & s1, size_t & s2, size_t & e1, size_t & e2,
     int & po_fwd, int & po_bwd,
     vector<uchar>* core_alignment, bitvector* full_alignment, 
     bool const flip) const
    {
      // if (core_alignment) cout << "HAVE CORE ALIGNMENT" << endl;

      // a word on the core_alignment:
      // 
      // since fringe words ([s1,...,s2),[e1,..,e2) if s1 < s2, or e1
      // < e2, respectively) are be definition unaligned, we store
      // only the core alignment in *core_alignment it is up to the
      // calling function to shift alignment points over for start
      // positions of extracted phrases that start with a fringe word
      assert(T1);
      assert(T2);
      assert(Tx);

      size_t slen1,slen2;
      if (flip)
	{
	  slen1 = T2->sntLen(sid);
	  slen2 = T1->sntLen(sid);
	}
      else
	{
	  slen1 = T1->sntLen(sid);
	  slen2 = T2->sntLen(sid);
	}
      bitvector forbidden(slen2);
      if (full_alignment)
	{
	  if (slen1*slen2 > full_alignment->size())
	    full_alignment->resize(slen1*slen2*2);
	  full_alignment->reset();
	}
      size_t src,trg;
      size_t lft = forbidden.size();
      size_t rgt = 0;
      vector<vector<ushort> > aln1(slen1),aln2(slen2);
      char const* p = Tx->sntStart(sid);
      char const* x = Tx->sntEnd(sid);

      while (p < x)
	{
	  if (flip) { p = binread(p,trg); assert(p<x); p = binread(p,src); }
	  else      { p = binread(p,src); assert(p<x); p = binread(p,trg); }

	  UTIL_THROW_IF2((src >= slen1 || trg >= slen2),
			 "Alignment range error at sentence " << sid << "!\n" 
			 << src << "/" << slen1 << " " << 
			 trg << "/" << slen2);
	  
	  if (src < start || src >= stop) 
	    forbidden.set(trg);
	  else
	    {
	      lft = min(lft,trg);
	      rgt = max(rgt,trg);
	    }
	  if (core_alignment) 
	    {
	      aln1[src].push_back(trg);
	      aln2[trg].push_back(src);
	    }
	  if (full_alignment)
	    full_alignment->set(src*slen2 + trg);
	}
      
      for (size_t i = lft; i <= rgt; ++i)
	if (forbidden[i]) 
	  return false;
      
      s2 = lft;   for (s1 = s2; s1 && !forbidden[s1-1]; --s1);
      e1 = rgt+1; for (e2 = e1; e2 < forbidden.size() && !forbidden[e2]; ++e2);
      
      if (lft > rgt) return false;
      if (core_alignment) 
	{
	  core_alignment->clear();
	  for (size_t i = start; i < stop; ++i)
	    {
	      BOOST_FOREACH(ushort x, aln1[i])
		{
		  core_alignment->push_back(i-start);
		  core_alignment->push_back(x-lft);
		}
	    }
	  // now determine fwd and bwd phrase orientation
	  po_fwd = find_po_fwd(aln1,aln2,start,stop,s1,e2);
	  po_bwd = find_po_bwd(aln1,aln2,start,stop,s1,e2);
	}
      return lft <= rgt;
    }

    template<typename Token>
    void
    Bitext<Token>::
    prep(iter const& phrase, vector<float> const* const bias) const
    {
      prep2(phrase, this->default_sample_size,bias);
    }

    template<typename Token>
    sptr<pstats> 
    Bitext<Token>::
    prep2(iter const& phrase, size_t const max_sample, 
	  vector<float> const* const bias) const
    {
      boost::lock_guard<boost::mutex> guard(this->lock);
      if (!ag) 
	{
	  ag.reset(new agenda(*this));
	  if (this->num_workers > 1)
	    ag->add_workers(this->num_workers);
	}
      sptr<pstats> ret;
#if 1
      // use pcache only for plain sentence input
      if (StaticData::Instance().GetInputType() == SentenceInput && 
	  max_sample == this->default_sample_size && bias == NULL && 
	  phrase.approxOccurrenceCount() > m_pstats_cache_threshold)
      	{
	  // still need to test what a good caching threshold is
	  // is caching here the cause of the apparent memory leak in 
	  // confusion network decoding ???? No, it isn't. 
	  // That was because of naive, brute-force input path generation.
	  ::uint64_t pid = phrase.getPid();
	  pcache_t & cache(phrase.root == &(*this->I1) ? cache1 : cache2);
	  pcache_t::value_type entry(pid,sptr<pstats>());
	  pair<pcache_t::iterator,bool> foo;
	  foo = cache.insert(entry); 
	  if (foo.second) 
	    {
	      // cerr << "NEW FREQUENT PHRASE: "
	      // << phrase.str(V1.get()) << " " << phrase.approxOccurrenceCount()  
	      // << " at " << __FILE__ << ":" << __LINE__ << endl;
	      foo.first->second = ag->add_job(phrase, max_sample,NULL);
	      assert(foo.first->second);
	    }
	  assert(foo.first->second);
	  ret = foo.first->second;
	  assert(ret);
	}
      else 
#endif
	ret = ag->add_job(phrase, max_sample,bias);
      assert(ret);
      return ret;
    }

    // worker for scoring and sorting phrase table entries in parallel
    template<typename Token>
    class pstats2pplist
    {
      Ttrack<Token> const& m_other;
      sptr<pstats> m_pstats;
      vector<PhrasePair<Token> >& m_pplist;
      typename PhrasePair<Token>::Scorer const* m_scorer;
      PhrasePair<Token> m_pp;
      Token const* m_token;
      size_t m_len;
      ::uint64_t m_pid1;
      bool m_is_inverse;
    public:

      // CONSTRUCTOR
      pstats2pplist(typename TSA<Token>::tree_iterator const& m,
		    Ttrack<Token> const& other,
		    sptr<pstats> const& ps, 
		    vector<PhrasePair<Token> >& dest, 
		    typename PhrasePair<Token>::Scorer const* scorer)
	: m_other(other)
	, m_pstats(ps)
	, m_pplist(dest)
	, m_scorer(scorer)
	, m_token(m.getToken(0))
	, m_len(m.size())
	, m_pid1(m.getPid())
	, m_is_inverse(false)
      { }
      
      // WORKER
      void 
      operator()() 
      {
	// wait till all statistics have been collected
	boost::unique_lock<boost::mutex> lock(m_pstats->lock);
	while (m_pstats->in_progress)
	  m_pstats->ready.wait(lock);

	m_pp.init(m_pid1, m_is_inverse, m_token,m_len,m_pstats.get(),0); 

	// convert pstats entries to phrase pairs
	pstats::trg_map_t::iterator a;
	for (a = m_pstats->trg.begin(); a != m_pstats->trg.end(); ++a)
	  {
	    uint32_t sid,off,len;
	    parse_pid(a->first, sid, off, len);
	    m_pp.update(a->first, m_other.sntStart(sid)+off, len, a->second);
	    m_pp.good2 = max(uint32_t(m_pp.raw2 * float(m_pp.good1)/m_pp.raw1),m_pp.joint);
	    size_t J = m_pp.joint<<7; // hard coded threshold of 1/128
	    if (m_pp.good1 > J || m_pp.good2 > J) continue; 
	    if (m_scorer) 
	      {
		(*m_scorer)(m_pp);
	      }
	    m_pplist.push_back(m_pp);
	  }
	greater<PhrasePair<Token> > sorter;
	if (m_scorer) sort(m_pplist.begin(), m_pplist.end(),sorter);
      }
    };
    
    template<typename Token>
    void
    Bitext<Token>::
    lookup(vector<Token> const& snt, TSA<Token>& idx, 
	   vector<vector<sptr<vector<PhrasePair<Token> > > > >& dest,
	   vector<vector<typename ::uint64_t> >* pidmap,
	   typename PhrasePair<Token>::Scorer* scorer,
	   vector<float> const* const bias, bool multithread) const
    {
      // typedef vector<vector<sptr<vector<PhrasePair<Token> > > > > ret_t;
      
      dest.clear(); 
      dest.resize(snt.size());
      if (pidmap) { pidmap->clear(); pidmap->resize(snt.size()); }

      // collect statistics in parallel, then build PT entries as 
      // the sampling finishes
      bool fwd = &idx == I1.get();
      vector<boost::thread*> workers; // background threads doing the lookup
      pplist_cache_t& C = (fwd ? m_pplist_cache1 : m_pplist_cache2);
      if (C.capacity() < 100000) C.reserve(100000);
      for (size_t i = 0; i < snt.size(); ++i)
	{
	  dest[i].reserve(snt.size()-i);
	  typename TSA<Token>::tree_iterator m(&idx);
	  for (size_t k = i; k < snt.size() && m.extend(snt[k].id()); ++k)
	    {
	      ::uint64_t key = m.getPid();
	      if (pidmap) (*pidmap)[i].push_back(key);
	      sptr<vector<PhrasePair<Token> > > pp = C.get(key);
	      if (pp) 
		dest[i].push_back(pp);
	      else 
		{
		  pp.reset(new vector<PhrasePair<Token> >());
		  C.set(key,pp);
		  dest[i].push_back(pp);
		  sptr<pstats> x = prep2(m, this->default_sample_size,bias);
		  pstats2pplist<Token> w(m,*(fwd?T2:T1),x,*pp,scorer);
		  if (multithread) 
		    {
		      boost::thread* t = new boost::thread(w);
		      workers.push_back(t);
		    }
		  else w();
		}
	    }
	}
      for (size_t w = 0; w < workers.size(); ++w) 
	{
	  workers[w]->join(); 
	  delete workers[w];
	}
    }

    template<typename Token>
    sptr<pstats> 
    Bitext<Token>::
    lookup(iter const& phrase, vector<float> const* const bias) const
    {
      sptr<pstats> ret = prep2(phrase, this->default_sample_size, bias);
      assert(ret);
      boost::lock_guard<boost::mutex> guard(this->lock);
      if (this->num_workers <= 1)
	typename agenda::worker(*this->ag)();
      else 
	{
	  boost::unique_lock<boost::mutex> lock(ret->lock);
	  while (ret->in_progress)
	    ret->ready.wait(lock);
	}
      return ret;
    }

    template<typename Token>
    sptr<pstats> 
    Bitext<Token>::
    lookup(iter const& phrase, size_t const max_sample,
	   vector<float> const* const bias) const
    {
      sptr<pstats> ret = prep2(phrase, max_sample);
      boost::lock_guard<boost::mutex> guard(this->lock);
      if (this->num_workers <= 1)
	typename agenda::worker(*this->ag)();
      else 
	{
	  boost::unique_lock<boost::mutex> lock(ret->lock);
	  while (ret->in_progress)
	    ret->ready.wait(lock);
	}
      return ret;
    }

    template<typename Token>
    Bitext<Token>::
    agenda::
    ~agenda()
    {
      this->lock.lock();
      this->shutdown = true;
      this->lock.unlock();
      for (size_t i = 0; i < workers.size(); ++i)
	workers[i]->join();
    }
    
    template<typename Token>
    Bitext<Token>::
    agenda::
    agenda(Bitext<Token> const& thebitext)
      : shutdown(false), doomed(0), bt(thebitext)
    { }
    
    template<typename Token>
    bool
    Bitext<Token>::
    agenda::
    job::
    done() const
    { 
      return (max_samples && stats->good >= max_samples) || next == stop; 
    }

#if UG_BITEXT_TRACK_ACTIVE_THREADS
    template<typename TKN>
    ThreadSafeCounter 
    Bitext<TKN>::
    agenda::
    job::active;
#endif

    template<typename Token>
    void 
    expand(typename Bitext<Token>::iter const& m, 
	   Bitext<Token> const& bt, 
	   pstats const& ps, vector<PhrasePair<Token> >& dest)
    {
      bool fwd = m.root == bt.I1.get();
      dest.reserve(ps.trg.size());
      PhrasePair<Token> pp;
      pp.init(m.getPid(), !fwd, m.getToken(0), m.size(), &ps, 0);
      // cout << HERE << " " << toString(*(fwd ? bt.V1 : bt.V2), pp.start1,pp.len1) << endl;
      pstats::trg_map_t::const_iterator a;
      for (a = ps.trg.begin(); a != ps.trg.end(); ++a)
	{
	  uint32_t sid,off,len;
	  parse_pid(a->first, sid, off, len);
	  pp.update(a->first, (fwd ? bt.T2 : bt.T1)->sntStart(sid)+off, 
		    len, a->second);
	  dest.push_back(pp);
	}
#if 0
      typename PhrasePair<Token>::SortByTargetIdSeq sorter;
      sort(dest.begin(), dest.end(),sorter);
      BOOST_FOREACH(PhrasePair<Token> const& p, dest)
	cout << toString (*(fwd ? bt.V1 : bt.V2),p.start1,p.len1) << " ::: " 
	     << toString (*(fwd ? bt.V2 : bt.V1),p.start2,p.len2) << " " 
	     << p.joint << endl;
#endif
    }
    
  } // end of namespace bitext
} // end of namespace moses
#endif

