# Elio Coroutine Library

**Elio** is a modern, high-performance C++20 coroutine library for asynchronous programming on Linux, approaching stable release. It provides stackless coroutines with virtual stack tracking, a multi-threaded work-stealing scheduler, and a foundation for efficient I/O operations.

[![CI](https://github.com/Coldwings/Elio/actions/workflows/ci.yml/badge.svg)](https://github.com/Coldwings/Elio/actions/workflows/ci.yml)
[![C++20](https://img.shields.io/badge/C%2B%2B-20-blue.svg)](https://en.cppreference.com/w/cpp/20)
[![License](https://img.shields.io/badge/license-MIT-green.svg)](LICENSE)

## Features

- **C++20 Stackless Coroutines** with `task<T>` type
- **Ergonomic Task Spawning**: `go()` for fire-and-forget, `spawn()` for joinable tasks
- **Virtual Stack Tracking** for natural exception propagation
- **Work-Stealing Scheduler** with lock-free Chase-Lev deques
- **Dynamic Thread Pool** with runtime adjustment
- **Autoscaler** for automatic worker thread scaling under load
- **Synchronization Primitives**: mutex, shared_mutex, semaphore, event, channel, spinlock, condition_variable; coroutine-aware waiters use intrusive lifetime cleanup, while cancellation support remains operation-specific
- **Timers**: sleep_for, sleep_until, yield
- **I/O Backends**: io_uring (preferred) and epoll fallback
- **Batch I/O**: Submit multiple file operations in a single syscall
- **File Helpers**: High-level async read/write/append operations
- **TCP Networking**: async client/server with connection management
- **HTTP/1.1**: full client and server implementation
- **HTTP/2**: HTTPS client built on nghttp2 with HPACK and stream handling
- **WebSocket**: bidirectional real-time communication (RFC 6455)
- **Server-Sent Events**: server-to-client event streaming
- **TLS/HTTPS**: OpenSSL-based with ALPN and certificate verification
- **Header-Only Library** for easy integration
- **Debugging Tools**: GDB/LLDB extensions and pstack-like CLI
- **Comprehensive Testing** with Catch2, ASAN, and TSAN
- **Integrated Logging** with fmtlib
- **CI/CD** with GitHub Actions

## Quick Start

### Prerequisites

- **Compiler**: GCC 12+ or Clang 15+ with C++20 support
- **OS**: Linux (kernel 5.1+ for io_uring, or any modern Linux for epoll)
- **CMake**: 3.20 or higher
- **Required for the default top-level build**: OpenSSL development files
  (TLS, HTTP, and HTTP/2 are enabled by default)
- **Optional**: liburing (for io_uring backend)
- **Optional for TCP benchmark comparison**: libuv development files
  (`libuv1-dev` on Debian/Ubuntu) for `bench_tcp_libuv`
- **Dependencies**: Automatically fetched via CMake FetchContent for source builds
  - fmtlib 10.2.1
  - Catch2 3.5.0 (for tests)
  - nghttp2 1.64.0 (for HTTP/2)
  - standalone Asio 1.30.2 (only when `ELIO_BUILD_TCP_BENCHMARKS=ON`)

The FetchContent dependencies above apply when building Elio from source. If
you consume an installed Elio package, the downstream project must also make
Elio's exported dependencies discoverable by CMake.

### Building

```bash
git clone https://github.com/Coldwings/Elio.git
cd Elio
cmake -S . -B build
cmake --build build
```

The default top-level build enables TLS, HTTP, and HTTP/2. To configure without
OpenSSL, disable the dependent protocol targets:

```bash
cmake -S . -B build-no-tls \
  -DELIO_ENABLE_TLS=OFF \
  -DELIO_ENABLE_HTTP=OFF \
  -DELIO_ENABLE_HTTP2=OFF
cmake --build build-no-tls
```

### CMake Options

```bash
# Core toggles
cmake -S . -B build -DELIO_BUILD_TESTS=ON -DELIO_BUILD_EXAMPLES=ON
cmake -S . -B build -DELIO_ENABLE_TLS=ON -DELIO_ENABLE_HTTP=ON -DELIO_ENABLE_HTTP2=ON

# Optional RDMA modules and tests
cmake -S . -B build-rdma \
  -DELIO_ENABLE_RDMA=ON \
  -DELIO_ENABLE_RDMA_CM=ON \
  -DELIO_ENABLE_RDMA_IBVERBS=ON \
  -DELIO_ENABLE_RDMA_IBVERBS_TESTS=ON \
  -DELIO_ENABLE_RDMA_CUDA=ON

# Optional TCP benchmark comparison targets
cmake -S . -B build-tcp-bench \
  -DCMAKE_BUILD_TYPE=Release \
  -DELIO_BUILD_EXAMPLES=ON \
  -DELIO_BUILD_TCP_BENCHMARKS=ON

# Warning policy for repository-local targets (tests/examples only)
cmake -S . -B build -DELIO_ENABLE_DEVELOPER_WARNINGS=ON -DELIO_WARNINGS_AS_ERRORS=ON

# Coroutine debug metadata is enabled by default; disable it for leaner builds
cmake -S . -B build -DELIO_ENABLE_DEBUG_METADATA=OFF
```

Note: strict warning flags are applied only to Elio's tests/examples targets and are not propagated through exported interface targets.
Enable RDMA, ibverbs, RDMA CM, CUDA, and TCP benchmark options only when the
corresponding system dependencies are installed. For TCP benchmark comparison
builds, install the libuv development package if you need `bench_tcp_libuv`;
without libuv, CMake skips that target.

### Install And Use As A Package

```bash
cmake --install build --prefix /your/prefix
```

Then in another CMake project:

```cmake
find_package(Elio REQUIRED)
target_link_libraries(your_target PRIVATE Elio::elio)

# Optional targets when enabled during Elio build
# Elio::elio_tls
# Elio::elio_http
# Elio::elio_http2
# Elio::elio_rdma
# Elio::elio_rdma_cm
# Elio::elio_rdma_ibverbs
# Elio::elio_rdma_cuda
```

Installed packages export only the targets that were enabled when Elio was
built. The installed config restores required dependencies during
`find_package(Elio)`: `fmt` is always required, `liburing` is required when
Elio was built with io_uring support, OpenSSL is required by the TLS/HTTP
targets, and nghttp2 is required by the HTTP/2 target. Install those packages
or pass their package locations through `CMAKE_PREFIX_PATH`/`fmt_DIR` and the
equivalent CMake hints for your environment.

RDMA package consumers should link the most specific enabled target:
`Elio::elio_rdma` for the header-only core abstraction, `Elio::elio_rdma_cm`
for the Connection Manager helper, `Elio::elio_rdma_ibverbs` for the reference
libibverbs backend, or `Elio::elio_rdma_cuda` for CUDA GPUDirect helpers. The
installed config restores the corresponding system dependencies during
`find_package(Elio)`: `librdmacm` for RDMA CM, `libibverbs` for ibverbs, and
`CUDAToolkit` for CUDA.

### Running Tests

```bash
# Run every registered CTest entry, including sanitizer-discovered tests
# and package/config checks
ctest --test-dir build --output-on-failure

# Direct non-sanitized test binary for focused local runs
./build/tests/elio_tests

# Direct sanitizer binaries
./build/tests/elio_tests_asan
./build/tests/elio_tests_tsan
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
    
    // Spawn the main task (pass callable, not invoked task)
    sched.go(main_task);
    
    // Wait for completion
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    
    // Clean shutdown
    sched.shutdown();
    
    return 0;
}
```

## Documentation

Detailed guides live in the repository `wiki/` directory and are synced to the
GitHub Wiki. Start with [Getting Started](wiki/Getting-Started.md) for setup,
[API Reference](wiki/API-Reference.md) for the public surface, and
[API Contracts](wiki/API-Contracts.md) plus
[Security Guidelines](wiki/Security-Guidelines.md) for the boundary between
Elio's library guarantees and caller responsibilities.

## Architecture Overview

### Core Components

```
elio::
├── coro::                    // Coroutine primitives
│   ├── task<T>              // Primary coroutine type
│   ├── join_handle<T>       // Handle for awaiting spawned tasks
│   ├── promise_base         // Virtual stack base class
│   ├── awaitable_base<T>    // CRTP awaitable base
│   └── frame utilities      // Virtual stack inspection
│
├── runtime::                // Scheduler and execution
│   ├── scheduler            // Work-stealing scheduler
│   ├── worker_thread        // Worker thread implementation
│   └── chase_lev_deque<T>   // Lock-free work queue
│
├── detail::                 // Internal utilities
│   └── intrusive_list<T>    // Generic intrusive doubly-linked list
│
├── sync::                   // Synchronization primitives
│   ├── mutex                // Async mutex with waiter cleanup
│   ├── shared_mutex         // Reader-writer mutex with waiter cleanup
│   ├── semaphore            // Counting semaphore with waiter cleanup
│   ├── event                // One-shot event with waiter cleanup
│   ├── channel<T>           // MPMC channel with waiter cleanup
│   ├── spinlock             // Busy-wait lock
│   └── condition_variable   // Async condition variable with waiter cleanup
│
├── time::                   // Timer utilities
│   ├── sleep_for            // Duration-based sleep
│   ├── sleep_until          // Time-point sleep
│   └── yield                // Yield execution
│
├── io::                     // I/O backend
│   ├── io_context           // Unified I/O interface
│   ├── io_uring_backend     // io_uring implementation
│   ├── io_awaitables        // Async I/O operations
│   ├── batch_read/write     // Batch file I/O (multi-segment)
│   ├── file_helpers         // High-level file operations
│   └── epoll_backend        // epoll fallback
│
├── net::                    // Networking
│   └── tcp                  // TCP client/server
│
├── http::                   // HTTP protocol
│   ├── http_client          // HTTP/HTTPS client
│   ├── http_server          // HTTP server
│   ├── h2_client            // HTTP/2 client
│   ├── h2_session           // HTTP/2 session (nghttp2)
│   ├── websocket            // WebSocket client/server
│   └── sse                  // Server-Sent Events
│
├── tls::                    // TLS/SSL support
│   └── tls_stream           // OpenSSL wrapper
│
└── log::                    // Logging infrastructure
    ├── logger               // Thread-safe logger
    └── ELIO_LOG_* macros    // Logging macros

tools/                       // Debugging tools
├── elio-pstack              // pstack-like CLI tool
├── elio-gdb.py              // GDB Python extension
├── elio_lldb.py             // LLDB import entrypoint
└── elio-lldb.py             // LLDB implementation script
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
- **Per-worker I/O context**: Each worker has its own io_uring/epoll backend for thread-safe I/O
- **Dynamic sizing**: Adjust thread count at runtime
- **Load balancing**: Automatic task distribution

## Examples

Explore the `examples/` directory for detailed examples. Core examples are
available when `ELIO_BUILD_EXAMPLES=ON`; protocol and RDMA examples also require
their corresponding feature targets.

- **Coroutine/runtime**: `hello_world.cpp`, `chained_coroutines.cpp`,
  `exception_handling.cpp`, `parallel_tasks.cpp`, `dynamic_threads.cpp`,
  `autoscaler_example.cpp`, `thread_affinity.cpp`, `signal_handling.cpp`,
  `debug_test.cpp`
- **Benchmarks**: `benchmark.cpp`, `quick_benchmark.cpp`, `microbench.cpp`,
  `io_benchmark.cpp`, `scalability_test.cpp`, `bench_channel.cpp`
- **TCP/UDS/RPC/file I/O**: `tcp_echo_server.cpp`, `tcp_echo_client.cpp`,
  `uds_echo_server.cpp`, `uds_echo_client.cpp`, `rpc_server_example.cpp`,
  `rpc_client_example.cpp`, `async_file_io.cpp`
- **HTTP/TLS protocols** (`elio_http` / `elio_http2`): `http_server.cpp`,
  `http_client.cpp`, `http2_client.cpp`, `websocket_server.cpp`,
  `websocket_client.cpp`, `sse_server.cpp`, `sse_client.cpp`
- **RDMA** (`elio_rdma`, `elio_rdma_ibverbs`, `elio_rdma_cm`,
  `elio_rdma_cuda`): `rdma_pingpong_mock.cpp`, `rdma_req_resp_ibverbs.cpp`,
  `rdma_perf.cpp`, `rdma_gpu_bw.cpp`
- **TCP benchmark comparison** (`ELIO_BUILD_TCP_BENCHMARKS=ON`):
  `bench_tcp_elio.cpp`, `bench_tcp_libuv.cpp`, `bench_tcp_asio.cpp`
  (`bench_tcp_libuv` requires system libuv development files)

Build and run examples:
```bash
cmake -S . -B build -DELIO_BUILD_EXAMPLES=ON
cmake --build build --target hello_world parallel_tasks benchmark
./build/examples/hello_world
./build/examples/parallel_tasks
./build/examples/benchmark
```

## Debugging

Elio provides debugging tools to inspect coroutine states and virtual call stacks:

```bash
# pstack-like tool for coroutines
elio-pstack <pid>              # Attach to running process
elio-pstack ./myapp core.1234  # Analyze coredump

# GDB extension
gdb -ex 'source /path/to/Elio/tools/elio-gdb.py' ./myapp
(gdb) elio list                # List all vthreads
(gdb) elio bt                  # Show all backtraces
(gdb) elio bt 42               # Show backtrace for vthread #42

# LLDB extension
lldb -o 'command script import /path/to/Elio/tools/elio_lldb.py' ./myapp
(lldb) elio list
(lldb) elio bt
```

See the [Debugging wiki page](wiki/Debugging.md) for detailed documentation.

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

// Spawn coroutines (pass callable, not invoked task)
sched.go(my_coroutine);  // fire-and-forget
// Or for joinable: auto handle = sched.go_joinable(my_coroutine);

// Dynamic thread adjustment
sched.set_thread_count(8);

// Statistics
size_t threads = sched.num_threads();
size_t pending = sched.pending_tasks();
size_t executed = sched.total_tasks_executed();

// Shutdown (waits for completion)
sched.shutdown();
```

### Task Spawning

Elio provides flexible ways to spawn concurrent tasks:

```cpp
// Fire-and-forget: spawn and don't wait for result
elio::go(some_task);

// With arguments
elio::go(task_with_args, arg1, arg2);

// Lambda with captures (safe - copied into coroutine frame)
int value = 42;
elio::go([value]() -> coro::task<void> {
    // Use captured value safely
    co_return;
});

// Joinable spawn: get a handle to await later
auto handle = elio::spawn(compute_value);
// ... do other work ...
int result = co_await handle;  // Wait and get result

// Multiple concurrent tasks
auto h1 = elio::spawn(task_a);
auto h2 = elio::spawn(task_b);
auto h3 = elio::spawn(task_c);
// All three run concurrently
int a = co_await h1;
int b = co_await h2;
int c = co_await h3;

// Check if spawned task is ready without blocking
if (handle.is_ready()) {
    // Task has completed
}
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
- **ASAN Tests**: Detect memory errors (use-after-free, buffer overflow, etc.)
- **TSAN Tests**: Detect data races and thread safety issues

```bash
# Run every registered CTest entry, including sanitizer-discovered tests
# and package/config checks
ctest --test-dir build --output-on-failure

# Run specific test suites
./build/tests/elio_tests "[scheduler]"
./build/tests/elio_tests "[integration]"

# Run sanitizer binaries directly
./build/tests/elio_tests_asan    # Memory safety
./build/tests/elio_tests_tsan    # Thread safety
```

## Performance

### Benchmark Results

Elio achieves competitive performance through careful optimization:

| Metric | Typical Value | Best Case |
|--------|---------------|-----------|
| Task Spawn | ~1300 ns | ~570 ns (pre-allocated) |
| Context Switch | ~230 ns | ~212 ns |
| Yield (1000 vthreads) | ~30 ns | ~16 ns |
| File I/O (single thread) | 1.46 μs/read | 685K IOPS |
| File I/O (4 threads) | 0.93 μs/read | 1.07M IOPS |

### Key Optimizations

- **Unconditional Wake**: Cross-thread submissions always wake the target worker; eventfd deduplication prevents lost wakes
- **io_uring Batch Submit**: Automatic batching of I/O operations
- **Coroutine Frame Pooling**: Hot path allocation ~72 ns vs cold ~250 ns
- **Lock-free Scheduling**: MPSC inbox (~5 ns) + Chase-Lev deque (~13 ns)

### Scalability

| Threads | Throughput | Speedup |
|---------|-----------|---------|
| 1 | ~18K tasks/sec | 1.0x |
| 2 | ~33K tasks/sec | 1.9x |
| 4 | ~56K tasks/sec | 3.2x |
| 8 | ~86K tasks/sec | 4.9x |

*CPU-bound workload with 100K iterations per task*

### Running Benchmarks

```bash
cd build
cmake --build . --target quick_benchmark microbench io_benchmark benchmark scalability_test

# Quick benchmark (spawn, context switch, yield)
./examples/quick_benchmark

# Microbenchmarks (individual operations)
./examples/microbench

# I/O throughput benchmark
./examples/io_benchmark

# Full benchmark suite
./examples/benchmark

# Scalability test
./examples/scalability_test
```

See [Performance Tuning Guide](wiki/Performance-Tuning.md) for optimization tips.

## Project Status

**Current Status**: Feature Complete

- Core coroutine infrastructure (task<T>, virtual stack)
- Work-stealing scheduler with dynamic thread pool
- Synchronization primitives (mutex, semaphore, event, channel)
- Timers (sleep_for, sleep_until, yield)
- I/O backends (io_uring, epoll)
- **Batch I/O** (multi-segment pread/pwrite in single syscall)
- **File Helpers** (read_file, write_file, append_file, file_exists, file_size, read_dir)
- TCP networking (client/server)
- HTTP/1.1 (client/server)
- HTTP/2 (client)
- WebSocket (client/server, RFC 6455)
- Server-Sent Events (client/server)
- TLS/HTTPS support
- Comprehensive test suite
- CI/CD pipeline

## Contributing

Contributions are welcome! See [CONTRIBUTING.md](CONTRIBUTING.md) for guidelines on
development setup, code style, testing requirements, and the pull request workflow.

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

**Status**: Approaching Stable | **Version**: 0.6.0 (unreleased) | **Last Release**: 0.5.3 (2026-07-17)
