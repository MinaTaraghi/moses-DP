#include <math.h>
#include <vector>
#include "TF1.h"
#include "moses/ScoreComponentCollection.h"
#include "moses/TargetPhrase.h"
#include "moses/PP/PhraseProperty.h"
#include "moses/PP/TopicsPhraseProperty.h"
#include "moses/InputPath.h"


using namespace std;

namespace Moses
{
TF1::TF1(const std::string &line)
  :StatelessFeatureFunction(1,line)
{
  ReadParameters();
}

void TF1::EvaluateInIsolation(const Phrase &source
    , const TargetPhrase &targetPhrase
    , ScoreComponentCollection &scoreBreakdown
    , ScoreComponentCollection &estimatedFutureScore) const
{
  //  targetPhrase.SetRuleSource(source);
  const PhraseProperty *property = targetPhrase.GetProperty("Topics");
  //std::cout << *property;
  if (property == NULL) {
      //std::cout<<"Null Property111111!"<<endl;
    return;
  }
}
/*
//  const TopicsPhraseProperty *tProp = static_cast<const TopicsPhraseProperty*>(property);
//  std::cout<<"Here1"<<std::endl;
//  std::cout << *tProp;
//  scoreBreakdown.PlusEquals(this,0.0001);
//  std::cout<<"Here2"<<std::endl;
  // dense scores
//  vector<float> newScores(m_numScoreComponents);
//  newScores[0] = 1.5;
//  newScores[1] = 0.3;
//  scoreBreakdown.PlusEquals(this, newScores);

  // sparse scores
  //scoreBreakdown.PlusEquals(this, "sparse-name", 2.4);

}*/

void TF1::EvaluateWithSourceContext(const InputType &input
    , const InputPath &inputPath
    , const TargetPhrase &targetPhrase
    , const StackVec *stackVec
    , ScoreComponentCollection &scoreBreakdown
    , ScoreComponentCollection *estimatedFutureScore) const
{
    float score;
   // std::cout<<endl<<targetPhrase<<endl;
  const PhraseProperty *property = targetPhrase.GetProperty("Topics");
  if (property == NULL) {
     // std::cout<<"Null Property!"<<endl;
    return;
  }

  const TopicsPhraseProperty *tProp = static_cast<const TopicsPhraseProperty*>(property);
  std::vector<float> phrase_topics= tProp->GetTopics();
  int numT=tProp->GetnumTopics();
 // std::cout<<"Number of Topics Detected:"<<numT<<std::endl;
  std::vector<float> sentence_topics;
  //=new vector<float>*;


  //const Sentence& sentence = static_cast<const Sentence&>(input);
  //const AlignmentInfo &alignment = targetPhrase.GetAlignTerm();

  //const bool use_topicid_prob = sentence.GetUseTopicIdAndProb();

        const vector<string> &topicid_prob = *(input.GetTopicIdAndProb());

            for (size_t i=0; 2*i+1 < topicid_prob.size(); i++)
            {
                sentence_topics.push_back(atof((topicid_prob[2*i+1]).c_str()));
            }
            //std::cout<<"Here3"<<std::endl;

    //************Computing Cosine Similarity************
     float norm_a,norm_b,ab,aj,bj;
     norm_a=0;
     norm_b=0;
     ab=0;
    for (int j=0;j<numT;j++)
    {
        aj=sentence_topics[j];
        bj=phrase_topics[j];
        norm_a+=aj*aj;
        norm_b+=bj*bj;
        ab+=aj*bj;
    }
    norm_a=sqrt(norm_a);
    norm_b=sqrt(norm_b);

    score=ab/(norm_a*norm_b);
    //std::cout<<score<<std::endl;
    scoreBreakdown.PlusEquals(this,score);

  /*if (targetPhrase.GetNumNonTerminals()) {
    vector<float> newScores(m_numScoreComponents);
    newScores[0] = - std::numeric_limits<float>::infinity();
    scoreBreakdown.PlusEquals(this, newScores);
  }*/
}

/*void SkeletonStatelessFF::EvaluateTranslationOptionListWithSourceContext(const InputType &input

    , const TranslationOptionList &translationOptionList) const
{}

void SkeletonStatelessFF::EvaluateWhenApplied(const Hypothesis& hypo,
    ScoreComponentCollection* accumulator) const
{}

void SkeletonStatelessFF::EvaluateWhenApplied(const ChartHypothesis &hypo,
    ScoreComponentCollection* accumulator) const
{}*/

void TF1::SetParameter(const std::string& key, const std::string& value)
{
  if (key == "arg") {
    // set value here
  } else {
    StatelessFeatureFunction::SetParameter(key, value);
  }
}
  /*bool TF1::IsUseable(const FactorMask &mask) const
{
  return true;
}*/

}

