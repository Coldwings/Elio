# Attached by run-native-ci-test.py before the child execs the native test.
set pagination off
set confirm off
set print thread-events off
set print frame-arguments none
set print entry-values no
handle SIGPIPE SIGUSR1 SIGUSR2 nostop noprint pass

python
import os
import signal
import gdb

seen = set()

def label(value):
    return ascii(value[:180])

def emit(text):
    gdb.write("[native-ci] " + text + "\n")

def record_stacks():
    original = gdb.selected_thread()
    try:
        signum = int(gdb.parse_and_eval("$_siginfo.si_signo"))
        if signum in seen:
            return
        seen.add(signum)
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
    except Exception as error:
        emit("stack collection unavailable: " + type(error).__name__)
    finally:
        try:
            if original is not None and original.is_valid():
                original.switch()
        except gdb.error:
            emit("signalled thread no longer available")
end

catch signal SIGALRM SIGSEGV SIGABRT SIGBUS SIGILL SIGFPE SIGSYS SIGQUIT
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
os.close(ready)
while gdb.selected_inferior().pid:
    gdb.execute("continue")
end
