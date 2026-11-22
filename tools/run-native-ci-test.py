#!/usr/bin/env python3
"""Run one Linux CI test; a sibling GDB observes it but never owns its exit status."""
import ctypes
import os
from pathlib import Path
import resource
import select
import shutil
import signal
import socket
import subprocess
import sys
import time


def note(message):
    print(f"[native-ci] {message}", flush=True)


def start_observer(pid):
    gdb = shutil.which("gdb")
    if not gdb:
        return None
    ready, notify = os.pipe()
    try:
        # Disable automatic scripts before attaching. No arbitrary PID is accepted:
        # this is the forked, still-gated child of this invocation.
        args = [gdb, "-q", "-nx", "-batch", "-iex", "set auto-load off",
                "-iex", "set debuginfod enabled off", "-iex", "set print frame-arguments none",
                "-iex", "set print entry-values no", "-p", str(pid),
                "-x", str(Path(__file__).with_name("native-ci-stacks.gdb"))]
        try:
            observer = subprocess.Popen(args, stdin=subprocess.DEVNULL, pass_fds=(notify,),
                                        env={**os.environ, "ELIO_CI_GDB_READY": str(notify),
                                             "ELIO_CI_TEST_PID": str(pid)})
        except OSError:
            os.close(ready)
            return None
    finally:
        os.close(notify)
    if select.select([ready], [], [], 10)[0] and os.read(ready, 1) == b"1":
        return observer, ready
    os.close(ready)
    observer.kill()
    observer.wait()
    return None


def supervise(pid, observer, events, capture_seconds, grace_seconds, run_seconds):
    # The child's own alarm is unchanged. This separate last resort also
    # bounds debugger stops outside the Python stack collector.
    end = time.monotonic() + run_seconds
    capture_end = None
    interrupted = False
    watching = True
    finished = False
    try:
        while True:
            done, status = os.waitpid(pid, os.WNOHANG)
            if done:
                try:
                    observer.wait(timeout=grace_seconds)
                except subprocess.TimeoutExpired:
                    observer.kill()
                    observer.wait()
                if watching and select.select([events], [], [], 0)[0]:
                    finished |= b"X" in os.read(events, 256)
                if not finished or observer.returncode:
                    note("observer ended unexpectedly; stack information may be incomplete")
                    code = os.waitstatus_to_exitcode(status)
                    if code >= 0:
                        note(f"native exit_status={code} but signal delivery was not verified; CI failure=125")
                        return None
                return status
            now = time.monotonic()
            if now >= end or (capture_end is not None and interrupted and now >= capture_end):
                # Killing only GDB at a delivery stop can silently discard the
                # signal and turn a failing test green. No native outcome is
                # knowable here. Stop the owned test first and fail the job.
                note("observer supervision expired; original test outcome unknown; CI failure=125")
                os.kill(pid, signal.SIGKILL)
                observer.kill()
                os.waitpid(pid, 0)
                observer.wait()
                return None
            if capture_end is not None and now >= capture_end:
                note("stack collection timed out; asking observer to forward the original signal")
                observer.send_signal(signal.SIGCONT)
                observer.send_signal(signal.SIGUSR1)
                interrupted = True
                capture_end = now + grace_seconds
            if not select.select([events] if watching else [], [], [], 0.1)[0]:
                continue
            marks = os.read(events, 256)
            if not marks:
                watching = False
                if not finished:
                    note("observer channel closed before completion; stack evidence may be incomplete")
                continue
            for mark in marks:
                if mark == ord("B"):
                    capture_end = time.monotonic() + capture_seconds
                    interrupted = False
                elif mark == ord("E"):
                    capture_end = None
                elif mark == ord("X"):
                    finished = True
    finally:
        os.close(events)


def run(command, *, capture_seconds=5, grace_seconds=5, run_seconds=None):
    note(f"build={os.environ.get('GITHUB_SHA', 'unknown')} arch={os.uname().machine}"
         f" config={os.environ.get('ELIO_CI_BUILD_TYPE', 'unknown')}")
    # Apply to both the test and the observer (a debugger can itself fail).
    resource.setrlimit(resource.RLIMIT_CORE, (0, 0))
    parent, child = socket.socketpair()
    pid = os.fork()
    if pid == 0:
        parent.close()
        try:
            # On a Yama-restricted runner, nominate our own parent and its children
            # (the observer). Never change system policy or grant an arbitrary tracer.
            prctl = ctypes.CDLL(None).prctl
            prctl.argtypes = [ctypes.c_int] + [ctypes.c_ulong] * 4
            ptracer = 0x59616d61  # Linux PR_SET_PTRACER
            prctl(ptracer, os.getppid(), 0, 0, 0)
            child.sendall(b"R")
            if child.recv(1) != b"G":
                os._exit(125)
            prctl(ptracer, 0, 0, 0, 0)
            child.close()
            os.execvp(command[0], command)
        except OSError as error:
            note(f"could not execute test: {error.strerror}")
            os._exit(127 if isinstance(error, FileNotFoundError) else 126)

    child.close()
    with parent:
        if parent.recv(1) != b"R":
            os.waitpid(pid, 0)
            note("test launcher failed before exec")
            return 125
        session = start_observer(pid)
        if session is None:
            note("stacks unavailable: GDB with Python and child tracing is required")
        parent.sendall(b"G")
    if session is None:
        _, wait_status = os.waitpid(pid, 0)
    else:
        observer, events = session
        if run_seconds is None:
            try:
                seconds = int(os.environ.get("ELIO_TEST_WATCHDOG_SECS", "600"))
            except ValueError:
                seconds = 600
            # With no native watchdog, the CI observer still needs its own bound.
            if os.environ.get("ELIO_TEST_WATCHDOG_DISABLE") == "1":
                seconds = 600
            run_seconds = (seconds if seconds > 0 else 600) + 20
        wait_status = supervise(pid, observer, events, capture_seconds, grace_seconds, run_seconds)
        if wait_status is None:
            return 125
    code = os.waitstatus_to_exitcode(wait_status)
    status = 128 - code if code < 0 else code
    note((f"termination_signal={-code} " if code < 0 else "") + f"exit_status={status}")
    return status


if __name__ == "__main__":
    if len(sys.argv) < 2:
        sys.exit("usage: run-native-ci-test.py TEST [ARG ...]")
    sys.exit(run(sys.argv[1:]))
