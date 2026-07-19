# Fork Safety

Elio uses worker threads, per-worker I/O backends, blocking pools, mutexes,
condition variables, thread-local runtime state, and process-wide signal setup.
Those resources define a strict process fork boundary.

## Supported Boundaries

### Fork Before Elio Runtime Use

A single-threaded process may call `fork()` before it creates or uses any Elio
scheduler, worker, I/O context, blocking pool, autoscaler, or other
runtime-owned resource. The parent and child may then construct independent
Elio runtimes. If any other thread already exists, the child is instead subject
to the active-runtime rule below even when that thread was not created by Elio.
The application must also satisfy the fork requirements of the C/C++ runtime
and every other linked library; Elio cannot make a process fork-safe if another
thread or library already owns incompatible state.

### Active Runtime: Parent Continues, Child Replaces Or Exits

If an Elio runtime is active when `fork()` occurs, the parent may continue
using its existing runtime. The child may only perform operations that are safe
after `fork()` in a multithreaded process and then immediately call an
`exec*()` function or `_exit()`.

After a successful `exec*()`, the new process image may create a fresh Elio
runtime normally. If `exec*()` fails, call `_exit()` from the child error path.

```cpp
pid_t pid = ::fork();
if (pid == 0) {
    char path[] = "/path/to/child";
    char arg0[] = "child";
    char* argv[] = {arg0, nullptr};
    char* envp[] = {nullptr};
    ::execve(path, argv, envp);
    ::_exit(127);
}
```

## Unsupported Child Continuation

The child must not continue an inherited Elio runtime. In particular, do not:

- schedule, resume, cancel, poll, resize, shut down, or destroy inherited Elio
  runtime objects;
- use inherited schedulers, coroutine frames, task groups, streams, listeners,
  timers, synchronization primitives, I/O contexts, or backend objects;
- return normally through stack frames that own inherited Elio objects, call
  `std::exit()`, or run inherited C++ cleanup and `atexit` handlers;
- construct a new Elio runtime in the child while inherited Elio runtime state
  is still present.

Only the thread that called `fork()` exists in the child. Locks held by vanished
threads, worker-local backend ownership, pending kernel completions, and
thread-local coroutine state cannot be repaired there. Elio 0.6 does not
install `pthread_atfork` handlers and does not provide a child-side reset or
reinitialization API.

## Caller Responsibilities

- Prefer spawning a new process through a direct fork-and-exec facility whose
  child path performs no application or Elio work before `exec*()`.
- Apply `FD_CLOEXEC` or explicit descriptor passing according to application
  policy. File descriptors inherited across `fork()` are a process concern;
  Elio does not decide which application sockets or files should survive
  `exec*()`.
- Coordinate signal masks and handlers with Elio and other libraries. The
  child should not invoke logging, allocation, or general C++ library code on
  an active-runtime fork path.
- Bound parent-side child waits and always reap children. A failed `exec*()`
  path should terminate with `_exit()` rather than unwind inherited state.

See [[API Contracts]] for the concise library/caller responsibility split and
[[Migrating to 0.6]] for the 0.6 upgrade checklist.
