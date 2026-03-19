# Elio

**Elio** is a modern C++20 coroutine-based asynchronous I/O library for Linux. It provides high-performance networking primitives with native support for TCP, HTTP/1.1, and TLS.

[![CI](https://github.com/Coldwings/Elio/actions/workflows/ci.yml/badge.svg)](https://github.com/Coldwings/Elio/actions/workflows/ci.yml)

## Features

- **C++20 Coroutines**: First-class coroutine support with `co_await` and `co_return`
- **Multi-threaded Scheduler**: Work-stealing scheduler with per-worker I/O contexts
- **Async I/O Backends**: io_uring (preferred) and epoll fallback
- **Signal Handling**: Coroutine-friendly signal handling via signalfd
- **Synchronization Primitives**: mutex, shared_mutex, semaphore, event, channel
- **Timers**: sleep_for, sleep_until, yield with cancellation support
- **TCP Networking**: Async TCP client/server with connection management
- **HTTP/1.1**: Full HTTP client and server implementation
- **TLS/HTTPS**: OpenSSL-based TLS with ALPN and certificate verification
- **RPC Framework**: High-performance RPC with zero-copy serialization, checksums, and cleanup callbacks
- **Hash Functions**: CRC32, SHA-1, and SHA-256 with incremental hashing support
- **Header-only**: Easy integration - just include and go
- **CI/CD**: Automated testing with ASAN and TSAN

## Requirements

- Linux (kernel 5.1+ for io_uring, or any modern Linux for epoll)
- GCC 12+ with C++20 support
- CMake 3.20+
- liburing (optional, for io_uring backend)
- OpenSSL (optional, for TLS/HTTPS support)

## Quick Start

```cpp
#include <elio/elio.hpp>

using namespace elio;

coro::task<void> hello() {
    ELIO_LOG_INFO("Hello from Elio!");
    co_return;
}

int main() {
    runtime::scheduler sched(4);
    sched.start();
    
    auto t = hello();
    sched.spawn(t.release());
    
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    sched.shutdown();
}
```

## Why Elio?

Elio is built around a few key technical decisions:

- **Header-only**: No separate build step, no ABI compatibility concerns. Template-heavy coroutine code naturally suits header-only distribution.
- **Linux-native**: Deep integration with io_uring and signalfd enables optimal performance on modern Linux kernels. epoll provides a fallback for older systems.
- **Per-worker I/O**: Each scheduler thread owns its I/O backend (io_uring or epoll), eliminating I/O-related locking entirely. Cross-thread communication uses lock-free MPSC queues.
- **Work-stealing**: The Chase-Lev deque provides lock-free local operations with a global load balancing fallback. Tasks with thread affinity are respected during stealing.
- **Virtual stack tracking**: C++20 stackless coroutines lose stack information at suspension points. Elio's intrusive virtual stack enables production debugging via `elio-pstack` and GDB/LLDB extensions.

## Wiki Contents

- [[Getting Started]] - Installation and first steps
- [[Core Concepts]] - Coroutines, tasks, and scheduler
- [[Signal Handling]] - Safe signal handling with signalfd
- [[Networking]] - TCP, HTTP/1.1, and connections
- [[HTTP2 Guide]] - HTTP/2 client usage and multiplexing
- [[TLS Configuration]] - TLS/SSL setup and certificate management
- [[WebSocket SSE]] - WebSocket and Server-Sent Events
- [[RPC Framework]] - High-performance RPC with zero-copy serialization
- [[Hash Functions]] - CRC32, SHA-1, and SHA-256
- [[Performance Tuning]] - Optimization and benchmarking
- [[Debugging]] - GDB/LLDB extensions and elio-pstack
- [[Examples]] - Code examples and use cases
- [[API Reference]] - Detailed API documentation

## License

MIT License
