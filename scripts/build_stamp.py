"""Stamp the firmware with the commit it was built from.

There is no way to ask a flashed panel what it is running - it publishes
"online" and nothing else - so "is the display up to date?" could only be
answered by comparing file timestamps and hoping. This injects the short SHA
(plus "-dirty" for uncommitted work) as FW_GIT_SHA, which main.cpp publishes
retained to TOPIC_DEVICE_INFO on connect, where `bridge.js status` reads it.

The build *time* deliberately comes from __DATE__/__TIME__ in main.cpp rather
than from here. A timestamp generated on every invocation would change the
define on every invocation, and SCons would rebuild and relink a file that
nothing had actually changed - turning every no-op build into ten seconds of
work. The SHA only moves when the tree does.

Consequence worth knowing: with uncommitted changes the SHA reads "-dirty" but
the time only advances when main.cpp itself recompiles, so it dates the last
compile of that file, not the last build of the project.
"""

import subprocess

Import("env")


def git(*args):
    return (
        subprocess.check_output(["git", *args], stderr=subprocess.DEVNULL)
        .decode()
        .strip()
    )


def build_stamp():
    try:
        sha = git("rev-parse", "--short", "HEAD")
    except Exception:
        # Not a checkout, or no git on PATH. A stamp that says so is more use
        # than a failed build.
        return "nogit"
    try:
        if git("status", "--porcelain"):
            sha += "-dirty"
    except Exception:
        pass
    return sha


stamp = build_stamp()
print(f"firmware build stamp: {stamp}")
env.Append(CPPDEFINES=[("FW_GIT_SHA", env.StringifyMacro(stamp))])
