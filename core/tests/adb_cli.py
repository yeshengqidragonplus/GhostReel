"""Runs the Android build of `alphaclip` on a connected device as if it were a local command.

This lets the same host-side test scripts (test_vs_ffmpeg.py, fuzz_core.py) exercise the arm64
binary on a real phone: pass `core/tests/adb_cli.cmd` as --cli.

For every call it
  * pushes each argument that is an existing local file to the device (the clip),
  * replaces `--out <path>` by a file on the device and pulls it back to <path> afterwards,
  * runs the command with `adb shell`, forwards stdout / stderr, and returns its exit code.

Only files under one device folder are touched: AC_DEVICE_DIR (default /data/local/tmp/ghostreel_m1),
which must already contain the `alphaclip` binary. Environment: ADB (path to adb), AC_DEVICE_DIR,
AC_TASKSET (optional hex CPU mask, e.g. c0 for the Cortex-X1 cores), AC_DEVICE_ENV (optional
"NAME=value ..." put in front of the command, e.g. LD_LIBRARY_PATH / ASAN_OPTIONS for sanitizer builds).
"""
import os
import shlex
import subprocess
import sys

ADB = os.environ.get("ADB", r"E:\AndroidSDK\platform-tools\adb.exe")
DEV = os.environ.get("AC_DEVICE_DIR", "/data/local/tmp/ghostreel_m1")
TASKSET = os.environ.get("AC_TASKSET", "")
# Extra environment for the device command, e.g. "LD_LIBRARY_PATH=/data/local/tmp/x ASAN_OPTIONS=..."
DEV_ENV = os.environ.get("AC_DEVICE_ENV", "")


def adb(*args, capture=True):
    return subprocess.run([ADB, *args], capture_output=capture)


def main():
    if not DEV.startswith("/"):
        # Git Bash rewrites /data/... in environment variables to C:/Program Files/Git/data/...
        sys.stderr.write(f"adb_cli: AC_DEVICE_DIR/AC_DEVICE_ENV were mangled ({DEV!r}); "
                         "set MSYS_NO_PATHCONV=1 in Git Bash\n")
        return 125
    argv = sys.argv[1:]
    out_local = None
    remote_args = []
    pushed = 0
    i = 0
    while i < len(argv):
        a = argv[i]
        if a == "--out" and i + 1 < len(argv):
            out_local = argv[i + 1]
            remote_args += ["--out", f"{DEV}/out.raw"]
            i += 2
            continue
        if os.path.isfile(a):
            remote = f"{DEV}/in{pushed}.webm"
            pushed += 1
            r = adb("push", a, remote)
            if r.returncode != 0:
                sys.stderr.buffer.write(b"adb_cli: push failed: " + r.stderr)
                return 125
            remote_args.append(remote)
        else:
            remote_args.append(a)  # options, numbers, or a path that does not exist (on purpose)
        i += 1

    prefix = f"taskset {TASKSET} " if TASKSET else ""
    env = f"{DEV_ENV} " if DEV_ENV else ""
    cmd = f"cd {DEV} && rm -f out.raw && {env}{prefix}./alphaclip " + " ".join(shlex.quote(a) for a in remote_args)
    r = adb("shell", cmd)
    sys.stdout.buffer.write(r.stdout)
    sys.stderr.buffer.write(r.stderr)
    if out_local is not None:
        pull = adb("pull", f"{DEV}/out.raw", out_local)
        if pull.returncode != 0:
            # The command failed before it created the file: leave an empty one, like a failed run would.
            open(out_local, "wb").close()
        adb("shell", f"rm -f {DEV}/out.raw")
    return r.returncode


if __name__ == "__main__":
    sys.exit(main())
