# Debugging

Elio provides debugging tools to inspect coroutine (vthread) states, virtual call stacks, and scheduler information. These tools work with both live processes and coredump files.

## Overview

Elio coroutines maintain debug metadata in each frame:
- Unique ID for identification
- State (created, running, suspended, completed, failed)
- Source location (file, function, line)
- Worker thread assignment
- Construction parent, activation parent, and vthread ownership metadata

The debugger extensions find coroutine frames by traversing the scheduler's worker queues (Chase-Lev deque and MPSC inbox). This approach has **zero runtime overhead** - no global registry or synchronization is required.

Important limitation: today these tools primarily see **queued** coroutines. A coroutine that is currently running on a worker thread is not guaranteed to appear until it suspends or is re-enqueued.

## Virtual Stack

C++20 stackless coroutines allocate each frame independently on the heap. When a coroutine suspends, the native call stack unwinds completely, so traditional stack traces cannot show the logical call chain. Elio reconstructs this information through an intrusive virtual-stack and vthread metadata model built into every coroutine frame.

### How It Works

Each coroutine's promise type inherits from `promise_base`, which now carries multiple relationships:

- `parent_`: the construction-time parent captured from `current_frame_`
- `activation_parent_`: the coroutine that first activates a cold child
- `vthread_owner_`: the owner domain responsible for memory ownership and resume context

`current_frame_` is still used to capture low-cost construction lineage. Runtime execution, however, is determined later:

- direct `co_await` binds activation-parent metadata when the child is first awaited
- `spawn()` and `go()` create a fresh vthread owner and cold-relocate the spawned root before first resume
- work stealing resumes an already-owned suspended leaf on a different worker without changing frame ownership

This means the debugger-visible construction chain and the runtime ownership chain are related, but not identical. In particular, nested expressions such as `co_await bar(foo())` may have construction order different from first execution order.

### Frame Validation

Each `promise_base` contains a `frame_magic_` field set to `0x454C494F46524D45` (the ASCII string "ELIOFRME"). The debugger tools check this magic value when traversing memory to distinguish valid Elio coroutine frames from arbitrary data. This is especially important during coredump analysis, where the debugger walks raw memory without type information.

### Debug Metadata

Every frame carries the following debug metadata with no additional allocation:

| Field | Description |
|-------|-------------|
| `debug_id_` | Unique monotonic identifier assigned on demand |
| `debug_state_` | Current state: created, running, suspended, completed, or failed |
| `debug_worker_id_` | Index of the worker thread the frame is assigned to (or -1 if unassigned) |
| `debug_location_` | Source file, function name, and line number |
| `parent_` | Construction-time parent pointer used for low-overhead lineage/debug traversal |
| `activation_parent_` | Runtime first-activation parent for direct await / detached activation semantics |
| `vthread_owner_` | Owner domain used for frame ownership and vthread context restoration |
| `frame_magic_` | Magic number for frame integrity validation |

The debugger tools (`elio-pstack`, `elio-gdb.py`, `elio_lldb.py`) use this metadata to present coroutine state in a format familiar to anyone who has used `pstack` or `thread apply all bt`. They now prefer the activation-parent chain when it is present, and fall back to the construction-parent chain otherwise. Owner metadata is shown separately in `elio info` style output.

## Tools

| Tool | Description |
|------|-------------|
| `elio-pstack` | Command-line tool similar to `pstack` |
| `elio-gdb.py` | GDB Python extension |
| `elio_lldb.py` | LLDB Python entrypoint |

## elio-pstack

A command-line tool that prints stack traces of all Elio coroutines, similar to `pstack` for threads.

### Usage

```bash
# Attach to running process
elio-pstack <pid>

# Analyze coredump with executable
elio-pstack <executable> <corefile>

# Analyze coredump (auto-detect executable)
elio-pstack <corefile>
```

### Options

| Option | Description |
|--------|-------------|
| `-h, --help` | Show help message |
| `-v, --verbose` | Show verbose output |
| `-l, --list` | Only list vthreads (no backtraces) |
| `-i, --info <id>` | Show detailed info for specific vthread |
| `-s, --stats` | Show statistics only |

### Examples

```bash
# Print all vthread backtraces for a running process
elio-pstack 12345

# List all vthreads without backtraces
elio-pstack -l 12345

# Get detailed info for vthread #42
elio-pstack -i 42 12345

# Analyze a coredump
elio-pstack ./myapp core.12345

# Show scheduler statistics from coredump
elio-pstack -s core.12345
```

## GDB Extension

### Loading

```bash
# From GDB command line
gdb -ex 'source /path/to/tools/elio-gdb.py' ./myapp

# Or in GDB session
(gdb) source /path/to/tools/elio-gdb.py

# Or add to ~/.gdbinit
source /path/to/tools/elio-gdb.py
```

### Commands

| Command | Description |
|---------|-------------|
| `elio` | Show help |
| `elio list` | List all vthreads from worker queues |
| `elio bt [id]` | Show backtrace for vthread(s) |
| `elio info <id>` | Show detailed info for a vthread |
| `elio workers` | Show worker thread information |
| `elio stats` | Show scheduler statistics |

### Examples

```
(gdb) elio list
--------------------------------------------------------------------------------
ID       State        Worker   Function                       Location
--------------------------------------------------------------------------------
1        suspended    0        worker_task                    debug_test.cpp:84
2        suspended    1        process_data                   debug_test.cpp:73
3        running      2        compute_value                  debug_test.cpp:60

Total queued coroutines: 3

(gdb) elio bt 1
vthread #1 [suspended] (worker 0)
  #0   0x00007f1234567890 in worker_task at debug_test.cpp:84
  #1   0x00007f1234567abc in async_main at debug_test.cpp:112

(gdb) elio info 1
vthread #1
  State:    suspended
  Worker:   0
  Handle:   0x00007f1234567890
  Promise:  0x00007f12345678a0
    Owner:    0x00007f1234500000
    Root:     no
    Started:  yes
    FrameSz:  160
    Parent:   0x00007f1234567000
    ActParent:0x00007f1234567000
  Function: worker_task
  Location: debug_test.cpp:84

    Virtual Call Stack (activation chain):
        #0   [self] worker_task at debug_test.cpp:84
        #1   [activation_parent] async_main at debug_test.cpp:112

(gdb) elio workers
Scheduler: running
Worker threads: 4
------------------------------------------------------------
Worker   Status       Queue Size   Tasks Executed
------------------------------------------------------------
0        running      5            1234
1        running      3            1189
2        running      4            1201
3        running      2            1156

(gdb) elio stats
Scheduler: running
Worker threads: 4
Total queued coroutines: 14
Total tasks executed: 4780
```

## LLDB Extension

### Loading

```bash
# From LLDB command line
lldb -o 'command script import /path/to/tools/elio_lldb.py' ./myapp

# Or in LLDB session
(lldb) command script import /path/to/tools/elio_lldb.py

# Or add to ~/.lldbinit
command script import /path/to/tools/elio_lldb.py
```

### Commands

The LLDB extension provides the same commands as GDB:

| Command | Description |
|---------|-------------|
| `elio` | Show help |
| `elio list` | List all vthreads from worker queues |
| `elio bt [id]` | Show backtrace for vthread(s) |
| `elio info <id>` | Show detailed info for a vthread |
| `elio workers` | Show worker thread information |
| `elio stats` | Show scheduler statistics |

`elio info` in both GDB and LLDB now prints owner/root/started metadata in addition to source location and queue worker information.

## Setting Debug Location

For more accurate debugging information, you can manually set the debug location in your coroutines:

```cpp
#include <elio/elio.hpp>

// Helper awaitable to get promise reference
struct get_promise {
    bool await_ready() const noexcept { return false; }
    
    template<typename Promise>
    bool await_suspend(std::coroutine_handle<Promise> h) noexcept {
        promise_ = &h.promise();
        return false; // Don't actually suspend
    }
    
    elio::coro::promise_base& await_resume() noexcept {
        return *promise_;
    }
    
    elio::coro::promise_base* promise_ = nullptr;
};

elio::coro::task<void> my_coroutine() {
    // Set debug location
    auto& p = co_await get_promise{};
    p.set_location(__FILE__, __FUNCTION__, __LINE__);
    p.set_state(elio::coro::coroutine_state::running);
    
    // ... coroutine body ...
    
    co_await some_operation();
    
    // Update state after suspension point
    p.set_state(elio::coro::coroutine_state::suspended);
    
    co_return;
}
```

## Sanitizer Support

Elio's test suite builds with both AddressSanitizer (ASAN) and ThreadSanitizer (TSAN). When either sanitizer is active, the custom frame allocator is automatically disabled and coroutine frames are allocated with standard `new`/`delete` instead. This ensures that sanitizers can properly track all allocations, detect use-after-free errors, and report accurate stack traces for memory and threading issues.

No source changes or build flags are needed beyond enabling the sanitizer itself:

```bash
# Build and run with ASAN
cmake --build . --target elio_tests_asan
./tests/elio_tests_asan

# Build and run with TSAN
cmake --build . --target elio_tests_tsan
./tests/elio_tests_tsan
```

## Limitations

1. **Only queued coroutines are visible**: Coroutines currently executing on a worker thread are not in any queue and cannot be found by the debugger. Use regular GDB/LLDB thread inspection for those.

2. **Queue traversal is best-effort**: The debugger reads queue data structures directly from memory. In rare cases of concurrent modification, some frames may be missed.

3. **Parent chain traversal**: The virtual stack display shows the parent pointer chain, but detailed info for parent frames may be limited.

## Coredump Analysis

All tools work with coredump files for post-mortem debugging:

```bash
# Generate coredump on crash (ensure ulimit allows it)
ulimit -c unlimited

# Or trigger manually
kill -ABRT <pid>
gcore <pid>

# Analyze with elio-pstack
elio-pstack ./myapp core.12345

# Or with GDB
gdb ./myapp core.12345 -ex 'source tools/elio-gdb.py' -ex 'elio bt'
```

## Troubleshooting

### "No active scheduler found"

The debugger couldn't find the scheduler. Possible causes:
- The process hasn't created a scheduler yet
- The scheduler has been destroyed
- Symbol information is not available (stripped binary)

### "No queued vthreads found"

All coroutines are either completed or currently executing. Check:
- Regular thread backtraces with `bt` or `thread apply all bt`
- Scheduler statistics with `elio stats`

### Symbols not found

Ensure the binary is compiled with debug symbols:
```bash
cmake -DCMAKE_BUILD_TYPE=Debug ..
# or
cmake -DCMAKE_BUILD_TYPE=RelWithDebInfo ..
```
