"""Smoke-test the Linux CI observer with deliberately signalled stand-alone fixtures."""
import os
from pathlib import Path
import re
import runpy
import shutil
import signal
import subprocess
import sys
import tempfile
import threading


ROOT = Path(__file__).resolve().parents[2]
WRAPPER = ROOT / "tools/run-native-ci-test.py"
SENTINEL = "fixture-private-value-do-not-record"


def fixture(mode):
    print("fixture started", flush=True)
    if mode == "failure":
        return 7
    name, _, handler = mode.partition("-")
    if name in ("SIGTRAP", "SIGINT"):
        signum = signal.Signals[name]
        received = []
        signal.signal(signum, (lambda sig, frame: received.append(sig)) if handler else signal.SIG_DFL)
        signal.raise_signal(signum)
        return 23 if received == [signum] else 0  # A swallowed signal would falsely pass.
    if mode == "fork-exec":
        code = subprocess.run([sys.executable, __file__, "--fixture", "failure"]).returncode
        return 17 if code == 7 else 1
    # More threads than the report cap; none exit before the signal.
    ready = threading.Barrier(35)

    def park():
        ready.wait()
        threading.Event().wait()

    for _ in range(34):
        threading.Thread(target=park, daemon=True).start()
    ready.wait()
    signal.raise_signal(signal.SIGSEGV)
    return 1


def check(probe):
    def run(command, status, *, short=False, infrastructure=False, **overrides):
        env = {**os.environ, "ELIO_TEST_WATCHDOG_SECS": "1",
               "ELIO_TEST_WATCHDOG_DISABLE": "0", "OBSERVER_TEST_SECRET": SENTINEL,
               **overrides}
        launcher = [__file__, "--short-supervisor"] if short else [WRAPPER]
        result = subprocess.run([sys.executable, *launcher, *command], env=env,
                                stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                text=True, timeout=30)
        summary = rf"^\[native-ci\] (?:termination_signal=\d+ )?exit_status={status}$"
        if infrastructure:
            summary = r"^\[native-ci\] .*CI failure=125$"
        if result.returncode != status or not re.search(summary, result.stdout, re.MULTILINE):
            raise AssertionError(result.stdout)
        assert SENTINEL not in result.stdout
        return result.stdout

    unavailable = "[native-ci] stacks unavailable"
    self_cmd = [sys.executable, str(Path(__file__).resolve()), "--fixture"]
    with tempfile.TemporaryDirectory() as directory:
        (Path(directory) / "gdb").symlink_to("/bin/false")
        fallback = run([*self_cmd, "failure"], 7, PATH=directory + os.pathsep + os.environ["PATH"])
        assert unavailable in fallback and fallback.count("fixture started") == 1

    normal = run([probe, "disabled"], 0, ELIO_TEST_WATCHDOG_DISABLE="1")
    if unavailable in normal:
        print("Native stack smoke: child tracing unavailable; only direct execution verified.")
        return
    assert "[native-ci] signal=" not in normal
    failure = run([*self_cmd, "failure"], 7)
    assert failure.count("fixture started") == 1 and "[native-ci] signal=" not in failure
    forked = run([*self_cmd, "fork-exec"], 17)
    assert forked.count("fixture started") == 2 and "[native-ci] signal=" not in forked
    for signum in (signal.SIGTRAP, signal.SIGINT):
        delivered = run([*self_cmd, signum.name], 128 + signum)
        assert f"[native-ci] signal={signum.name}" in delivered
        handled = run([*self_cmd, signum.name + "-handled"], 23)
        assert f"[native-ci] signal={signum.name}" in handled
    for mode, status in (("wait", 86), ("fallback", 124)):
        timeout = run([probe, mode], status)
        assert "[native-ci] signal=SIGALRM" in timeout and "wall-clock timeout reached" in timeout
        assert " 'main'" in timeout, timeout
    # Delay only evidence formatting inside GDB, never the native fixture.
    with tempfile.TemporaryDirectory() as directory:
        root = Path(directory)
        shim = root / "gdb"
        shim.write_text('#!/bin/sh\nexec "$TEST_GDB" -iex "source $TEST_GDB_SCRIPT" "$@"\n')
        shim.chmod(0o700)
        script = root / "stall.py"
        env = dict(TEST_GDB=shutil.which("gdb"), TEST_GDB_SCRIPT=str(script),
                   PATH=directory + os.pathsep + os.environ["PATH"])
        script.write_text("""import gdb, os, signal, time
def stall():
    if os.environ.get("TEST_LOSE_OBSERVER"):
        os._exit(0)
    if os.environ.get("TEST_STOP_OBSERVER"):
        os.kill(os.getpid(), signal.SIGSTOP)
    if os.environ.get("TEST_IGNORE_INTERRUPT"):
        signal.signal(signal.SIGUSR1, signal.SIG_IGN)
    while True:
        time.sleep(0.05)
gdb.newest_frame = stall
""")
        for mode, status in (("SIGTRAP-handled", 23), ("SIGINT", 130)):
            timed = run([*self_cmd, mode], status, short=True, TEST_STOP_OBSERVER="1", **env)
            assert "stack collection incomplete: time limit reached" in timed, timed
            assert timed.count("fixture started") == 1
        for mode, status in (("wait", 86), ("fallback", 124)):
            timed = run([probe, mode], status, short=True, **env)
            assert "stack collection incomplete: time limit reached" in timed, timed
            assert "wall-clock timeout reached" in timed
        stuck = run([*self_cmd, "SIGTRAP-handled"], 125, short=True, infrastructure=True,
                    TEST_IGNORE_INTERRUPT="1", **env)
        assert "original test outcome unknown" in stuck and stuck.count("fixture started") == 1
        lost = run([*self_cmd, "SIGTRAP-handled"], 125, short=True, infrastructure=True,
                   TEST_LOSE_OBSERVER="1", **env)
        assert "signal delivery was not verified" in lost and lost.count("fixture started") == 1
        # Even an observer wedged before the first capture event has a bound.
        script.write_text("""import gdb, signal
def stall_on_resume(_event):
    while True:
        signal.pause()
gdb.events.cont.connect(stall_on_resume)
""")
        outside = run([*self_cmd, "failure"], 125, short=True, infrastructure=True,
                      TEST_SHORT_RUN_CAP="4", **env)
        assert "original test outcome unknown" in outside and "stack collection timed out" not in outside
    fatal = run([*self_cmd, "signal"], 128 + signal.SIGSEGV)
    assert fatal.count("fixture started") == 1
    assert "[native-ci] signal=SIGSEGV" in fatal and "[native-ci] termination_signal=11" in fatal
    assert fatal.count("[native-ci] thread=") == 32 and "[native-ci]   #0 " in fatal, fatal
    assert "additional threads omitted" in fatal and "[native-ci]   #24 " not in fatal
    print("Native stack smoke: pass, ordinary failure, watchdog, no-core fallback, "
          "bounded signal stacks and observer stalls verified.")


if __name__ == "__main__":
    if sys.argv[1] == "--short-supervisor":
        sys.exit(runpy.run_path(str(WRAPPER))["run"](
            sys.argv[2:], capture_seconds=0.5, grace_seconds=2,
            run_seconds=int(os.environ.get("TEST_SHORT_RUN_CAP", "15"))))
    if sys.argv[1] == "--fixture":
        sys.exit(fixture(sys.argv[2]))
    check(Path(sys.argv[1]).resolve())
