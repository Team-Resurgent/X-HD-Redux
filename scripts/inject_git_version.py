Import("env")
import datetime
import hashlib
import subprocess

def git_output(args, default):
    try:
        return subprocess.check_output(
            ["git"] + args, stderr=subprocess.DEVNULL
        ).decode().strip()
    except Exception:
        return default

git_hash = git_output(["rev-parse", "--short=8", "HEAD"], "unknown")
dirty = git_output(["status", "--porcelain"], "")
if dirty:
    # A bare "-dirty" suffix can't tell two different uncommitted builds apart -- every
    # flash during a work session shows the identical version string regardless of what
    # actually changed, indistinguishable without comparing CRC/size out of band. Fold in
    # a content hash of the actual tracked-file diff so each distinct uncommitted state
    # gets its own suffix. Doesn't cover a brand-new untracked source file matching
    # build_src_filter's globs; that still shows as plain "-dirty" with no diff content
    # to hash.
    diff_content = git_output(["diff", "HEAD"], "")
    if diff_content:
        diff_hash = hashlib.sha1(diff_content.encode("utf-8", "replace")).hexdigest()[:8]
        git_hash += "-dirty-%s" % diff_hash
    else:
        git_hash += "-dirty"

env.Append(CPPDEFINES=[("GIT_HASH", '\\"%s\\"' % git_hash)])

# Space-free (unlike "%s %s" % (__DATE__, __TIME__), e.g. "Sep 17 2026") so debug.c's "v"
# command can put it in a single whitespace-tokenized field of its compact [INFO:RAW]
# payload -- see that command's own comment. UTC, so the binary doesn't carry the builder's
# time zone.
build_datetime = datetime.datetime.now(datetime.timezone.utc).strftime("%Y-%m-%dT%H:%M:%S")
env.Append(CPPDEFINES=[("BUILD_DATETIME", '\\"%s\\"' % build_datetime)])
