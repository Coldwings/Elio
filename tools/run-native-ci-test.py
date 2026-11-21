#!/usr/bin/env python3
"""Run one Linux CI test; a sibling GDB observes it but never owns its exit status."""
import ctypes
import os
from pathlib import Path
import resource
import select
import shutil
import socket
import subprocess
import sys


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
    try:
        if select.select([ready], [], [], 10)[0] and os.read(ready, 1) == b"1":
            return observer
    finally:
        os.close(ready)
    observer.kill()
    observer.wait()
    return None


def run(command):
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
        observer = start_observer(pid)
        if observer is None:
            note("stacks unavailable: GDB with Python and child tracing is required")
        parent.sendall(b"G")
    _, wait_status = os.waitpid(pid, 0)
    if observer is not None:
        try:
            observer.wait(timeout=5)
        except subprocess.TimeoutExpired:
            observer.kill()
            observer.wait()
        if observer.returncode:
            note("observer ended unexpectedly; stack information may be incomplete")
    code = os.waitstatus_to_exitcode(wait_status)
    status = 128 - code if code < 0 else code
    note((f"termination_signal={-code} " if code < 0 else "") + f"exit_status={status}")
    return status


if __name__ == "__main__":
    if len(sys.argv) < 2:
        sys.exit("usage: run-native-ci-test.py TEST [ARG ...]")
    sys.exit(run(sys.argv[1:]))
