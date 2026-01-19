# Elio

**Elio** is a modern C++20 coroutine-based asynchronous I/O library for Linux. It provides high-performance networking primitives with native support for TCP, HTTP/1.1, and TLS.

[![CI](https://github.com/Coldwings/Elio/actions/workflows/ci.yml/badge.svg)](https://github.com/Coldwings/Elio/actions/workflows/ci.yml)

## Features

- **C++20 Coroutines**: First-class coroutine support with `co_await` and `co_return`
- **Multi-threaded Scheduler**: Work-stealing scheduler with configurable worker threads
- **Async I/O Backends**: io_uring (preferred) and epoll fallback
- **Synchronization Primitives**: mutex, shared_mutex, semaphore, event, channel
- **Timers**: sleep_for, sleep_until, yield
- **TCP Networking**: Async TCP client/server with connection management
- **HTTP/1.1**: Full HTTP client and server implementation
- **TLS/HTTPS**: OpenSSL-based TLS with ALPN and certificate verification
- **Header-only**: Easy integration - just include and go
- **CI/CD**: Automated testing with GitHub Actions

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

## Wiki Contents

- [[Getting Started]] - Installation and first steps
- [[Core Concepts]] - Coroutines, tasks, and scheduler
- [[Networking]] - TCP, HTTP, and TLS
- [[Examples]] - Code examples and use cases
- [[API Reference]] - Detailed API documentation

## License

MIT License
