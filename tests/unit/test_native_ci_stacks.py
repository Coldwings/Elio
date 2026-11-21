"""Smoke-test the Linux CI observer with deliberately signalled stand-alone fixtures."""
import os
from pathlib import Path
import re
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
    def run(command, status, **overrides):
        env = {**os.environ, "ELIO_TEST_WATCHDOG_SECS": "1",
               "ELIO_TEST_WATCHDOG_DISABLE": "0", "OBSERVER_TEST_SECRET": SENTINEL,
               **overrides}
        result = subprocess.run([sys.executable, WRAPPER, *command], env=env,
                                stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                text=True, timeout=30)
        summary = rf"^\[native-ci\] (?:termination_signal=\d+ )?exit_status={status}$"
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
    for mode, status in (("wait", 86), ("fallback", 124)):
        timeout = run([probe, mode], status)
        assert "[native-ci] signal=SIGALRM" in timeout and "wall-clock timeout reached" in timeout
        assert " 'main'" in timeout, timeout
    fatal = run([*self_cmd, "signal"], 128 + signal.SIGSEGV)
    assert fatal.count("fixture started") == 1
    assert "[native-ci] signal=SIGSEGV" in fatal and "[native-ci] termination_signal=11" in fatal
    assert fatal.count("[native-ci] thread=") == 32 and "[native-ci]   #0 " in fatal, fatal
    assert "additional threads omitted" in fatal and "[native-ci]   #24 " not in fatal
    print("Native stack smoke: pass, ordinary failure, watchdog, no-core fallback and bounded signal stacks verified.")


if __name__ == "__main__":
    if sys.argv[1] == "--fixture":
        sys.exit(fixture(sys.argv[2]))
    check(Path(sys.argv[1]).resolve())
