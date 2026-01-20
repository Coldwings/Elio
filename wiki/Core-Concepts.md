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

coro::task<int> async_main() {
    // Your async code here
    co_return 0;
}

ELIO_ASYNC_MAIN(async_main)
```

For `async_main` functions returning `void`:

```cpp
coro::task<void> async_main() {
    co_await do_work();
    co_return;
}

ELIO_ASYNC_MAIN_VOID(async_main)
```

### Using elio::run()

For more control, use `elio::run()` directly:

```cpp
int main() {
    // Run with default thread count (hardware concurrency)
    return elio::run(async_main());
}

// Or specify thread count
int main() {
    return elio::run(async_main(), 4);  // 4 worker threads
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

// Spawn tasks
auto task = my_coroutine();
sched.spawn(task.release());

// Shutdown when done
sched.shutdown();
```

### Work Stealing

Elio uses a work-stealing scheduler for load balancing. Each worker thread has a local queue, and idle workers steal tasks from busy workers.

## I/O Context

The I/O context manages async I/O operations.

### Using the Default Context

```cpp
#include <elio/io/io_context.hpp>

// Get the global I/O context
auto& ctx = io::default_io_context();

// Associate with scheduler
sched.set_io_context(&ctx);
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
