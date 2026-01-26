#pragma once

/// Elio Debug Support
/// 
/// This header provides debugging utilities for Elio coroutines:
/// - Source location tracking for coroutine frames
/// - State tracking (created/running/suspended/completed/failed)
/// - Virtual stack via parent pointer chain
/// - Integration with GDB and LLDB via Python scripts
///
/// Note: There is no global frame registry to avoid synchronization overhead.
/// Debuggers find coroutine frames by traversing the scheduler's worker queues.
///
/// Usage:
///   In GDB:  source /path/to/elio-gdb.py
///            elio bt          # Show all vthread backtraces
///            elio list        # List all vthreads
///            elio workers     # Show worker information
///
///   In LLDB: command script import /path/to/elio-lldb.py
///            elio bt          # Show all vthread backtraces
///
///   Command line:
///            elio-pstack <pid>         # pstack-like output for running process
///            elio-pstack <corefile>    # pstack-like output for coredump

#include "coro/promise_base.hpp"

namespace elio::debug {

/// Get current virtual stack depth for the calling thread
inline size_t current_stack_depth() noexcept {
    size_t depth = 0;
    coro::promise_base* frame = coro::promise_base::current_frame();
    while (frame) {
        ++depth;
        frame = frame->parent();
    }
    return depth;
}

/// Walk the current thread's virtual stack
template<typename Func>
void walk_current_stack(Func&& func) {
    coro::promise_base* frame = coro::promise_base::current_frame();
    while (frame) {
        func(frame);
        frame = frame->parent();
    }
}

} // namespace elio::debug
