#pragma once

/// Elio Coroutine Library - Main Header
/// 
/// Version: 0.3.0 (Phase 3)
/// 
/// This header provides convenient access to all Elio components.
/// Include this file to use the Elio coroutine library.

// Version information
#define ELIO_VERSION_MAJOR 0
#define ELIO_VERSION_MINOR 3
#define ELIO_VERSION_PATCH 0

// Core coroutine types
#include "coro/promise_base.hpp"
#include "coro/task.hpp"
#include "coro/awaitable_base.hpp"
#include "coro/frame.hpp"

// Runtime scheduler
#include "runtime/scheduler.hpp"
#include "runtime/worker_thread.hpp"
#include "runtime/chase_lev_deque.hpp"

// I/O backend
#include "io/io_backend.hpp"
#include "io/io_context.hpp"
#include "io/io_uring_backend.hpp"
#include "io/epoll_backend.hpp"
#include "io/io_awaitables.hpp"

// Networking
#include "net/tcp.hpp"
#include "net/uds.hpp"

// Timers
#include "time/timer.hpp"

// Synchronization primitives
#include "sync/primitives.hpp"

// Logging
#include "log/logger.hpp"
#include "log/macros.hpp"

/// Root namespace for the Elio library
namespace elio {

/// Get library version string
inline const char* version() noexcept {
    return "0.3.0";
}

/// Get library version as tuple
inline constexpr auto version_tuple() noexcept {
    return std::make_tuple(ELIO_VERSION_MAJOR, ELIO_VERSION_MINOR, ELIO_VERSION_PATCH);
}

} // namespace elio

/// Quick Start Example:
/// 
/// ```cpp
/// #include <elio/elio.hpp>
/// 
/// using namespace elio;
/// 
/// coro::task<int> compute() {
///     co_return 42;
/// }
/// 
/// coro::task<void> main_task() {
///     int result = co_await compute();
///     std::cout << "Result: " << result << std::endl;
/// }
/// 
/// int main() {
///     runtime::scheduler sched(4);  // 4 worker threads
///     sched.start();
///     
///     auto t = main_task();
///     sched.spawn(t.handle());
///     
///     std::this_thread::sleep_for(std::chrono::milliseconds(100));
///     sched.shutdown();
/// }
/// ```
