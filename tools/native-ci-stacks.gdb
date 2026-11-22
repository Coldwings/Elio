# Attached by run-native-ci-test.py before the child execs the native test.
set pagination off
set confirm off
set print thread-events off
set print frame-arguments none
set print entry-values no
handle SIGPIPE SIGUSR1 SIGUSR2 nostop noprint pass
# GDB normally swallows these two target signals. Its own exec/fork events
# are not signal-delivery stops and must not be reinjected by the observer.
handle SIGINT pass

python
import os
import signal
import gdb

seen = set()
active_capture = False

class CaptureExpired(Exception):
    pass

def interrupt_capture(_signum, _frame):
    # Directed to the debugger, not the target. A late request outside
    # formatting must not interrupt restoration or the next continue.
    if active_capture:
        raise CaptureExpired()

signal.signal(signal.SIGUSR1, interrupt_capture)

def label(value):
    return ascii(value[:180])

def emit(text):
    gdb.write("[native-ci] " + text + "\n")

def record_stacks():
    global active_capture
    try:
        original = gdb.selected_thread()
        signum = int(gdb.parse_and_eval("$_siginfo.si_signo"))
    except gdb.error:
        emit("signal stop information unavailable")
        return
    if signum in seen:
        return
    seen.add(signum)
    try:
        active_capture = True
        os.write(ready, b"B")
        threads = gdb.selected_inferior().threads()
        emit(f"signal={signal.Signals(signum).name} thread_count={len(threads)}")
        # Always include the signalled thread, even when the cap is reached.
        ordered = [original] + [t for t in threads if t.num != original.num]
        for thread in ordered[:32]:
            emit(f"thread={thread.num}")
            try:
                thread.switch()
                frame = gdb.newest_frame()
                for level in range(24):
                    if frame is None:
                        break
                    sal = frame.find_sal()
                    source = (f" {label(os.path.basename(sal.symtab.filename))}:{sal.line}"
                              if sal and sal.symtab is not None else "")
                    emit(f"  #{level} {frame.pc():#x} {label(frame.name() or '??')}{source}")
                    frame = frame.older()
                if frame is not None:
                    emit("  additional frames omitted")
            except (gdb.error, RuntimeError):
                emit("  stack unavailable or incomplete")
        if len(ordered) > 32:
            emit("additional threads omitted")
    except CaptureExpired:
        emit("stack collection incomplete: time limit reached")
    except Exception as error:
        emit("stack collection unavailable: " + type(error).__name__)
    finally:
        active_capture = False
        try:
            if original is not None and original.is_valid():
                original.switch()
        except gdb.error:
            emit("signalled thread no longer available")
        os.write(ready, b"E")
end

catch signal SIGALRM SIGSEGV SIGABRT SIGBUS SIGILL SIGFPE SIGSYS SIGQUIT SIGINT
commands
  silent
  python record_stacks()
  continue
end
python
if gdb.selected_inferior().pid != int(os.environ["ELIO_CI_TEST_PID"]):
    raise gdb.GdbError("could not attach to native test")
ready = int(os.environ["ELIO_CI_GDB_READY"])
os.write(ready, b"1")
resume = "continue"
while gdb.selected_inferior().pid:
    gdb.execute(resume)
    resume = "continue"
    # An explicit SIGTRAP catchpoint also sees GDB's own loader breakpoints.
    # Leave GDB's internal trap handling alone; only reinject a signal stop
    # that ordinary 'continue' actually returned to the command loop.
    if gdb.selected_inferior().pid and int(gdb.parse_and_eval("$_siginfo.si_signo")) == signal.SIGTRAP:
        record_stacks()
        resume = "signal SIGTRAP"
os.write(ready, b"X")
os.close(ready)
end
