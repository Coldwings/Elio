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

- **Unified wake mechanism**: Each worker's I/O backend contains an eventfd that external threads can write to, waking the worker from its I/O poll
- **Configurable wait strategy**: Workers can spin before blocking (for low-latency) or block immediately (for CPU efficiency)
- **Automatic wake-up**: When tasks are scheduled to a worker, it is automatically woken via eventfd write to the I/O backend
- **IO integration**: Workers poll the IO backend (io_uring/epoll) before sleeping

This design ensures:
- Near-zero CPU usage when idle with default blocking strategy (< 1%)
- Configurable spin-before-block for low-latency workloads
- Fast wake-up latency when new work arrives
- Efficient coordination between task scheduling and IO polling

### Thread Affinity

Tasks can be pinned to specific worker threads using the affinity API. A pinned task will not be stolen by other workers.

```cpp
#include <elio/runtime/affinity.hpp>

coro::task<void> affinity_example() {
    // Bind to worker 2 and migrate there immediately
    co_await elio::runtime::set_affinity(2);

    // Bind to the current worker (prevent migration)
    co_await elio::runtime::bind_to_current_worker();

    // Remove affinity (allow free migration again)
    co_await elio::runtime::clear_affinity();

    co_return;
}
```

All three functions are also available in the `elio` namespace as convenience aliases (`elio::set_affinity`, `elio::bind_to_current_worker`, `elio::clear_affinity`).

`set_affinity` accepts an optional second parameter `migrate` (default `true`). When `true`, the coroutine is immediately rescheduled on the target worker. When `false`, the affinity is recorded but the coroutine continues on its current worker until its next suspension point.

### Design Rationale

**Why work-stealing.** A centralized task queue becomes a contention bottleneck under high concurrency. Work-stealing distributes scheduling decisions: each worker thread operates on its own local deque, and idle workers steal from busy ones. This provides automatic load balancing without requiring a central coordinator. The owner-side LIFO order also has a cache-locality benefit -- the most recently pushed task is the one most likely to have hot data in the current core's cache.

**Why Chase-Lev deque.** The Chase-Lev algorithm gives the owner thread lock-free push and pop operations with no atomic read-modify-write on the fast path. Contention only occurs when the deque has a single element and both the owner and a thief attempt to take it simultaneously. The deque also supports dynamic resizing by replacing the underlying circular buffer, which avoids the need to pre-allocate a fixed upper bound.

**Why MPSC inbox for external submissions.** Cross-thread task submissions (e.g., spawning a task onto a specific worker from another thread) go through a bounded MPSC ring buffer rather than directly into the Chase-Lev deque. This separation keeps the deque's invariants simple -- only the owner ever pushes -- and the bounded capacity with cache-line aligned slots (`alignas(64)`) eliminates false sharing between producers and the consumer.

## Virtual Stack

C++20 stackless coroutines do not maintain a call stack in the traditional sense. When a coroutine suspends, the compiler-generated frame is stored on the heap, but the chain of callers that led to that suspension point is lost. This makes debugging difficult -- tools like `gdb bt` show the scheduler's dispatch loop rather than the logical call chain of coroutines.

Elio reconstructs this information through a **virtual stack**: an intrusive linked list of `promise_base` objects connected by `parent_` pointers. Each `promise_base` constructor links itself to the current frame via the `current_frame_` thread-local, and the destructor restores the previous frame. This gives every coroutine a pointer to the coroutine that `co_await`ed it.

The overhead is minimal -- one pointer per coroutine frame, set during construction and cleared during destruction.

### What it enables

- **`elio-pstack`**: A CLI tool that attaches to a running process (or reads a coredump) and walks the virtual stack chains to print coroutine backtraces, similar to `pstack` for threads.
- **Debugger extensions**: `elio-gdb.py` and `elio-lldb.py` use the same frame linkage to implement `elio bt` (backtrace) and `elio list` (list active coroutines).
- **Exception propagation**: When a coroutine throws, `unhandled_exception()` captures it in the promise. The parent coroutine can then rethrow the exception when it `co_await`s the child's result, propagating errors up the logical call chain.

### Frame metadata

Each `promise_base` also carries debug metadata:

- **`frame_magic_`**: A constant (`0x454C494F46524D45`, ASCII "ELIOFRME") that debuggers use to validate that a memory region is a live coroutine frame.
- **`debug_id_`**: A lazily-allocated unique ID (batch-allocated per thread to avoid global contention).
- **`debug_location_`**: Source file, function name, and line number.
- **`debug_state_`**: Current state (created, running, suspended, completed, failed).

## Frame Allocator

Coroutine frames are heap-allocated by default. In workloads that create and destroy many short-lived coroutines, this can cause significant allocator contention across threads.

`frame_allocator` is a thread-local free-list pool that handles small coroutine frames (up to 256 bytes, with up to 1024 pooled slots). When a coroutine frame is allocated, the allocator checks the thread-local pool first. When freed, the frame is returned to the pool instead of the global heap.

### Cross-thread returns

Work-stealing means a coroutine may be allocated on thread A but destroyed on thread B. The frame allocator handles this with an MPSC (multi-producer, single-consumer) return queue per pool. When a frame is freed on a different thread than the one that allocated it, the frame is pushed onto the source pool's MPSC queue. The owning thread reclaims these returns in batches during subsequent allocations.

Each allocated block carries a hidden header that records its source pool ID, so the deallocator knows which pool to return it to.

### Sanitizer compatibility

Under AddressSanitizer or ThreadSanitizer, the pool is bypassed entirely -- all allocations go directly through `::operator new` and `::operator delete`. This ensures that sanitizers can accurately detect leaks, use-after-free, and data races without being confused by the pooling layer.

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

### Event

`event` is a one-shot signaling primitive. One or more coroutines wait for the event to be set:

```cpp
sync::event evt;

coro::task<void> waiter() {
    co_await evt.wait();
    // Event was set
    co_return;
}

coro::task<void> signaler() {
    // Wake all waiters
    evt.set();
    co_return;
}
```

### Channel

`channel<T>` provides a bounded, multi-producer multi-consumer queue for passing values between coroutines:

```cpp
sync::channel<int> ch(16);  // Bounded capacity of 16

coro::task<void> producer() {
    co_await ch.send(42);
    co_return;
}

coro::task<void> consumer() {
    auto value = co_await ch.receive();
    // value == 42
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

### Spinlock

`spinlock` is a lightweight, non-suspending lock for very short critical sections. It uses a TTAS (Test-and-Test-and-Set) algorithm with CPU pause instructions for efficient spinning:

```cpp
sync::spinlock sl;

coro::task<void> quick_update() {
    sl.lock();          // Spins until acquired (does not suspend coroutine)
    // Very short critical section
    sl.unlock();
    co_return;
}
```

Use `spinlock_guard` for RAII-style locking:

```cpp
sync::spinlock sl;

coro::task<void> safe_update() {
    sync::spinlock_guard guard(sl);  // Locks on construction
    // Critical section
    co_return;
    // Automatically unlocked on destruction
}
```

**When to use spinlock vs mutex:** Use `spinlock` when the critical section is very short (a few assignments or pointer swaps) and contention is low. Use `mutex` when the critical section might suspend (e.g., performing I/O) or when contention is high — `mutex` suspends the coroutine instead of busy-waiting, allowing other coroutines to run.

### Condition Variable

`condition_variable` allows coroutines to wait for a condition to become true. It works with `mutex`, `spinlock`, or in an unlocked mode:

**With mutex** (recommended for most cases):

```cpp
sync::mutex mtx;
sync::condition_variable cv;
bool ready = false;

coro::task<void> waiter() {
    co_await mtx.lock();
    while (!ready) {
        // Double co_await: outer suspends for signal, inner re-acquires mutex
        co_await co_await cv.wait(mtx);
    }
    mtx.unlock();
    co_return;
}

coro::task<void> notifier() {
    co_await mtx.lock();
    ready = true;
    mtx.unlock();
    cv.notify_one();  // Wake one waiter
    co_return;
}
```

**With spinlock:**

```cpp
sync::spinlock sl;
sync::condition_variable cv;
bool ready = false;

coro::task<void> waiter() {
    sl.lock();
    while (!ready) {
        co_await cv.wait(sl);  // Single co_await (spinlock re-lock is synchronous)
    }
    sl.unlock();
    co_return;
}
```

**Unlocked mode** (for single-worker scenarios where all coroutines run on the same thread):

```cpp
sync::condition_variable cv;
bool ready = false;

coro::task<void> waiter() {
    while (!ready) {
        co_await cv.wait_unlocked();
    }
    co_return;
}
```

`notify_one()` wakes exactly one waiting coroutine; `notify_all()` wakes all of them.

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
