# Elio Coroutine Library

**Elio** is a modern, production-ready C++20 coroutine library for high-performance asynchronous programming on Linux. It provides stackless coroutines with virtual stack tracking, a multi-threaded work-stealing scheduler, and a foundation for efficient I/O operations.

[![CI](https://github.com/Coldwings/Elio/actions/workflows/ci.yml/badge.svg)](https://github.com/Coldwings/Elio/actions/workflows/ci.yml)
[![C++20](https://img.shields.io/badge/C%2B%2B-20-blue.svg)](https://en.cppreference.com/w/cpp/20)
[![License](https://img.shields.io/badge/license-MIT-green.svg)](LICENSE)

## Features

- **C++20 Stackless Coroutines** with `task<T>` type
- **Virtual Stack Tracking** for natural exception propagation
- **Work-Stealing Scheduler** with lock-free Chase-Lev deques
- **Dynamic Thread Pool** with runtime adjustment
- **Synchronization Primitives**: mutex, semaphore, event, channel
- **Timers**: sleep_for, sleep_until, yield
- **I/O Backends**: io_uring (preferred) and epoll fallback
- **TCP Networking**: async client/server with connection management
- **HTTP/1.1**: full client and server implementation
- **HTTP/2**: client with multiplexed streams via nghttp2
- **TLS/HTTPS**: OpenSSL-based with ALPN and certificate verification
- **Header-Only Library** for easy integration
- **Comprehensive Testing** with Catch2 and ASAN
- **Integrated Logging** with fmtlib
- **CI/CD** with GitHub Actions

## Quick Start

### Prerequisites

- **Compiler**: GCC 12+ or Clang 15+ with C++20 support
- **OS**: Linux (kernel 5.1+ for io_uring, or any modern Linux for epoll)
- **CMake**: 3.20 or higher
- **Optional**: liburing (for io_uring backend), OpenSSL (for TLS/HTTPS)
- **Dependencies**: Automatically fetched via CMake FetchContent
  - fmtlib 10.2.1
  - Catch2 3.5.0 (for tests)
  - nghttp2 1.64.0 (for HTTP/2, requires OpenSSL)

### Building

```bash
git clone https://github.com/Coldwings/Elio.git
cd Elio
mkdir build && cd build
cmake ..
cmake --build .
```

### Running Tests

```bash
# Standard tests
ctest --output-on-failure

# ASAN tests (memory safety)
./build/tests/elio_tests_asan
```

### Your First Coroutine

```cpp
#include <elio/elio.hpp>
#include <iostream>

using namespace elio;

// A simple coroutine that returns a value
coro::task<int> compute() {
    co_return 42;
}

// Main coroutine that awaits other coroutines
coro::task<void> main_task() {
    int result = co_await compute();
    std::cout << "Result: " << result << std::endl;
}

int main() {
    // Create scheduler with 4 worker threads
    runtime::scheduler sched(4);
    sched.start();
    
    // Spawn the main task
    auto t = main_task();
    sched.spawn(t.handle());
    
    // Wait for completion
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    
    // Clean shutdown
    sched.shutdown();
    
    return 0;
}
```

## Architecture Overview

### Core Components

```
elio::
├── coro::                    // Coroutine primitives
│   ├── task<T>              // Primary coroutine type
│   ├── promise_base         // Virtual stack base class
│   ├── awaitable_base<T>    // CRTP awaitable base
│   └── frame utilities      // Virtual stack inspection
│
├── runtime::                // Scheduler and execution
│   ├── scheduler            // Work-stealing scheduler
│   ├── worker_thread        // Worker thread implementation
│   └── chase_lev_deque<T>   // Lock-free work queue
│
├── sync::                   // Synchronization primitives
│   ├── mutex                // Async mutex
│   ├── semaphore            // Counting semaphore
│   ├── event                // One-shot event
│   └── channel<T>           // MPSC channel
│
├── time::                   // Timer utilities
│   ├── sleep_for            // Duration-based sleep
│   ├── sleep_until          // Time-point sleep
│   └── yield                // Yield execution
│
├── io::                     // I/O backend
│   ├── io_context           // Unified I/O interface
│   ├── io_uring_backend     // io_uring implementation
│   └── epoll_backend        // epoll fallback
│
├── net::                    // Networking
│   └── tcp                  // TCP client/server
│
├── http::                   // HTTP protocol
│   ├── http_client          // HTTP/HTTPS client
│   ├── http_server          // HTTP server
│   ├── h2_client            // HTTP/2 client
│   └── h2_session           // HTTP/2 session (nghttp2)
│
├── tls::                    // TLS/SSL support
│   └── tls_stream           // OpenSSL wrapper
│
└── log::                    // Logging infrastructure
    ├── logger               // Thread-safe logger
    └── ELIO_LOG_* macros    // Logging macros
```

### Virtual Stack

Elio implements a **virtual stack** by linking coroutine frames together. This enables:
- Natural exception propagation through `co_await` chains
- Call chain inspection for debugging
- Automatic cleanup on coroutine completion

```cpp
outer() -> middle() -> inner()
  |          |          |
  v          v          v (throws exception)
[frame]   [frame]   [frame + exception_ptr]
  ^          |          |
  |          +----------+ (exception propagates up)
  |
  +--- (caught and handled)
```

### Work-Stealing Scheduler

The scheduler manages a pool of worker threads, each with a local task queue. Key features:
- **Lock-free operations**: Chase-Lev deques for optimal performance
- **Work stealing**: Idle threads steal tasks from busy threads
- **Dynamic sizing**: Adjust thread count at runtime
- **Load balancing**: Automatic task distribution

## Examples

Explore the `examples/` directory for detailed examples:

- **hello_world.cpp** - Basic coroutine usage
- **chained_coroutines.cpp** - Virtual stack demonstration
- **exception_handling.cpp** - Exception propagation
- **parallel_tasks.cpp** - Work stealing in action
- **dynamic_threads.cpp** - Dynamic thread pool
- **tcp_echo_server.cpp** - TCP server example
- **tcp_echo_client.cpp** - TCP client example
- **http_server.cpp** - HTTP server example
- **http_client.cpp** - HTTP/HTTPS client example
- **http2_client.cpp** - HTTP/2 client example
- **async_file_io.cpp** - Async file operations
- **benchmark.cpp** - Performance measurements

Build and run examples:
```bash
cd build
make
./hello_world
./parallel_tasks
./benchmark
```

## API Reference

### Creating Coroutines

```cpp
// Coroutine returning a value
elio::coro::task<int> get_value() {
    co_return 42;
}

// Coroutine returning void
elio::coro::task<void> do_work() {
    // ... work ...
    co_return;
}

// Awaiting other coroutines
elio::coro::task<int> compute() {
    int value = co_await get_value();
    co_return value * 2;
}
```

### Scheduler Operations

```cpp
// Create and start scheduler
elio::runtime::scheduler sched(num_threads);
sched.start();

// Spawn coroutines
auto t = my_coroutine();
sched.spawn(t.handle());

// Dynamic thread adjustment
sched.set_thread_count(8);

// Statistics
size_t threads = sched.num_threads();
size_t pending = sched.pending_tasks();
size_t executed = sched.total_tasks_executed();

// Shutdown (waits for completion)
sched.shutdown();
```

### Exception Handling

```cpp
elio::coro::task<void> safe_operation() {
    try {
        int result = co_await risky_operation();
        // ... use result ...
    } catch (const std::exception& e) {
        // Handle exception
        std::cerr << "Error: " << e.what() << std::endl;
    }
}
```

### Logging

```cpp
// Set log level
elio::log::logger::instance().set_level(elio::log::level::debug);

// Use logging macros
ELIO_LOG_DEBUG("Debug message: {}", value);   // Compile-time conditional
ELIO_LOG_INFO("Info: {}", data);              // Runtime conditional
ELIO_LOG_WARNING("Warning: {}", issue);       // Runtime conditional
ELIO_LOG_ERROR("Error: {}", error);           // Always enabled
```

## Testing

Elio includes comprehensive tests:

- **Unit Tests**: Test each component in isolation
- **Integration Tests**: Test components working together
- **ASAN Tests**: Detect memory errors

```bash
# Run all tests
ctest --test-dir build --output-on-failure

# Run specific test suites
./build/tests/elio_tests "[scheduler]"
./build/tests/elio_tests "[integration]"

# Run with sanitizers
./build/tests/elio_tests_asan
```

## Performance

Phase 1 benchmarks (on typical hardware):
- **Task spawn overhead**: < 200ns per task
- **Context switch**: < 100ns per suspend/resume
- **Work stealing**: Efficient load balancing across threads
- **Scalability**: Near-linear with thread count

Run benchmarks:
```bash
./benchmark
```

## Project Status

**Current Status**: Feature Complete

- Core coroutine infrastructure (task<T>, virtual stack)
- Work-stealing scheduler with dynamic thread pool
- Synchronization primitives (mutex, semaphore, event, channel)
- Timers (sleep_for, sleep_until, yield)
- I/O backends (io_uring, epoll)
- TCP networking (client/server)
- HTTP/1.1 (client/server)
- HTTP/2 (client)
- TLS/HTTPS support
- Comprehensive test suite
- CI/CD pipeline

## Contributing

Contributions are welcome! Please:
1. Follow C++20 best practices
2. Write tests for new features
3. Ensure ASAN tests pass
4. Ensure CI passes
5. Document public APIs
6. Follow existing code style

## License

MIT License - see [LICENSE](LICENSE) file for details.

## References

### Academic Papers
- Chase-Lev Deque: "Dynamic Circular Work-Stealing Deque" by David Chase and Yossi Lev
- Work Stealing: "Scheduling Multithreaded Computations by Work Stealing" by Robert Blumofe and Charles Leiserson
- C++20 Coroutines: "Coroutines: A Programmer's Perspective" by Lewis Baker

### Implementations
- Tokio (Rust): Work-stealing runtime
- Go runtime: Goroutine scheduler
- Folly (Facebook): Async framework

## Contact

For questions, issues, or feature requests, please open an issue on the repository.

---

**Status**: Feature Complete | **Version**: 0.1.0 | **Date**: 2026-01-18
