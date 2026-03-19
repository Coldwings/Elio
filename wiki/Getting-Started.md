# Getting Started

This guide will help you get Elio up and running in your project.

## Installation

### Prerequisites

```bash
# Ubuntu/Debian (22.04+)
sudo apt install build-essential cmake ninja-build g++-12 liburing-dev libssl-dev

# Fedora
sudo dnf install gcc-c++ cmake ninja-build liburing-devel openssl-devel

# Arch Linux
sudo pacman -S base-devel cmake ninja liburing openssl
```

### Building from Source

```bash
git clone https://github.com/Coldwings/Elio.git
cd Elio
cmake -B build -G Ninja
cmake --build build
```

### Running Tests

```bash
cd build
ctest --output-on-failure

# With AddressSanitizer (memory safety)
./tests/elio_tests_asan

# With ThreadSanitizer (thread safety)
./tests/elio_tests_tsan
```

### CMake Integration

#### Using FetchContent

```cmake
include(FetchContent)

FetchContent_Declare(elio
    GIT_REPOSITORY https://github.com/Coldwings/Elio.git
    GIT_TAG main
)
FetchContent_MakeAvailable(elio)

target_link_libraries(your_target PRIVATE elio)
```

#### Using add_subdirectory

```cmake
add_subdirectory(path/to/elio)
target_link_libraries(your_target PRIVATE elio)
```

#### Optional Features

Elio has three optional feature flags, each controlled by a CMake option:

| CMake option | Dependency | What it enables |
|---|---|---|
| `ELIO_ENABLE_TLS` | OpenSSL | TLS/SSL streams, HTTPS support |
| `ELIO_ENABLE_HTTP` | (none beyond TLS if HTTPS needed) | HTTP/1.1 client and server, WebSocket, SSE |
| `ELIO_ENABLE_HTTP2` | nghttp2 | HTTP/2 multiplexed connections |

Enable them at configure time:

```bash
cmake -B build -DELIO_ENABLE_TLS=ON -DELIO_ENABLE_HTTP=ON -DELIO_ENABLE_HTTP2=ON
```

When `ELIO_ENABLE_HTTP2` is set, nghttp2 is fetched automatically via CMake's `FetchContent` if it is not already installed on the system. OpenSSL and liburing must be installed separately (see Prerequisites above).

## Your First Program

Create a simple "Hello World" program:

```cpp
#include <elio/elio.hpp>
#include <iostream>

using namespace elio;

coro::task<std::string> get_greeting() {
    co_return "Hello from Elio!";
}

coro::task<int> async_main(int argc, char* argv[]) {
    std::cout << "Program: " << argv[0] << std::endl;
    if (argc > 1) {
        std::cout << "First argument: " << argv[1] << std::endl;
    }
    
    std::string greeting = co_await get_greeting();
    std::cout << greeting << std::endl;
    co_return 0;
}

ELIO_ASYNC_MAIN(async_main)
```

The `ELIO_ASYNC_MAIN` macro handles scheduler setup, I/O context initialization, and cleanup automatically. It passes `argc` and `argv` to your async_main function.

### Using I/O Context

The scheduler automatically creates and manages an I/O context. Access it via `io::default_io_context()`:

```cpp
coro::task<int> async_main(int argc, char* argv[]) {
    // Get the I/O context for async operations
    auto& ctx = io::default_io_context();
    
    // Use ctx for networking, timers, etc.
    co_await time::sleep_for(std::chrono::seconds(1));
    
    co_return 0;
}

ELIO_ASYNC_MAIN(async_main)
```

### Alternative: Using elio::run()

For more control, use `elio::run()` directly:

```cpp
int main(int argc, char* argv[]) {
    return elio::run(async_main(argc, argv));
}
```

Or configure via `run_config`:

```cpp
int main(int argc, char* argv[]) {
    elio::run_config config;
    config.num_threads = 4;  // Use 4 worker threads
    
    return elio::run(async_main(argc, argv), config);
}
```

### Macros Reference

| Macro | async_main signature | Description |
|-------|---------------------|-------------|
| `ELIO_ASYNC_MAIN` | `task<int>(int, char**)` | With args, returns exit code |
| `ELIO_ASYNC_MAIN_VOID` | `task<void>(int, char**)` | With args, always exits 0 |
| `ELIO_ASYNC_MAIN_NOARGS` | `task<int>()` | No args, returns exit code |
| `ELIO_ASYNC_MAIN_VOID_NOARGS` | `task<void>()` | No args, always exits 0 |

## Design Philosophy

A few deliberate choices shaped how Elio is built.

### Header-only

Elio ships as a header-only library. Coroutine-heavy code is inherently template-heavy -- the compiler needs to see full definitions to instantiate coroutine frames, promise types, and awaitables. A separate compilation model would add link-time complexity for very little gain. Header-only also means zero build integration friction: add the include path and you are done. There are no ABI compatibility concerns between your code and the library, because there is no compiled library.

### C++20 coroutines

C++20 stackless coroutines give us suspension and resumption without allocating a full thread stack per concurrent operation. The compiler manages coroutine frame layout and lifetime, which means the optimizer can see through `co_await` boundaries. When you are not using coroutines, you pay nothing -- no background threads, no hidden allocations. The trade-off is that C++20 coroutine support requires a reasonably modern compiler (GCC 12+, Clang 14+), but that is a reasonable baseline in 2024.

### Linux-only

Elio targets Linux exclusively. This is not an arbitrary limitation -- it enables deep integration with Linux-specific facilities like `io_uring` for high-throughput async I/O and `signalfd` for clean signal handling inside the event loop. Epoll is supported as a fallback for older kernels, but both backends assume Linux semantics. Targeting a single platform means the library can provide consistent behavior guarantees without `#ifdef` complexity or lowest-common-denominator abstractions.

## Running the Examples

```bash
cd build

# Hello World
./examples/hello_world

# TCP Echo Server (run in one terminal)
./examples/tcp_echo_server 8080

# TCP Echo Client (run in another terminal)
./examples/tcp_echo_client localhost 8080

# HTTP Server
./examples/http_server 8080

# HTTP Client
./examples/http_client https://httpbin.org/get
```

## Project Structure

Elio is a header-only library. Everything lives under `include/elio/`:

```
include/elio/
├── elio.hpp              # Main include
├── coro/                 # Coroutine primitives
│   ├── task.hpp          # task<T>
│   ├── promise_base.hpp  # Virtual stack base
│   ├── frame.hpp         # Stack introspection
│   ├── frame_allocator.hpp # Frame memory pool
│   ├── cancel_token.hpp  # Cooperative cancellation
│   └── awaitable_base.hpp # Awaitable interface
├── runtime/              # Scheduler and threading
│   ├── scheduler.hpp     # Work-stealing scheduler
│   ├── worker_thread.hpp # Worker implementation
│   ├── chase_lev_deque.hpp # Lock-free deque
│   ├── mpsc_queue.hpp    # Cross-thread queue
│   ├── wait_strategy.hpp # Idle wait policies
│   ├── affinity.hpp      # Thread affinity
│   ├── async_main.hpp    # Entry point macros
│   └── serve.hpp         # Server lifecycle
├── io/                   # I/O backends
│   ├── io_context.hpp
│   ├── io_backend.hpp
│   ├── io_uring_backend.hpp
│   └── epoll_backend.hpp
├── net/                  # Networking
│   ├── tcp.hpp
│   └── uds.hpp
├── sync/                 # Synchronization
│   └── primitives.hpp    # mutex, shared_mutex, semaphore, event, channel
├── time/                 # Timers
│   └── timer.hpp
├── signal/               # Signal handling
│   └── signalfd.hpp
├── http/                 # HTTP stack
│   ├── http_client.hpp
│   ├── http_server.hpp
│   ├── http2.hpp
│   ├── websocket.hpp
│   └── sse.hpp
├── tls/                  # TLS/SSL
│   ├── tls_context.hpp
│   └── tls_stream.hpp
├── rpc/                  # RPC framework
│   ├── rpc.hpp
│   ├── rpc_types.hpp
│   ├── rpc_buffer.hpp
│   ├── rpc_client.hpp
│   └── rpc_server.hpp
├── hash/                 # Hash functions
│   ├── crc32.hpp
│   ├── sha1.hpp
│   └── sha256.hpp
└── log/                  # Logging
    └── logger.hpp
```

The repository also contains `examples/` with runnable programs, `tests/` with Catch2 tests, and `tools/` with debugging utilities (`elio-pstack`, GDB/LLDB extensions).

## Next Steps

- Read [[Core Concepts]] to understand how Elio works
- Explore [[Networking]] for TCP and HTTP usage
- Check out [[Examples]] for more code samples
