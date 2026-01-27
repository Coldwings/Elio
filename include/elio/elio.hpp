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
#include "coro/cancel_token.hpp"

// Runtime scheduler
#include "runtime/scheduler.hpp"
#include "runtime/worker_thread.hpp"
#include "runtime/chase_lev_deque.hpp"
#include "runtime/async_main.hpp"
#include "runtime/affinity.hpp"

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

// Signal handling
#include "signal/signalfd.hpp"

// Debug support
#include "debug.hpp"

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
/// coro::task<int> async_main(int argc, char* argv[]) {
///     int result = co_await compute();
///     std::cout << "Result: " << result << std::endl;
///     co_return 0;
/// }
/// 
/// ELIO_ASYNC_MAIN(async_main)
/// ```
