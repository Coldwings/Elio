# Core Concepts

This page explains the fundamental concepts behind Elio's design.

## Coroutines and Tasks

Elio uses C++20 coroutines as the foundation for async programming. A coroutine is a function that can suspend and resume execution.

### The `task<T>` Type

`coro::task<T>` is the primary coroutine type in Elio:

```cpp
#include <elio/coro/task.hpp>

// A task that returns an int
coro::task<int> compute() {
    co_return 42;
}

// A task that returns nothing
coro::task<void> do_work() {
    int result = co_await compute();
    ELIO_LOG_INFO("Result: {}", result);
    co_return;
}
```

### Awaiting Tasks

Use `co_await` to wait for a task to complete:

```cpp
coro::task<void> example() {
    // Sequential execution
    int a = co_await compute();
    int b = co_await compute();
    
    ELIO_LOG_INFO("Sum: {}", a + b);
    co_return;
}
```

## Scheduler

The scheduler manages coroutine execution across multiple threads.

### Using async_main (Recommended)

The simplest way to run async code is with `ELIO_ASYNC_MAIN`:

```cpp
#include <elio/elio.hpp>

coro::task<int> async_main(int argc, char* argv[]) {
    // Access command line arguments
    if (argc > 1) {
        std::cout << "Argument: " << argv[1] << std::endl;
    }

    // Your async code here
    co_return 0;
}

ELIO_ASYNC_MAIN(async_main)
```

For `async_main` functions returning `void`:

```cpp
coro::task<void> async_main(int argc, char* argv[]) {
    co_await do_work();
    co_return;
}

ELIO_ASYNC_MAIN_VOID(async_main)
```

For simple programs without command line arguments:

```cpp
coro::task<int> async_main() {
    co_return 0;
}

ELIO_ASYNC_MAIN_NOARGS(async_main)
```

### Using elio::run()

For more control, use `elio::run()` directly:

```cpp
int main(int argc, char* argv[]) {
    // Run with default thread count (hardware concurrency)
    return elio::run(async_main(argc, argv));
}

// Or use run_config for full control
int main(int argc, char* argv[]) {
    elio::run_config config;
    config.num_threads = 4;  // 4 worker threads
    
    return elio::run(async_main(argc, argv), config);
}
```

### Manual Scheduler Control

For advanced use cases, you can manage the scheduler manually:

```cpp
#include <elio/runtime/scheduler.hpp>

// Create with N worker threads
runtime::scheduler sched(4);

// Start the scheduler
sched.start();

// Spawn tasks (new simplified API)
sched.spawn(my_coroutine());

// Shutdown when done
sched.shutdown();
```

### Task Spawning

Elio provides several ways to spawn concurrent tasks:

#### Fire-and-Forget with `go()`

Use `go()` to spawn a task that runs independently:

```cpp
coro::task<void> background_work() {
    // This runs in the background
    co_return;
}

coro::task<void> main_task() {
    // Spawn and continue immediately (don't wait)
    background_work().go();
    
    // Continue with other work...
    co_return;
}
```

#### Joinable Tasks with `spawn()`

Use `spawn()` to get a `join_handle` that lets you await the result later:

```cpp
coro::task<int> compute(int x) {
    co_return x * 2;
}

coro::task<void> parallel_example() {
    // Spawn multiple tasks concurrently
    auto h1 = compute(10).spawn();
    auto h2 = compute(20).spawn();
    auto h3 = compute(30).spawn();
    
    // All three run in parallel
    // Now wait for results
    int a = co_await h1;  // 20
    int b = co_await h2;  // 40
    int c = co_await h3;  // 60
    
    ELIO_LOG_INFO("Sum: {}", a + b + c);  // 120
    co_return;
}
```

#### Checking Completion

You can check if a spawned task is done without blocking:

```cpp
coro::task<void> poll_example() {
    auto handle = slow_operation().spawn();
    
    while (!handle.is_ready()) {
        // Do other work while waiting
        co_await do_something_else();
    }
    
    auto result = co_await handle;
    co_return;
}
```

### Work Stealing

Elio uses a work-stealing scheduler for load balancing. Each worker thread has a local queue, and idle workers steal tasks from busy workers.

### Efficient Idle Waiting

When workers have no tasks to execute, they enter an efficient sleep state instead of busy-waiting:

- **eventfd-based wake mechanism**: Each worker has an `eventfd` that external threads can signal to wake it up
- **Blocking epoll wait**: Idle workers block on `epoll_wait` with a timeout, consuming near-zero CPU
- **Automatic wake-up**: When tasks are scheduled to a worker, it is automatically woken via `eventfd`
- **IO integration**: One worker polls the IO backend (io_uring/epoll) while others sleep on their eventfd

This design ensures:
- Near-zero CPU usage when idle (< 1%)
- Fast wake-up latency when new work arrives (< 10ms)
- Efficient coordination between task scheduling and IO polling

## I/O Context

The I/O context manages async I/O operations. Each worker thread has its own I/O context for lock-free operation. In most cases, you don't need to interact with io_context directly - the library automatically uses the per-thread io_context.

### Accessing the Current Context

```cpp
#include <elio/io/io_context.hpp>

// Get the current thread's I/O context (from within a coroutine)
auto& ctx = io::current_io_context();
```

### I/O Backends

Elio supports two I/O backends:

| Backend | Kernel Version | Performance |
|---------|----------------|-------------|
| io_uring | Linux 5.1+ | Best |
| epoll | Any Linux | Good |

The backend is selected automatically at compile time based on availability.

## Awaitables

Awaitables are types that can be used with `co_await`. Elio provides several built-in awaitables:

### I/O Awaitables

```cpp
// Read from a socket
auto result = co_await stream.read(buffer, size);

// Write to a socket
auto result = co_await stream.write(data, len);

// Accept a connection
auto stream = co_await listener.accept();
```

### Timer Awaitables

```cpp
#include <elio/time/timer.hpp>

// Sleep for a duration
co_await time::sleep(io_ctx, std::chrono::seconds(1));

// Sleep until a time point
co_await time::sleep_until(io_ctx, deadline);
```

## Synchronization Primitives

Elio provides coroutine-aware synchronization primitives.

### Mutex

```cpp
#include <elio/sync/primitives.hpp>

sync::mutex mtx;

coro::task<void> critical_section() {
    auto lock = co_await mtx.lock();
    // Protected code here
    co_return;
}
```

### Shared Mutex (Read-Write Lock)

`shared_mutex` allows multiple concurrent readers or a single exclusive writer:

```cpp
sync::shared_mutex rwlock;

// Multiple readers can run concurrently
coro::task<void> reader() {
    co_await rwlock.lock_shared();
    sync::shared_lock_guard guard(rwlock);  // RAII unlock
    // Read shared data
    co_return;
}

// Writers get exclusive access
coro::task<void> writer() {
    co_await rwlock.lock();
    sync::unique_lock_guard guard(rwlock);  // RAII unlock
    // Modify shared data
    co_return;
}
```

### Condition Variable

```cpp
sync::condition_variable cv;
sync::mutex mtx;
bool ready = false;

coro::task<void> waiter() {
    auto lock = co_await mtx.lock();
    co_await cv.wait(lock, [&] { return ready; });
    // Condition met
    co_return;
}

coro::task<void> notifier() {
    {
        auto lock = co_await mtx.lock();
        ready = true;
    }
    cv.notify_one();
    co_return;
}
```

### Semaphore

```cpp
sync::semaphore sem(10);  // Max 10 concurrent

coro::task<void> limited_work() {
    co_await sem.acquire();
    // Do work
    sem.release();
    co_return;
}
```

## Cancellation

Elio provides a cooperative cancellation mechanism for async operations using `cancel_source` and `cancel_token`.

### Basic Usage

```cpp
#include <elio/coro/cancel_token.hpp>

coro::task<void> cancellable_work(coro::cancel_token token) {
    while (!token.is_cancelled()) {
        // Do work...
        
        // Cancellable sleep - returns early if cancelled
        auto result = co_await time::sleep_for(100ms, token);
        if (result == coro::cancel_result::cancelled) {
            break;
        }
    }
    co_return;
}

coro::task<void> controller() {
    coro::cancel_source source;
    
    // Start work with a token
    cancellable_work(source.get_token()).go();
    
    // Wait some time
    co_await time::sleep_for(5s);
    
    // Cancel the operation
    source.cancel();
    co_return;
}
```

### How It Works

1. **`cancel_source`** - Creates and controls cancellation state
2. **`cancel_token`** - Lightweight handle passed to operations
3. Operations periodically check `token.is_cancelled()`
4. Calling `source.cancel()` triggers all registered callbacks

### Cancellable Operations

Many Elio operations support cancellation:

```cpp
// Sleep with cancellation
auto result = co_await time::sleep_for(1s, token);

// HTTP request with cancellation
auto response = co_await client.get(url, token);

// RPC call with cancellation
auto result = co_await rpc_client->call<Method>(req, timeout, token);

// WebSocket receive with cancellation
auto msg = co_await ws_client.receive(token);

// SSE event receive with cancellation
auto event = co_await sse_client.receive(token);
```

### Implementing Cancellable Operations

Register callbacks to respond to cancellation:

```cpp
coro::task<void> custom_operation(coro::cancel_token token) {
    // Register a callback
    auto reg = token.on_cancel([&]() {
        // Cleanup or signal early exit
    });
    
    // Do work...
    
    // Registration automatically unregisters on destruction
    co_return;
}
```

## Error Handling

Elio uses `std::optional` for error handling in I/O operations. On failure, functions return `std::nullopt` and set `errno`:

```cpp
coro::task<void> handle_errors() {
    auto result = co_await stream.read(buffer, size);
    
    if (result.result > 0) {
        // Success - result.result contains bytes read
    } else if (result.result == 0) {
        // EOF - connection closed
    } else {
        // Error - result.result is negative errno
        ELIO_LOG_ERROR("Read error: {}", strerror(-result.result));
    }
    co_return;
}
```

For factory methods like `tcp_listener::bind()`, check for `std::nullopt` and use `errno` to get the error code:

```cpp
auto listener = tcp_listener::bind(ipv4_address(port), ctx);
if (!listener) {
    ELIO_LOG_ERROR("Bind failed: {}", strerror(errno));
    co_return;
}
```

## Logging

Elio includes a built-in logging system:

```cpp
#include <elio/log/macros.hpp>

ELIO_LOG_DEBUG("Debug message");
ELIO_LOG_INFO("Info: value={}", 42);
ELIO_LOG_WARNING("Warning!");
ELIO_LOG_ERROR("Error: {}", strerror(errno));

// Set log level
log::logger::instance().set_level(log::level::debug);
```

## Next Steps

- Learn about [[Networking]] for TCP and HTTP
- See [[Examples]] for complete code samples
