"""Which commit a firmware build came from, and whether the build inputs differ from it.

Shared by scripts/inject_git_version.py (the version string inside the firmware) and
scripts/combine_firmware.py (the name of the archived image in bin/), so the two always agree.
"""
import hashlib
import os
import subprocess

# Everything a firmware build reads. A change anywhere else -- documentation, the host tests,
# the sweep database, or the images in bin/ the build itself writes -- cannot alter the
# firmware, so it must not mark it "dirty".
FIRMWARE_INPUTS = [
    "platformio.ini",
    "boards",
    "src",
    "scripts/git_state.py",
    "scripts/inject_git_version.py",
    "scripts/hal_overrides.py",
    "scripts/lto.py",
    "scripts/combine_firmware.py",
]


def _git(project_dir, *args):
    try:
        return subprocess.check_output(
            ["git", *args], cwd=project_dir, stderr=subprocess.DEVNULL
        ).decode(errors="replace").strip()
    except (OSError, subprocess.CalledProcessError):
        return None


def firmware_version(project_dir):
    """The build's identity: "<8-char commit>" when the build inputs match that commit,
    "<commit>-dirty-<8-char hash of the diff>" when tracked inputs differ (each distinct
    uncommitted state gets its own suffix, so two work-in-progress images are never
    mistaken for each other), or "<commit>-dirty" when only untracked files are present.
    "unknown" outside a git checkout.

    XHD_FIRMWARE_VERSION, when set, is used as is: scripts/make_release.py builds from a
    plain copy of the sources (no git history) and passes the originating commit this way."""
    forced = os.environ.get("XHD_FIRMWARE_VERSION")
    if forced:
        return forced
    commit = _git(project_dir, "rev-parse", "--short=8", "HEAD")
    if not commit:
        return "unknown"
    if not _git(project_dir, "status", "--porcelain", "--", *FIRMWARE_INPUTS):
        return commit
    diff = _git(project_dir, "diff", "HEAD", "--", *FIRMWARE_INPUTS)
    if diff:
        return "%s-dirty-%s" % (commit, hashlib.sha1(diff.encode("utf-8", "replace")).hexdigest()[:8])
    return commit + "-dirty"
