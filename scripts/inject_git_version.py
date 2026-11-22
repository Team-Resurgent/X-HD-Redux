Import("env")
import datetime
import os
import sys

sys.path.insert(0, os.path.join(env["PROJECT_DIR"], "scripts"))
from git_state import firmware_version

# "<commit>" for a clean build, "<commit>-dirty-<diff hash>" otherwise -- see git_state.py
# for what counts as a build input.
git_hash = firmware_version(env["PROJECT_DIR"])

env.Append(CPPDEFINES=[("GIT_HASH", '\\"%s\\"' % git_hash)])

# Space-free (unlike "%s %s" % (__DATE__, __TIME__), e.g. "Sep 17 2026") so debug.c's "v"
# command can put it in a single whitespace-tokenized field of its compact [INFO:RAW]
# payload -- see that command's own comment. UTC, so the binary doesn't carry the builder's
# time zone.
build_datetime = datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%S")
env.Append(CPPDEFINES=[("BUILD_DATETIME", '\\"%s\\"' % build_datetime)])
