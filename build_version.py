# Adapted from https://gitlab.com/pvojnisek/buildnumber-for-platformio/-/tree/master. 
# Thank you Peter Vojnisek!
import time
import datetime
import subprocess
tm = datetime.datetime.today()

FILENAME_BUILDNO = 'build_nb'
FILENAME_VERSION_H = 'include/version.h'

version = 'v0.1.' + str(tm.year)[-2:]+('0'+str(tm.month))[-2:]+('0'+str(tm.day))[-2:] + '_'

build_no = 0
try:
    with open(FILENAME_BUILDNO) as f:
        build_no = int(f.readline()) + 1
except:
    print('Starting build number from 1..')
    build_no = 1
with open(FILENAME_BUILDNO, 'w+') as f:
    f.write(str(build_no))
    print('Build number: {}'.format(build_no))

build_date = datetime.datetime.now()

try:
    git_branch = subprocess.check_output(["git", "rev-parse", "--abbrev-ref", "HEAD"]).strip().decode("utf-8")
    git_commit_date = subprocess.check_output(["git", "log", "-1", "--format=%cd", "--date=short"]).strip().decode("utf-8")
    git_hash = subprocess.check_output(["git", "rev-parse", "HEAD"]).strip().decode("utf-8")
    git_description = subprocess.check_output(["git", "describe", "--always", "--abbrev", "--dirty"]).strip().decode("utf-8")
    git_repo_url = subprocess.check_output(["git", "remote", "get-url", "origin"]).strip().decode("utf-8")
except:
    print('Could not get git version info')
    git_description = git_branch = git_commit_date = git_hash = git_repo_url = "?"

hf = f"""
#ifndef VERSION_H_GENERATED
#define VERSION_H_GENERATED

// This file was generated {time.asctime(time.localtime())} by the build_version.py script

#ifndef BUILD_NUMBER
  #define BUILD_NUMBER {build_no}
#endif
#ifndef BUILD_DATE
  #define BUILD_DATE "{build_date}"
#endif
#ifndef GIT_REPO_URL
  #define GIT_REPO_URL "{git_repo_url}"
#endif
#ifndef GIT_BRANCH
  #define GIT_BRANCH "{git_branch}"
#endif
#ifndef GIT_HASH
  #define GIT_HASH "{git_hash}"
#endif
#ifndef GIT_COMMIT_DATE
  #define GIT_COMMIT_DATE "{git_commit_date}"
#endif
#ifndef GIT_DESCRIPTION
  #define GIT_DESCRIPTION "{git_description}"
#endif

#define BUILD_DETAILS "Build #{build_no} ({build_date}): {git_branch}/{git_description} (committed {git_commit_date}) at {git_repo_url}"

#endif
"""

with open(FILENAME_VERSION_H, 'w+') as f:
    f.write(hf)
