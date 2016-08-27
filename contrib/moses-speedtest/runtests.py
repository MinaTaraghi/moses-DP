"""Given a config file, runs tests"""
import os
import subprocess
import time
from argparse import ArgumentParser
from testsuite_common import processLogLine

def parse_cmd():
    """Parse the command line arguments"""
    description = "A python based speedtest suite for moses."
    parser = ArgumentParser(description=description)
    parser.add_argument("-c", "--configfile", action="store",\
                dest="configfile", required=True,\
                help="Specify test config file")
    parser.add_argument("-s", "--singletest", action="store",\
                dest="singletestdir", default=None,\
                help="Single test name directory. Specify directory name,\
                not full path!")
    parser.add_argument("-r", "--revision", action="store",\
                dest="revision", default=None,\
                help="Specify a specific revison for the test.")
    parser.add_argument("-b", "--branch", action="store",\
                dest="branch", default=None,\
                help="Specify a branch for the test.")

    arguments = parser.parse_args()
    return arguments

def repoinit(testconfig):
    """Determines revision and sets up the repo."""
    revision = ''
    #Update the repo
    os.chdir(testconfig.repo)
    #Checkout specific branch, else maintain main branch
    if testconfig.branch != 'master':
        subprocess.call(['git', 'checkout', testconfig.branch])
        rev, _ = subprocess.Popen(['git', 'rev-parse', 'HEAD'],\
            stdout=subprocess.PIPE, stderr=subprocess.PIPE).communicate()
        revision = str(rev).replace("\\n'", '').replace("b'", '')
    else:
        subprocess.call(['git checkout master'], shell=True)

    #Check a specific revision. Else checkout master.
    if testconfig.revision:
        subprocess.call(['git', 'checkout', testconfig.revision])
        revision = testconfig.revision
    elif testconfig.branch == 'master':
        subprocess.call(['git pull'], shell=True)
        rev, _ = subprocess.Popen(['git rev-parse HEAD'], stdout=subprocess.PIPE,\
            stderr=subprocess.PIPE, shell=True).communicate()
        revision = str(rev).replace("\\n'", '').replace("b'", '')
    
    return revision

class Configuration:
    """A simple class to hold all of the configuration constatns"""
    def __init__(self, repo, drop_caches, tests, testlogs, basebranch, baserev):
        self.repo = repo
        self.drop_caches = drop_caches
        self.tests = tests
        self.testlogs = testlogs
        self.basebranch = basebranch
        self.baserev = baserev
        self.singletest = None
        self.revision = None
        self.branch = 'master' # Default branch

    def additional_args(self, singletest, revision, branch):
        """Additional configuration from command line arguments"""
        self.singletest = singletest
        if revision is not None:
            self.revision = revision
        if branch is not None:
            self.branch = branch

    def set_revision(self, revision):
        """Sets the current revision that is being tested"""
        self.revision = revision


class Test:
    """A simple class to contain all information about tests"""
    def __init__(self, name, command, ldopts, permutations):
        self.name = name
        self.command = command
        self.ldopts = ldopts.replace(' ', '').split(',') #Not tested yet
        self.permutations = permutations

def parse_configfile(conffile, testdir, moses_repo):
    """Parses the config file"""
    command, ldopts = '', ''
    permutations = []
    fileopen = open(conffile, 'r')
    for line in fileopen:
        line = line.split('#')[0] # Discard comments
        if line == '' or line == '\n':
            continue # Discard lines with comments only and empty lines
        opt, args = line.split(' ', 1) # Get arguments

        if opt == 'Command:':
            command = args.replace('\n', '')
            command = moses_repo + '/bin/' + command
        elif opt == 'LDPRE:':
            ldopts = args.replace('\n', '')
        elif opt == 'Variants:':
            permutations = args.replace('\n', '').replace(' ', '').split(',')
        else:
            raise ValueError('Unrecognized option ' + opt)
    #We use the testdir as the name.
    testcase = Test(testdir, command, ldopts, permutations)
    fileopen.close()
    return testcase

def parse_testconfig(conffile):
    """Parses the config file for the whole testsuite."""
    repo_path, drop_caches, tests_dir, testlog_dir = '', '', '', ''
    basebranch, baserev = '', ''
    fileopen = open(conffile, 'r')
    for line in fileopen:
        line = line.split('#')[0] # Discard comments
        if line == '' or line == '\n':
            continue # Discard lines with comments only and empty lines
        opt, args = line.split(' ', 1) # Get arguments
        if opt == 'MOSES_REPO_PATH:':
            repo_path = args.replace('\n', '')
        elif opt == 'DROP_CACHES_COMM:':
            drop_caches = args.replace('\n', '')
        elif opt == 'TEST_DIR:':
            tests_dir = args.replace('\n', '')
        elif opt == 'TEST_LOG_DIR:':
            testlog_dir = args.replace('\n', '')
        elif opt == 'BASEBRANCH:':
            basebranch = args.replace('\n', '')
        elif opt == 'BASEREV:':
            baserev = args.replace('\n', '')
        else:
            raise ValueError('Unrecognized option ' + opt)
    config = Configuration(repo_path, drop_caches, tests_dir, testlog_dir,\
    basebranch, baserev)
    fileopen.close()
    return config

def get_config():
    """Builds the config object with all necessary attributes"""
    args = parse_cmd()
    config = parse_testconfig(args.configfile)
    config.additional_args(args.singletestdir, args.revision, args.branch)
    revision = repoinit(config)
    config.set_revision(revision)
    return config

def check_for_basever(testlogfile, basebranch):
    """Checks if the base revision is present in the testlogs"""
    filetoopen = open(testlogfile, 'r')
    for line in filetoopen:
        templine = processLogLine(line)
        if templine.branch == basebranch:
            return True
    return False

def split_time(filename):
    """Splits the output of the time function into seperate parts.
    We will write time to file, because many programs output to
    stderr which makes it difficult to get only the exact results we need."""
    timefile = open(filename, 'r')
    realtime = float(timefile.readline().replace('\n', '').split()[1])
    usertime = float(timefile.readline().replace('\n', '').split()[1])
    systime = float(timefile.readline().replace('\n', '').split()[1])
    timefile.close()

    return (realtime, usertime, systime)


def write_log(time_file, logname, config):
    """Writes to a logfile"""
    log_write = open(config.testlogs + '/' + logname, 'a') # Open logfile
    date_run = time.strftime("%d.%m.%Y %H:%M:%S") # Get the time of the test
    realtime, usertime, systime = split_time(time_file) # Get the times in a nice form

    # Append everything to a log file.
    writestr = date_run + " " + config.revision + " Testname: " + logname +\
    " RealTime: " + str(realtime) + " UserTime: " + str(usertime) +\
    " SystemTime: " + str(systime) + " Branch: " + config.branch +'\n'
    log_write.write(writestr)
    log_write.close()


def execute_tests(testcase, cur_directory, config):
    """Executes timed tests based on the config file"""
    #Figure out the order of which tests must be executed.
    #Change to the current test directory
    os.chdir(config.tests + '/' + cur_directory)
    #Clear caches
    subprocess.call(['sync'], shell=True)
    subprocess.call([config.drop_caches], shell=True)
    #Perform vanilla test and if a cached test exists - as well
    print(testcase.name)
    if 'vanilla' in testcase.permutations:
        print(testcase.command)
        subprocess.Popen(['time -p -o /tmp/time_moses_tests ' + testcase.command], stdout=None,\
         stderr=subprocess.PIPE, shell=True).communicate()
        write_log('/tmp/time_moses_tests', testcase.name + '_vanilla', config)
        if 'cached' in testcase.permutations:
            subprocess.Popen(['time -p -o /tmp/time_moses_tests ' + testcase.command], stdout=None,\
            stderr=None, shell=True).communicate()
            write_log('/tmp/time_moses_tests', testcase.name + '_vanilla_cached', config)
    
    #Now perform LD_PRELOAD tests
    if 'ldpre' in testcase.permutations:
        for opt in testcase.ldopts:
            #Clear caches
            subprocess.call(['sync'], shell=True)
            subprocess.call([config.drop_caches], shell=True)

            #test
            subprocess.Popen(['LD_PRELOAD ' + opt + ' time -p -o /tmp/time_moses_tests ' + testcase.command], stdout=None,\
            stderr=None, shell=True).communicate()
            write_log('/tmp/time_moses_tests', testcase.name + '_ldpre_' + opt, config)
            if 'cached' in testcase.permutations:
                subprocess.Popen(['LD_PRELOAD ' + opt + ' time -p -o /tmp/time_moses_tests ' + testcase.command], stdout=None,\
                stderr=None, shell=True).communicate()
                write_log('/tmp/time_moses_tests', testcase.name + '_ldpre_' +opt +'_cached', config)

# Go through all the test directories and executes tests
if __name__ == '__main__':
    CONFIG = get_config()
    ALL_DIR = os.listdir(CONFIG.tests)

    #We should first check if any of the tests is run for the first time.
    #If some of them are run for the first time we should first get their
    #time with the base version (usually the previous release)
    FIRSTTIME = []
    TESTLOGS = []
    #Strip filenames of test underscores
    for listline in os.listdir(CONFIG.testlogs):
        listline = listline.replace('_vanilla', '')
        listline = listline.replace('_cached', '')
        listline = listline.replace('_ldpre', '')
        TESTLOGS.append(listline)
    for directory in ALL_DIR:
        if directory not in TESTLOGS:
            FIRSTTIME.append(directory)

    #Sometimes even though we have the log files, we will need to rerun them
    #Against a base version, because we require a different baseversion (for
    #example when a new version of Moses is released.) Therefore we should
    #Check if the version of Moses that we have as a base version is in all
    #of the log files.

    for logfile in os.listdir(CONFIG.testlogs):
        logfile_name = CONFIG.testlogs + '/' + logfile
        if not check_for_basever(logfile_name, CONFIG.basebranch):
            logfile = logfile.replace('_vanilla', '')
            logfile = logfile.replace('_cached', '')
            logfile = logfile.replace('_ldpre', '')
            FIRSTTIME.append(logfile)
    FIRSTTIME = list(set(FIRSTTIME)) #Deduplicate

    if FIRSTTIME != []:
        #Create a new configuration for base version tests:
        BASECONFIG = Configuration(CONFIG.repo, CONFIG.drop_caches,\
            CONFIG.tests, CONFIG.testlogs, CONFIG.basebranch,\
            CONFIG.baserev)
        BASECONFIG.additional_args(None, CONFIG.baserev, CONFIG.basebranch)
        #Set up the repository and get its revision:
        REVISION = repoinit(BASECONFIG)
        BASECONFIG.set_revision(REVISION)
        #Build
        os.chdir(BASECONFIG.repo)
        subprocess.call(['./previous.sh'], shell=True)

        #Perform tests
        for directory in FIRSTTIME:
            cur_testcase = parse_configfile(BASECONFIG.tests + '/' + directory +\
            '/config', directory, BASECONFIG.repo)
            execute_tests(cur_testcase, directory, BASECONFIG)

        #Reset back the repository to the normal configuration
        repoinit(CONFIG)

    #Builds moses
    os.chdir(CONFIG.repo)
    subprocess.call(['./previous.sh'], shell=True)

    if CONFIG.singletest:
        TESTCASE = parse_configfile(CONFIG.tests + '/' +\
            CONFIG.singletest + '/config', CONFIG.singletest, CONFIG.repo)
        execute_tests(TESTCASE, CONFIG.singletest, CONFIG)
    else:
        for directory in ALL_DIR:
            cur_testcase = parse_configfile(CONFIG.tests + '/' + directory +\
            '/config', directory, CONFIG.repo)
            execute_tests(cur_testcase, directory, CONFIG)
