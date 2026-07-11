# API Reference

This page provides a reference for Elio's public API.

## Namespaces

| Namespace | Description |
|-----------|-------------|
| `elio` | Root namespace |
| `elio::coro` | Coroutine types |
| `elio::runtime` | Scheduler and workers |
| `elio::io` | I/O context and backends |
| `elio::signal` | Signal handling with signalfd |
| `elio::net` | TCP networking |
| `elio::http` | HTTP client/server |
| `elio::tls` | TLS/SSL support |
| `elio::sync` | Synchronization primitives |
| `elio::time` | Timers |
| `elio::log` | Logging |
| `elio::hash` | Hash and checksum functions |
| `elio::rpc` | RPC framework |
| `elio::rdma` | RDMA verbs abstraction (core, header-only, `ELIO_ENABLE_RDMA`) |
| `elio::rdma_cm` | RDMA Connection Manager helpers (`ELIO_ENABLE_RDMA_CM`) |
| `elio::rdma_ibverbs` | Reference libibverbs backend + endpoint (`ELIO_ENABLE_RDMA_IBVERBS`) |
| `elio::rdma_cuda` | CUDA GPUDirect RDMA helpers (`ELIO_ENABLE_RDMA_CUDA`) |

---

## Coroutines (`elio::coro`)

### `task<T>`

The primary coroutine type.

```cpp
template<typename T = void>
class task {
public:
    using promise_type = /* implementation */;
    
    // Non-copyable, non-movable (ensures LIFO destruction order)
    task(const task&) = delete;
    task& operator=(const task&) = delete;
    task(task&&) = delete;
    task& operator=(task&&) = delete;
    
    ~task();  // Destroys the coroutine frame
    
    // Awaitable interface (use with co_await)
    bool await_ready() const noexcept;
    std::coroutine_handle<> await_suspend(std::coroutine_handle<> awaiter) noexcept;
    T await_resume();  // Returns result or rethrows exception
};
```

**Basic Usage:**
```cpp
coro::task<int> compute() {
    co_return 42;
}

coro::task<void> example() {
    int result = co_await compute();  // Direct await
    std::cout << result << std::endl;
}
```

### Spawn Functions

Elio provides free functions for spawning concurrent tasks with automatic lambda lifetime safety.

#### `elio::go()` - Fire-and-Forget

Spawn a coroutine without awaiting its result. The coroutine runs independently and self-destructs on completion.

```cpp
template<typename F, typename... Args>
void go(F&& f, Args&&... args);
```

**Example:**
```cpp
coro::task<void> background_work(int x) {
    // Do some work...
    co_return;
}

coro::task<void> main_task() {
    // Spawn and continue immediately (fire-and-forget)
    elio::go(background_work, 42);
    
    // Lambda with captures is also safe
    int value = 100;
    elio::go([value]() -> coro::task<void> {
        // 'value' is safely copied into the coroutine frame
        co_return;
    });
    
    co_return;
}
```

#### `elio::go_to()` - Pinned Fire-and-Forget

Spawn a coroutine pinned to a specific worker thread. The task's affinity is set so it cannot be stolen by other workers; if a steal occurs, the task is bounced back to the target worker.

```cpp
template<typename F, typename... Args>
void go_to(size_t worker_id, F&& f, Args&&... args);
```

**Example:**
```cpp
coro::task<void> io_handler(int fd) {
    // Handle I/O on this specific worker
    co_return;
}

coro::task<void> main_task() {
    // Pin the handler to worker 0 for cache locality
    elio::go_to(0, io_handler, fd);
    co_return;
}
```

> **Note:** `go_to()` differs from spawning with `go()` + `co_await set_affinity()`. With `go_to()`, the task is pinned *from the start* — it is placed directly on the target worker's queue and marked as non-stealable before it ever runs. With `go()` + `set_affinity()`, the task may briefly run on any worker before migrating.

#### `elio::spawn()` - Joinable

Spawn a coroutine and return a `join_handle` to await the result later.

```cpp
template<typename F, typename... Args>
auto spawn(F&& f, Args&&... args) -> coro::join_handle<T>;
```

**Example:**
```cpp
coro::task<int> compute(int x) {
    co_return x * 2;
}

coro::task<void> parallel_example() {
    // Spawn multiple tasks concurrently
    auto h1 = elio::spawn(compute, 10);
    auto h2 = elio::spawn(compute, 20);
    auto h3 = elio::spawn(compute, 30);
    
    // All three run in parallel
    // Now wait for results
    int a = co_await h1;  // 20
    int b = co_await h2;  // 40
    int c = co_await h3;  // 60
    
    ELIO_LOG_INFO("Sum: {}", a + b + c);  // 120
}
```

#### Spawn Macros

For inline coroutine expressions:

```cpp
// Fire-and-forget macro
ELIO_GO(some_async_operation());

// Spawn macro returning join_handle
auto h = ELIO_SPAWN(compute_async());
auto result = co_await h;
```

These macros expand to lambdas with `[&]` captures. Use them only when every
referenced object outlives the spawned task. For detached work that touches local
state, prefer `elio::go()` / `elio::spawn()` with an explicit capture list:

```cpp
elio::go([value = std::move(value)]() mutable -> coro::task<void> {
    co_await use_value(value);
});
```

### `join_handle<T>`

Handle for awaiting spawned tasks. Returned by `elio::spawn(...)`.

```cpp
template<typename T = void>
class join_handle {
public:
    join_handle(join_handle&& other) noexcept;
    join_handle& operator=(join_handle&& other) noexcept;
    
    // Awaitable interface (use with co_await)
    bool await_ready() const noexcept;
    bool await_suspend(std::coroutine_handle<> awaiter) noexcept;
    T await_resume();  // Returns result or rethrows exception
    
    // Check if the spawned task has completed (non-blocking)
    bool is_ready() const noexcept;
};
```

**Example:**
```cpp
coro::task<int> compute() {
    co_return 42;
}

coro::task<void> main_task() {
    // Spawn a joinable task
    auto handle = elio::spawn(compute);
    
    // Check completion without blocking
    if (!handle.is_ready()) {
        // Do other work while waiting...
    }
    
    // Await the result
    int result = co_await handle;
    std::cout << "Result: " << result << std::endl;
}
```

### `generator<T>`

Async generator for producing a stream of values via symmetric transfer. A single type serves as both the coroutine return type and the consumer interface — the producer coroutine uses `co_yield` to produce values, and the consumer retrieves them via `co_await gen.next()`.

```cpp
template<typename T>
class generator {
public:
    using promise_type = /* implementation */;

    generator();
    generator(generator&& other) noexcept;
    generator& operator=(generator&& other) noexcept;

    // Non-copyable
    generator(const generator&) = delete;
    generator& operator=(const generator&) = delete;

    /// Get the next value. Returns std::nullopt when finished.
    auto next();  // Returns awaitable<std::optional<T>>

    /// Iterate with a callback. If func returns bool, false = early break.
    template<typename F>
    auto for_each(F&& func);  // Returns awaitable<void>

    /// Check if generator is finished.
    [[nodiscard]] bool finished() const noexcept;
};

/// Range-for-like macro (zero overhead, supports break/continue)
#define ELIO_CO_FOR(var, gen)  /* ... */
```

**Basic Usage:**
```cpp
// Producer: generates values using co_yield
generator<int> produce_values(int n) {
    for (int i = 0; i < n; ++i) {
        co_yield i;
    }
}

// Consumer: three iteration styles
coro::task<void> consume() {
    // Style 1: while loop
    auto gen = produce_values(5);
    while (auto val = co_await gen.next()) {
        std::cout << *val << "\n";  // 0, 1, 2, 3, 4
    }

    // Style 2: ELIO_CO_FOR macro (range-for-like, supports break/continue)
    auto gen2 = produce_values(10);
    ELIO_CO_FOR(v, gen2) {
        std::cout << v << "\n";
        if (v >= 4) break;
    }

    // Style 3: for_each method (functional style)
    auto gen3 = produce_values(5);
    co_await gen3.for_each([](int v) {
        std::cout << v << "\n";
    });

    // for_each with early termination (return false to break)
    auto gen4 = produce_values(100);
    co_await gen4.for_each([](int v) -> bool {
        std::cout << v << "\n";
        return v < 5;  // stop when v >= 5
    });
}
```

**With Async Operations:**
```cpp
// Producer can use co_await for async I/O
generator<std::string> read_chunks(net::tcp_stream& stream) {
    char buffer[4096];

    while (stream.is_valid()) {
        auto result = co_await stream.read(buffer, sizeof(buffer));
        if (result.result <= 0) {
            break;
        }

        co_yield std::string(buffer, static_cast<std::size_t>(result.result));
    }
}

coro::task<void> process(net::tcp_stream& stream) {
    auto chunks = read_chunks(stream);
    ELIO_CO_FOR(chunk, chunks) {
        handle_chunk(chunk);
    }
}
```

**Nested Generators:**
```cpp
generator<int> inner(int n) {
    for (int i = 0; i < n; ++i) co_yield i;
}

generator<int> outer() {
    auto g1 = inner(3);
    ELIO_CO_FOR(v, g1) {
        co_yield v + 100;  // 100, 101, 102
    }
}
```

### `cancel_token` and `cancel_source`

Cooperative cancellation mechanism for async operations.

```cpp
namespace elio::coro {

/// Result of a cancellable operation
enum class cancel_result {
    completed,   ///< Operation completed normally
    cancelled    ///< Operation was cancelled
};

/// A token that can be checked for cancellation
class cancel_token {
public:
    using registration = cancel_registration;
    
    cancel_token() = default;  // Empty token (never cancelled)
    
    // Check if cancellation has been requested
    bool is_cancelled() const noexcept;
    
    // Implicit bool conversion (true if NOT cancelled)
    explicit operator bool() const noexcept;
    
    // Register a callback for cancellation
    template<typename F>
    [[nodiscard]] registration on_cancel(F&& callback) const;
    
    // Register a coroutine to resume on cancellation
    // Deprecated: Unsafe with handles suspended on io_awaitables.
    // Pass a cancel_token to the awaitable instead.
    [[deprecated]] [[nodiscard]] registration on_cancel_resume(std::coroutine_handle<> h) const;
};

/// Source for creating cancel tokens and triggering cancellation
class cancel_source {
public:
    cancel_source();  // Create new cancellation state
    
    // Get a token to pass to cancellable operations
    cancel_token get_token() const noexcept;
    
    // Request cancellation (invokes all callbacks)
    void cancel();
    
    // Check if cancelled
    bool is_cancelled() const noexcept;
};

} // namespace elio::coro
```

**Basic Example:**
```cpp
coro::task<void> cancellable_work(coro::cancel_token token) {
    while (!token.is_cancelled()) {
        // Do some work...
        
        // Cancellable sleep
        auto result = co_await time::sleep_for(100ms, token);
        if (result == coro::cancel_result::cancelled) {
            break;  // Exit early
        }
    }
}

coro::task<void> controller() {
    coro::cancel_source source;
    
    // Start work with token
    elio::go(cancellable_work, source.get_token());
    
    // Later, cancel
    co_await time::sleep_for(5s);
    source.cancel();
}
```

**Supported Cancellable Operations:**

| Operation | Usage |
|-----------|-------|
| `time::sleep_for()` | `co_await sleep_for(duration, token)` |
| `rpc_client::call()` | `co_await client->call<Method>(req, timeout, token)` |
| `http::client::get()` | `co_await client.get(url, token)` |
| `websocket::ws_client::connect()` | `co_await client.connect(url, token)` |
| `websocket::ws_client::receive()` | `co_await client.receive(token)` |
| `sse::sse_client::connect()` | `co_await client.connect(url, token)` |
| `sse::sse_client::receive()` | `co_await client.receive(token)` |

RPC client cancellation returns `rpc_error::cancelled` and, after a request has
been written, sends a best-effort cancel frame so context-aware server handlers
can observe `rpc_context::cancel_token`. HTTP, WebSocket, and SSE client
cancellation is propagated into pending socket operations. A token passed to
`connect()` or an HTTP request can abort TCP connect, TLS handshake, request
write, and response-header/body reads. A token passed to WebSocket or SSE
`receive()` can abort a pending frame/event read. Cancelled client operations
return the normal failure shape (`std::nullopt` or `false`) and set `errno` to
`ECANCELED`.

---

## Runtime (`elio::runtime`)

### `scheduler`

Manages coroutine execution across worker threads.

```cpp
class scheduler {
public:
    // Create scheduler with worker threads, wait strategy, and blocking pool size
    explicit scheduler(size_t num_threads = std::thread::hardware_concurrency(),
                       wait_strategy strategy = wait_strategy::blocking(),
                       size_t blocking_threads = 4);
    ~scheduler();
    
    // Start worker threads
    void start();
    
    // Gracefully wait for tracked tasks, then stop workers.
    // Returns true if all tracked work drained before the timeout.
    bool shutdown(std::chrono::milliseconds timeout = std::chrono::milliseconds::max());

    // Stop workers immediately. Suspended I/O may be orphaned.
    void shutdown_force();

    // Inspect or wait for tracked work
    size_t active_tasks() const noexcept;
    bool wait_for_idle(std::chrono::milliseconds timeout = std::chrono::milliseconds::max());
    
    // Spawn a coroutine for execution
    void spawn(std::coroutine_handle<> handle);
    bool try_spawn(std::coroutine_handle<> handle);
    
    // Spawn a task directly (convenience overload)
    template<typename Task>
    void spawn(Task&& t);  // Accepts any type with release() method

    // High-level scheduler-owned spawning APIs
    template<typename F, typename... Args>
    void go(F&& f, Args&&... args);
    template<typename F, typename... Args>
    void go_to(size_t worker_id, F&& f, Args&&... args);
    template<typename F, typename... Args>
    coro::join_handle</* task value */> go_joinable(F&& f, Args&&... args);
    template<typename F, typename... Args>
    coro::join_handle</* task value */> go_joinable_to(size_t worker_id, F&& f, Args&&... args);
    void spawn_to(size_t worker_id, std::coroutine_handle<> handle);

    // Get number of worker threads
    size_t num_threads(std::memory_order order = std::memory_order_relaxed) const noexcept;

    // Get total pending tasks across all workers
    size_t pending_tasks() const noexcept;

    // Get total tasks executed across all workers
    size_t total_tasks_executed() const noexcept;

    // Get tasks executed by a specific worker
    size_t worker_tasks_executed(size_t worker_id) const noexcept;

    // Get steal counters
    size_t total_steals_executed() const noexcept;
    size_t worker_steals_executed(size_t worker_id) const noexcept;

    // Check scheduler state
    bool is_running() const noexcept;
    bool is_paused() const noexcept;

    // Pause/resume task execution
    void pause();
    void resume();

    // Dynamically resize the thread pool
    void set_thread_count(size_t count);

    // Get the current scheduler (thread-local)
    static scheduler* current() noexcept;

    // Advanced accessors
    worker_thread* get_worker(size_t index);
    const wait_strategy& get_wait_strategy() const noexcept;
    blocking_pool* get_blocking_pool() noexcept;

    // Unhandled exception reporting for detached tasks and when_any losers
    using unhandled_exception_handler = std::function<void(std::exception_ptr)>;
    void set_unhandled_exception_handler(unhandled_exception_handler handler);
    const unhandled_exception_handler* get_unhandled_exception_handler() const noexcept;
    void report_unhandled_exception(std::exception_ptr ex) noexcept;
};
```

`shutdown()` is the graceful path: it waits for tasks spawned through
`go()`, `go_to()`, `go_joinable()`, `go_joinable_to()`, or `elio::run()` to
finish, including work suspended on scheduler-owned I/O, and returns whether the
drain completed before the timeout. Use `shutdown_force()` only when immediate
teardown is required.

**Example:**
```cpp
runtime::scheduler sched(4);
sched.start();

// Spawn tasks directly (pass callable, not invoked task)
sched.go(my_coroutine);

sched.shutdown();
```

### `worker_thread`

Individual worker that executes tasks. Workers use a unified idle mechanism where both I/O completions and task submissions wake the same poll wait.

```cpp
class worker_thread {
public:
    // Schedule a task to this worker (thread-safe, wakes worker if sleeping)
    void schedule(std::coroutine_handle<> handle);

    // Schedule from owner thread (faster, no wake needed)
    void schedule_local(std::coroutine_handle<> handle);

    // Wake this worker if sleeping (called automatically by schedule())
    void wake() noexcept;

    // Get/set the wait strategy for this worker
    const wait_strategy& get_wait_strategy() const noexcept;
    void set_wait_strategy(wait_strategy strategy) noexcept;

    // Get worker ID
    size_t worker_id() const noexcept;

    // Check if running
    bool is_running() const noexcept;

    // Get current worker (thread-local)
    static worker_thread* current() noexcept;
};
```

**Idle Behavior:**
- Workers block efficiently on I/O poll (with eventfd wake support) when no tasks are available
- Optional spin phase before blocking (configurable via `wait_strategy`)
- When a task is scheduled via `schedule()`, the worker is automatically woken
- Results in near-zero CPU usage (< 1%) when idle with default blocking strategy

### `wait_strategy`

Configuration for how workers wait when idle.

```cpp
struct wait_strategy {
    size_t spin_iterations = 0;  // Spin count before blocking (0 = pure blocking)
    bool spin_yield = false;     // Yield during spin (true = friendlier to other threads)

    // Preset strategies
    static constexpr wait_strategy blocking() noexcept;      // Default: pure blocking
    static constexpr wait_strategy spinning(size_t n) noexcept;  // Spin with pause
    static constexpr wait_strategy hybrid(size_t n) noexcept;    // Spin with yield, then block
    static constexpr wait_strategy aggressive(size_t n = 1000) noexcept;  // More spinning
};
```

**Example:**
```cpp
// Low-latency scheduler with hybrid waiting
scheduler sched(4, wait_strategy::hybrid(1000));

// Ultra-low latency with dedicated CPUs
scheduler sched(4, wait_strategy::spinning(2000));
```

### `run_config`

Configuration for running async tasks.

```cpp
struct run_config {
    size_t num_threads = 0;           // 0 = hardware concurrency
    size_t blocking_threads = 4;      // Blocking thread pool size
    std::chrono::milliseconds shutdown_timeout = std::chrono::milliseconds::max();
};
```

### `run()`

Run a coroutine to completion.

```cpp
// Run callable returning task with configuration
template<typename F>
auto run(F&& f, const run_config& config = {}) -> task_value_t<invoke_result_t<F>>;

// Run callable with arguments and config first
template<typename F, typename... Args>
auto run(const run_config& config, F&& f, Args&&... args) -> task_value_t<invoke_result_t<F, Args...>>;

// Run callable with arguments (no config)
template<typename F, typename Arg0, typename... Args>
auto run(F&& f, Arg0&& arg0, Args&&... args) -> task_value_t<invoke_result_t<F, Arg0, Args...>>;
```

**Example:**
```cpp
coro::task<int> async_main(int argc, char* argv[]) {
    co_return 42;
}

int main(int argc, char* argv[]) {
    return elio::run(async_main, argc, argv);
}

// With configuration
int main(int argc, char* argv[]) {
    elio::run_config config;
    config.num_threads = 4;
    return elio::run(config, async_main, argc, argv);
}
```

### `ELIO_ASYNC_MAIN` Macro

A single `ELIO_ASYNC_MAIN(func)` macro handles all four signature combinations via compile-time dispatch:

| Supported signature | Description |
|---------------------|-------------|
| `task<int>(int, char**)` | With args, returns exit code |
| `task<void>(int, char**)` | With args, always exits 0 |
| `task<int>()` | No args, returns exit code |
| `task<void>()` | No args, always exits 0 |

**Example:**
```cpp
coro::task<int> async_main(int argc, char* argv[]) {
    if (argc < 2) {
        std::cerr << "Usage: " << argv[0] << " <arg>\n";
        co_return 1;
    }
    co_await do_work(argv[1]);
    co_return 0;
}

ELIO_ASYNC_MAIN(async_main)
```

---

## Server Lifecycle (`elio`)

### `serve()`

Run a server until a shutdown signal is received.

```cpp
// Serve a single server with graceful shutdown
template<typename Server, typename ListenFunc>
    requires std::invocable<ListenFunc>
coro::task<void> serve(Server& server, ListenFunc listen_func,
                       std::initializer_list<int> signals = {SIGINT, SIGTERM});
```

The function:
1. Spawns the listen function in the background
2. Waits for a shutdown signal (SIGINT or SIGTERM by default)
3. Calls `server.stop()` when signal is received
4. Waits for the listen task to complete

For process-directed signals to be consumed by `signalfd`, block the same
shutdown signals before scheduler threads are created. Use an explicit
`main()` that calls `signal_set::block_all_threads()` before `elio::run()`;
`ELIO_ASYNC_MAIN` does not mask the calling thread.

**Example:**
```cpp
coro::task<int> async_main(int argc, char* argv[]) {
    http::router r;
    r.get("/", handler);

    http::server srv(r);

    // serve() listens, waits for masked shutdown signals, and stops cleanly
    co_await elio::serve(srv, [&]() { return srv.listen(addr); });

    co_return 0;
}

int main(int argc, char* argv[]) {
    elio::signal::signal_set shutdown_signals(elio::default_shutdown_signals);
    shutdown_signals.block_all_threads();
    return elio::run(async_main, argc, argv);
}
```

### `serve_all()`

Run multiple servers until shutdown.

```cpp
template<typename... Servers, typename... ListenFuncs>
coro::task<void> serve_all(std::tuple<Servers&...> servers,
                           std::tuple<ListenFuncs...> listen_funcs,
                           std::initializer_list<int> signals = {SIGINT, SIGTERM});
```

**Example:**
```cpp
coro::task<void> run_servers() {
    http::server http_srv(http_router);
    websocket::ws_server ws_srv(ws_router);

    co_await elio::serve_all(
        std::tie(http_srv, ws_srv),
        std::make_tuple(
            [&]() { return http_srv.listen(http_addr); },
            [&]() { return ws_srv.listen(ws_addr); }
        )
    );
}

int main() {
    elio::signal::signal_set shutdown_signals(elio::default_shutdown_signals);
    shutdown_signals.block_all_threads();
    return elio::run(run_servers);
}
```

### `wait_shutdown_signal()`

Wait for shutdown signals without managing a server.

```cpp
coro::task<signal::signal_info> wait_shutdown_signal(
    std::initializer_list<int> signals = {SIGINT, SIGTERM});
```

**Example:**
```cpp
coro::task<void> custom_server_loop() {
    // Start server tasks...

    auto sig = co_await elio::wait_shutdown_signal();
    ELIO_LOG_INFO("Received {}, shutting down...", sig.full_name());

    // Custom shutdown logic...
}
```

---

## Thread Affinity (`elio::runtime`)

Thread affinity allows you to bind vthreads (coroutines) to specific worker threads, preventing work stealing and ensuring they run on a designated thread.

### Constants

```cpp
// Constant indicating no affinity (vthread can migrate freely)
inline constexpr size_t NO_AFFINITY = std::numeric_limits<size_t>::max();
```

### `current_worker_id()`

Get the current worker thread ID.

```cpp
size_t current_worker_id() noexcept;
```

Returns the worker ID if called from a worker thread, or `NO_AFFINITY` if called from outside the scheduler.

### `set_affinity()`

Bind the current vthread to a specific worker thread.

```cpp
auto set_affinity(size_t worker_id, bool migrate = true);
```

- `worker_id`: The worker thread to bind to
- `migrate`: If true (default), migrate to the target worker immediately

**Example:**
```cpp
coro::task<void> pinned_task() {
    // Bind to worker 0 and migrate there
    co_await set_affinity(0);
    
    // Now all subsequent code runs on worker 0
    // Work stealing is prevented for this vthread
    co_return;
}
```

### `clear_affinity()`

Remove affinity binding, allowing the vthread to migrate freely.

```cpp
auto clear_affinity();
```

**Example:**
```cpp
coro::task<void> temporary_pin() {
    co_await set_affinity(2);
    // Critical section on worker 2...
    
    co_await clear_affinity();
    // Can now migrate to any worker
    co_return;
}
```

### `bind_to_current_worker()`

Bind the vthread to whatever worker it's currently running on.

```cpp
auto bind_to_current_worker();
```

**Example:**
```cpp
coro::task<void> stay_here() {
    // Pin to current worker, wherever we are
    co_await bind_to_current_worker();
    
    // Will not migrate for rest of execution
    co_return;
}
```

### Promise Base Affinity Methods

The `promise_base` class provides direct access to affinity state:

```cpp
class promise_base {
public:
    // Get current affinity (NO_AFFINITY if not set)
    size_t affinity() const noexcept;
    
    // Set affinity to a specific worker
    void set_affinity(size_t worker_id) noexcept;
    
    // Check if affinity is set
    bool has_affinity() const noexcept;
    
    // Clear affinity (allow migration)
    void clear_affinity() noexcept;
};
```

---

## I/O (`elio::io`)

### `io_context`

Manages async I/O operations.

```cpp
class io_context {
public:
    io_context();
    ~io_context();
    
    // Poll for I/O completions (with optional timeout)
    int poll(std::chrono::milliseconds timeout = std::chrono::milliseconds::zero());
    
    // Check if there are pending operations
    bool has_pending() const noexcept;
    
    // Get the I/O backend
    io_backend& backend() noexcept;
};

// Get the current scheduler worker's I/O context, or the global fallback
// outside a scheduler worker.
io_context& current_io_context() noexcept;

// Get the default global I/O context.
io_context& default_io_context();
```

### `io_result`

Result of an I/O operation.

```cpp
struct io_result {
    int result;  // Bytes transferred or negative errno
    int flags;   // Backend-specific flags
};
```

### Async I/O Awaits

```cpp
// Read from fd
auto result = co_await async_read(fd, buffer, length, offset);

// Write to fd
auto result = co_await async_write(fd, data, length, offset);

// Scatter-gather read
auto result = co_await async_readv(fd, iovecs, count);

// Scatter-gather write
auto result = co_await async_writev(fd, iovecs, count);

// Recv/Send (sockets)
auto result = co_await async_recv(fd, buffer, length, flags);
auto result = co_await async_send(fd, buffer, length, flags);

// Accept. The accepted fd is returned in io_result::result; peer storage is
// populated through the addr/addrlen pointers when provided.
struct sockaddr_storage peer_addr{};
socklen_t peer_len = sizeof(peer_addr);
auto accept_result = co_await async_accept(
    listen_fd,
    reinterpret_cast<struct sockaddr*>(&peer_addr),
    &peer_len);
if (accept_result.result >= 0) {
    int client_fd = accept_result.result;
    // peer_addr contains the peer address.
}

// Connect
auto result = co_await async_connect(fd, addr, addrlen);

// Close
auto result = co_await async_close(fd);

// Poll
auto result = co_await async_poll_read(fd);
auto result = co_await async_poll_write(fd);
```

### Batch I/O

Submit multiple file operations in a single `io_uring_submit()` syscall.

```cpp
// Batch read: read multiple file regions at once
struct batch_read_segment {
    int64_t offset;   // File offset (-1 for current position)
    void* buffer;     // Destination buffer
    size_t length;    // Bytes to read
};

std::vector<batch_read_segment> segments = { ... };
auto results = co_await batch_read(fd, segments);
// results[i] > 0: bytes read; results[i] < 0: -errno

// Batch write: write multiple file regions at once
struct batch_write_segment {
    int64_t offset;      // File offset (-1 for current position)
    const void* buffer;  // Source data
    size_t length;       // Bytes to write
};

std::vector<batch_write_segment> segments = { ... };
auto results = co_await batch_write(fd, segments);
```

**How it works:**
1. All SQEs are prepared in the io_uring submission queue
2. Single `io_uring_submit()` syscall dispatches all operations
3. Each completion is tracked via tagged `user_data` encoding
4. Results are returned in a `std::vector<int>` matching segment order

**Fallback:** When io_uring is unavailable (epoll backend), falls back to sequential synchronous `pread`/`pwrite`.

### File Helpers

High-level coroutine functions for common file operations:

```cpp
// Read entire file into a string
std::optional<std::string> content = co_await read_file("/path/to/file.txt");

// Write string to file (creates/truncates)
bool ok = co_await write_file("/path/to/file.txt", "Hello, World!");

// Append to file (creates if not exists)
bool ok = co_await append_file("/path/to/log.txt", "New entry\n");

// File metadata (synchronous, no coroutine needed)
bool exists = file_exists("/path/to/file.txt");
std::optional<int64_t> size = file_size("/path/to/file.txt");

// Read directory
std::optional<std::vector<dir_entry>> entries = read_dir("/path/to/dir");

struct dir_entry {
    std::string name;   // Filename
    bool is_dir;        // Is directory
    bool is_file;       // Is regular file
    bool is_symlink;    // Is symbolic link
};
```

**Chunking strategy:** Large files are read/written in 1MB chunks to avoid excessive single-request memory allocation.

---

## Signal Handling (`elio::signal`)

Coroutine-friendly signal handling using Linux signalfd.

### `signal_set`

Manages a set of signals.

```cpp
class signal_set {
public:
    signal_set();                                    // Empty set
    signal_set(std::initializer_list<int> signals); // From list
    
    signal_set& add(int signo);     // Add signal (chainable)
    signal_set& remove(int signo);  // Remove signal
    signal_set& clear();            // Clear all signals
    signal_set& fill();             // Add all signals
    
    bool contains(int signo) const; // Check membership
    
    const sigset_t& mask() const;   // Get underlying mask
    
    // Signal mask operations
    bool block(sigset_t* old_mask = nullptr) const;  // Block for thread
    bool unblock() const;                             // Unblock for thread
    bool set_mask(sigset_t* old_mask = nullptr) const;
    // Blocks the current thread. Call before creating workers so they inherit it.
    bool block_all_threads() const;
};
```

### `signal_fd`

Async-friendly signalfd wrapper.

```cpp
class signal_fd {
public:
    // Create signalfd (auto_block=true blocks signals on the calling thread)
    explicit signal_fd(
        const signal_set& signals,
        io::io_context& ctx = io::current_io_context(),
        bool auto_block = true);
    
    signal_fd(signal_fd&& other) noexcept;
    signal_fd& operator=(signal_fd&& other) noexcept;
    
    int fd() const noexcept;             // Get file descriptor
    bool valid() const noexcept;         // Check if valid
    explicit operator bool() const;       // Bool conversion
    
    const signal_set& signals() const;   // Get signal set
    
    // Wait for signal (awaitable)
    /* awaitable */ wait();              // Returns std::optional<signal_info>
    
    // Non-blocking read
    std::optional<signal_info> try_read();
    
    // Update the signal set
    bool update(const signal_set& new_signals, bool block = true);
    
    // Restore original signal mask
    bool restore_mask() noexcept;
    
    void close();  // Close explicitly
};
```

### `signal_info`

Information about a received signal.

```cpp
struct signal_info {
    int signo;              // Signal number
    int32_t errno_value;    // Error number (if applicable)
    int32_t code;           // Signal code (SI_USER, SI_KERNEL, etc.)
    uint32_t pid;           // PID of sending process
    uint32_t uid;           // UID of sending process
    int32_t status;         // Exit status (for SIGCHLD)
    
    const char* name() const;      // "INT", "TERM", etc.
    std::string full_name() const; // "SIGINT", "SIGTERM", etc.
};
```

### `signal_block_guard`

RAII guard for temporary signal blocking.

```cpp
class signal_block_guard {
public:
    explicit signal_block_guard(const signal_set& signals);
    ~signal_block_guard();  // Restores original mask
};
```

### Utility Functions

```cpp
// Wait for signals (convenience, creates temporary signal_fd)
coro::task<signal_info> wait_signal(
    const signal_set& signals,
    io::io_context& ctx = io::current_io_context(),
    bool auto_block = true);

coro::task<signal_info> wait_signal(
    int signo,
    io::io_context& ctx = io::current_io_context());

// Signal name/number conversion
const char* signal_name(int signo);        // SIGINT -> "INT"
int signal_number(const char* name);       // "SIGINT" or "INT" -> 2
```

**Example:**
```cpp
#include <elio/elio.hpp>

using namespace elio::signal;

std::atomic<bool> g_running{true};

coro::task<void> signal_handler_task() {
    signal_set sigs{SIGINT, SIGTERM};
    signal_fd sigfd(sigs);
    
    auto info = co_await sigfd.wait();
    if (info) {
        ELIO_LOG_INFO("Received: {}", info->full_name());
    }
    g_running = false;
    co_return;
}

int main() {
    // Block signals BEFORE creating threads
    signal_set sigs{SIGINT, SIGTERM};
    sigs.block_all_threads();
    
    runtime::scheduler sched(4);
    sched.start();
    
    sched.go(signal_handler_task);
    
    // ... spawn other tasks ...
    
    while (g_running) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    
    sched.shutdown();
}
```

---

## Networking (`elio::net`)

### `ipv4_address`

IPv4 address with port.

```cpp
struct ipv4_address {
    uint32_t addr = INADDR_ANY;
    uint16_t port = 0;
    
    ipv4_address() = default;
    explicit ipv4_address(uint16_t p);
    ipv4_address(std::string_view ip, uint16_t p);
    
    // String representation
    std::string to_string() const;
};
```

### `tcp_listener`

TCP server socket.

```cpp
class tcp_listener {
public:
    // Bind to address (returns std::nullopt on error, check errno)
    static std::optional<tcp_listener> bind(
        const ipv4_address& addr,
        const tcp_options& opts = {}
    );
    static std::optional<tcp_listener> bind(
        const ipv6_address& addr,
        const tcp_options& opts = {}
    );
    static std::optional<tcp_listener> bind(
        const socket_address& addr,
        const tcp_options& opts = {}
    );
    
    // Accept a connection (awaitable, returns std::optional<tcp_stream>)
    /* awaitable */ accept();
    
    // Get file descriptor
    int fd() const noexcept;
    
    // Get local address
    const socket_address& local_address() const noexcept;
};
```

### `tcp_stream`

TCP connection.

```cpp
class tcp_stream {
public:
    tcp_stream(tcp_stream&& other) noexcept;
    
    // Read data (awaitable)
    /* awaitable */ read(void* buffer, size_t size);
    /* awaitable */ read(void* buffer, size_t size, coro::cancel_token token);
    
    // Write data (awaitable)
    /* awaitable */ write(const void* data, size_t size);
    /* awaitable */ write(const void* data, size_t size, coro::cancel_token token);

    // Exact-length helpers (awaitable)
    /* awaitable */ read_exactly(void* buffer, size_t size);
    /* awaitable */ read_exactly(void* buffer, size_t size, coro::cancel_token token);
    /* awaitable */ write_exactly(const void* data, size_t size);
    /* awaitable */ write_exactly(const void* data, size_t size, coro::cancel_token token);
    
    // Scatter-gather write (awaitable) - writes multiple buffers atomically
    /* awaitable */ writev(struct iovec* iovecs, size_t count);
    
    // Poll for readability (awaitable)
    /* awaitable */ poll_read();
    
    // Poll for writability (awaitable)
    /* awaitable */ poll_write();
    
    // Get file descriptor
    int fd() const noexcept;
    
    // Get peer address
    std::optional<socket_address> peer_address() const;
};

// peer_address() returns the generic socket_address wrapper so callers can
// inspect IPv4 or IPv6 peers.

// Connect to address (awaitable, returns std::optional<tcp_stream>)
/* awaitable */ tcp_connect(const ipv4_address& addr, const tcp_options& opts = {});
/* awaitable */ tcp_connect(const ipv4_address& addr,
                            coro::cancel_token token,
                            const tcp_options& opts = {});
/* awaitable */ tcp_connect(const ipv6_address& addr, const tcp_options& opts = {});
/* awaitable */ tcp_connect(const ipv6_address& addr,
                            coro::cancel_token token,
                            const tcp_options& opts = {});
/* awaitable */ tcp_connect(const socket_address& addr, const tcp_options& opts = {});
/* awaitable */ tcp_connect(const socket_address& addr,
                            coro::cancel_token token,
                            const tcp_options& opts = {});
```

### Unix Domain Sockets

Local Unix Domain Socket networking.

```cpp
struct uds_options {
    bool reuse_addr = false;
    int recv_buffer = 0;
    int send_buffer = 0;
    int backlog = 128;
    bool unlink_on_bind = true;
};

struct unix_address {
    explicit unix_address(std::string_view path);

    // Linux abstract socket address (does not create a filesystem entry)
    static unix_address abstract(std::string_view name);

    bool is_abstract() const;
    std::string to_string() const;
};

class uds_listener {
public:
    // Bind to filesystem or abstract UDS address
    static std::optional<uds_listener> bind(
        const unix_address& addr,
        const uds_options& opts = {}
    );

    // Accept a connection (awaitable, returns std::optional<uds_stream>)
    /* awaitable */ accept();
    /* awaitable */ accept(coro::cancel_token token);

    int fd() const noexcept;
    const unix_address& local_address() const noexcept;
};

class uds_stream {
public:
    uds_stream(uds_stream&& other) noexcept;

    // Read data (awaitable)
    /* awaitable */ read(void* buffer, size_t size);

    // Write data (awaitable)
    /* awaitable */ write(const void* data, size_t size);

    // Exact-length helpers (awaitable)
    /* awaitable */ read_exactly(void* buffer, size_t size);
    /* awaitable */ write_exactly(const void* data, size_t size);

    // Scatter-gather write and readiness polling
    /* awaitable */ writev(struct iovec* iovecs, size_t count);
    /* awaitable */ poll_read();
    /* awaitable */ poll_read(coro::cancel_token token);
    /* awaitable */ poll_write();
    /* awaitable */ poll_write(coro::cancel_token token);

    // Socket metadata and Linux credential passing
    int fd() const noexcept;
    std::optional<unix_address> peer_address() const;
    bool set_pass_credentials(bool enable);
};

// Connect to UDS address/path (awaitable, returns std::optional<uds_stream>)
/* awaitable */ uds_connect(const unix_address& addr);
/* awaitable */ uds_connect(std::string_view path);
```

UDS streams share the same concurrency contract as TCP streams: one reader and
one writer may operate concurrently, but multiple concurrent reads, multiple
concurrent writes, or a read racing with `close()` require external
serialization.

---

## HTTP (`elio::http`)

### `client`

HTTP client with connection pooling.

```cpp
class client {
public:
    client();
    explicit client(const client_config& config);

    // GET request (awaitable)
    /* awaitable */ get(std::string_view url);
    /* awaitable */ get(std::string_view url, coro::cancel_token token);

    // POST request (awaitable)
    /* awaitable */ post(std::string_view url,
                         std::string_view body,
                         std::string_view content_type = mime::application_form_urlencoded);
    /* awaitable */ post(std::string_view url,
                         std::string_view body,
                         coro::cancel_token token,
                         std::string_view content_type = mime::application_form_urlencoded);

    // PUT request (awaitable)
    /* awaitable */ put(std::string_view url,
                        std::string_view body,
                        std::string_view content_type = mime::application_json);
    /* awaitable */ put(std::string_view url,
                        std::string_view body,
                        coro::cancel_token token,
                        std::string_view content_type = mime::application_json);

    // DELETE request (awaitable)
    /* awaitable */ del(std::string_view url);
    /* awaitable */ del(std::string_view url, coro::cancel_token token);

    // PATCH request (awaitable)
    /* awaitable */ patch(std::string_view url,
                          std::string_view body,
                          std::string_view content_type = mime::application_json);
    /* awaitable */ patch(std::string_view url,
                          std::string_view body,
                          coro::cancel_token token,
                          std::string_view content_type = mime::application_json);

    // HEAD request (awaitable)
    /* awaitable */ head(std::string_view url);
    /* awaitable */ head(std::string_view url, coro::cancel_token token);

    // Send custom request (awaitable)
    /* awaitable */ send(request& req, const url& target);
    /* awaitable */ send(request& req, const url& target, coro::cancel_token token);

    // Configure TLS and client options
    tls::tls_context& tls_context() noexcept;
    client_config& config() noexcept;
    const client_config& config() const noexcept;
};

// Convenience functions for one-off requests
/* awaitable */ get(std::string_view url);
/* awaitable */ get(std::string_view url, coro::cancel_token token);
/* awaitable */ post(std::string_view url,
                     std::string_view body,
                     std::string_view content_type = mime::application_form_urlencoded);
/* awaitable */ post(std::string_view url,
                     std::string_view body,
                     coro::cancel_token token,
                     std::string_view content_type = mime::application_form_urlencoded);
```

### `base_client_config`

Shared by `http::client_config`, `websocket::client_config`, and
`sse::client_config`.

```cpp
struct base_client_config {
    std::chrono::seconds connect_timeout{10};
    std::chrono::seconds read_timeout{30};
    size_t read_buffer_size = 8192;
    std::string user_agent;
    bool verify_certificate = true;
    net::resolve_options resolve_options = net::default_cached_resolve_options();
    bool rotate_resolved_addresses = true;
    size_t max_headers = 100;
    size_t max_header_size = 8192;
};
```

- `connect_timeout`: TCP connect and TLS handshake deadline. `<=0` disables it.
- `read_timeout`: Request/response, WebSocket upgrade, or SSE response header
  read deadline depending on the client. `<=0` disables it.
- `read_buffer_size`: Per-client read buffer size.
- `user_agent`: User-Agent header; empty disables the header.
- `verify_certificate`: TLS certificate verification policy.
- `resolve_options`: DNS resolution and cache behavior.
- `rotate_resolved_addresses`: Rotate the starting address across DNS results.
- `max_headers`: Maximum response headers accepted by parsers.
- `max_header_size`: Maximum size of one response header line in bytes.

### `client_config`

```cpp
struct client_config : base_client_config {
    size_t max_redirects = 5;
    bool follow_redirects = true;
    size_t max_connections_per_host = 6;
    std::chrono::seconds pool_idle_timeout{60};
    size_t max_response_size = 16 * 1024 * 1024;
    // Inherits all base_client_config fields.
};
```

`websocket::client_config` and `sse::client_config` also inherit
`base_client_config`, including timeout, read-buffer, TLS verification, DNS
resolution/cache, address rotation, and response-header limit settings.

Cancellation tokens are independent from configured deadlines. Passing a token
to HTTP, WebSocket, or SSE client operations cancels the underlying pending I/O
instead of only checking the token between I/O calls.

### `server_config`

```cpp
struct server_config {
    size_t max_request_size = 10 * 1024 * 1024;
    size_t read_buffer_size = 8192;
    std::chrono::seconds keep_alive_timeout{30};
    size_t max_keep_alive_requests = 100;
    bool enable_logging = true;
    size_t max_headers = 100;
    size_t max_header_size = 8192;
};
```

`keep_alive_timeout` bounds each incoming request handled by `http::server`
and the HTTP upgrade request read handled by `websocket::ws_server`. For
`server::listen_tls()` and `websocket::ws_server::listen_tls()`, it also bounds
the inbound TLS handshake. A value less than or equal to zero disables these
server-side deadlines.

### `request`

HTTP request message.

```cpp
class request {
public:
    request(method m, std::string_view path);
    
    void set_method(method m) noexcept;
    void set_path(std::string_view path);
    void set_query(std::string_view query);
    void set_header(std::string_view name, std::string_view value);
    void set_body(std::string_view body);
    void set_body(std::string&& body);
    void set_host(std::string_view host);
    void set_content_type(std::string_view type);
    
    method get_method() const noexcept;
    std::string_view path() const noexcept;
    std::string_view query() const noexcept;
    std::string_view version() const noexcept;
    std::string_view header(std::string_view name) const;
    std::string_view body() const noexcept;
    std::string_view host() const;
    std::string_view content_type() const;
};
```

`set_path()` and `set_query()` validate request-target components and throw
`std::invalid_argument` for invalid control characters or spaces.

### `response`

HTTP response message.

```cpp
class response {
public:
    uint16_t status_code() const noexcept;
    status get_status() const noexcept;
    
    std::string_view header(std::string_view name) const;
    std::string_view content_type() const;
    std::string_view body() const noexcept;
    
    void set_status(status s) noexcept;
    void set_header(std::string_view name, std::string_view value);
    void set_body(std::string_view body);
    void set_body(std::string&& body);
    void set_content_type(std::string_view type);
};
```

### HTTP Enums

```cpp
enum class method {
    GET, HEAD, POST, PUT, DELETE_, CONNECT, OPTIONS, TRACE, PATCH
};

enum class status {
    ok = 200,
    created = 201,
    no_content = 204,
    moved_permanently = 301,
    found = 302,
    bad_request = 400,
    unauthorized = 401,
    forbidden = 403,
    not_found = 404,
    internal_server_error = 500,
    // ... more
};

// Get reason phrase for status
const char* status_reason(status s);
```

---

## HTTP/2 (`elio::http`)

HTTP/2 support requires linking with `elio_http2`.

### `h2_client`

HTTP/2 client with sequential pooled connection reuse. The underlying session
layer supports HTTP/2 streams, but the current high-level client does not
coordinate multiple in-flight requests over one shared connection.

```cpp
class h2_client {
public:
    h2_client();
    explicit h2_client(const h2_client_config& config);

    // GET request (awaitable)
    coro::task<std::optional<response>> get(std::string_view url);

    // POST request (awaitable)
    coro::task<std::optional<response>> post(
        std::string_view url,
        std::string_view body,
        std::string_view content_type = mime::application_form_urlencoded
    );

    // PUT request (awaitable)
    coro::task<std::optional<response>> put(
        std::string_view url,
        std::string_view body,
        std::string_view content_type = mime::application_json
    );

    // DELETE request (awaitable)
    coro::task<std::optional<response>> del(std::string_view url);

    // PATCH request (awaitable)
    coro::task<std::optional<response>> patch(
        std::string_view url,
        std::string_view body,
        std::string_view content_type = mime::application_json
    );

    // Send custom request (awaitable)
    coro::task<std::optional<response>> send(
        method m,
        const url& target,
        std::string_view body = {},
        std::string_view content_type = {}
    );

    // Access TLS context and client configuration
    tls::tls_context& tls_context() noexcept;
    h2_client_config& config() noexcept;
    const h2_client_config& config() const noexcept;
};

// Convenience function for one-off HTTP/2 GET
coro::task<std::optional<response>> h2_get(std::string_view url);

// Convenience function for one-off HTTP/2 POST
coro::task<std::optional<response>> h2_post(
    std::string_view url,
    std::string_view body,
    std::string_view content_type = mime::application_form_urlencoded
);
```

### `h2_client_config`

```cpp
struct h2_client_config {
    std::chrono::seconds connect_timeout{10};  // TCP connect + TLS handshake timeout
    std::chrono::seconds read_timeout{30};     // Session I/O timeout; <=0 disables
    size_t max_concurrent_streams = 100;
    uint32_t initial_window_size = 65535;
    size_t max_response_size = 16 * 1024 * 1024;  // Max buffered body bytes
    std::string user_agent = "elio-http2/1.0";
    bool enable_push = false;  // Advertise SETTINGS_ENABLE_PUSH only;
                               // pushed responses are not exposed
    net::resolve_options resolve_options = net::default_cached_resolve_options();
    bool rotate_resolved_addresses = true;
};
```

### `h2_session`

Low-level HTTP/2 session (for advanced use).

```cpp
class h2_session {
public:
    explicit h2_session(tls::tls_stream& stream);
    
    // Submit a request, returns stream ID
    int32_t submit_request(method m, const url& target,
                           std::string_view body = {},
                           std::string_view content_type = {});
    
    // Process session I/O (awaitable)
    /* awaitable */ process();
    
    // Wait for stream to complete (awaitable)
    /* awaitable */ wait_for_stream(int32_t stream_id);
    
    // Check if session is alive
    bool is_alive() const;
    
    // Graceful shutdown (awaitable)
    /* awaitable */ shutdown();
};
```

---

## TLS (`elio::tls`)

### `tls_context`

TLS configuration context.

```cpp
class tls_context {
public:
    explicit tls_context(tls_mode mode, tls_version version = tls_version::tls_1_2_or_higher);
    
    // Load certificate and key
    bool load_certificate(std::string_view path);
    bool load_private_key(std::string_view path);
    
    // Certificate verification
    bool use_default_verify_paths();
    void set_verify_mode(verify_mode mode);
    
    // ALPN protocol negotiation
    bool set_alpn_protocols(std::string_view protocols);
};

enum class tls_mode {
    client,
    server
};
```

### `tls_stream`

TLS-wrapped TCP stream.

```cpp
class tls_stream {
public:
    tls_stream(tcp_stream tcp, tls_context& ctx);
    
    // Set SNI hostname
    void set_hostname(const std::string& hostname);
    
    // Perform TLS handshake (awaitable)
    /* awaitable */ handshake();
    
    // Read decrypted data (awaitable)
    /* awaitable */ read(void* buffer, size_t size);
    
    // Write data to encrypt (awaitable)
    /* awaitable */ write(const void* data, size_t size);
    
    // Get negotiated ALPN protocol
    std::string alpn_protocol() const;
};
```

---

## Synchronization (`elio::sync`)

### Cancellation Safety

All coroutine-aware synchronization primitives (`mutex`, `shared_mutex`, `event`, `semaphore`, `condition_variable`, `channel`) are **cancellation-safe**. Waiting coroutines are tracked via an intrusive linked list embedded in the coroutine frame. If a waiter is destroyed (cancellation, timeout, forced termination) before being woken, it automatically unlinks itself from the primitive's waiter list — preventing use-after-free when the primitive later calls its wake function.

No manual cleanup is required. Destroying a waiting coroutine is always safe.

### `mutex`

Coroutine-aware mutex.

```cpp
class mutex {
public:
    mutex();
    
    // Acquire lock (awaitable)
    /* awaitable */ lock();
    
    // Try to acquire without waiting
    bool try_lock();
    
    // Release lock
    void unlock();
};
```

### `shared_mutex`

Coroutine-aware read-write lock. Allows multiple concurrent readers or a single exclusive writer.

```cpp
class shared_mutex {
public:
    shared_mutex();
    
    // Acquire shared (read) lock (awaitable)
    /* awaitable */ lock_shared();
    
    // Acquire exclusive (write) lock (awaitable)
    /* awaitable */ lock();
    
    // Try to acquire shared lock without waiting
    bool try_lock_shared();
    
    // Try to acquire exclusive lock without waiting
    bool try_lock();
    
    // Release shared lock
    void unlock_shared();
    
    // Release exclusive lock
    void unlock();
    
    // Get current reader count
    size_t reader_count() const;
    
    // Check if a writer holds the lock
    bool is_writer_active() const;
};
```

### `shared_lock_guard`

RAII guard for shared (reader) locks.

```cpp
class shared_lock_guard {
public:
    explicit shared_lock_guard(shared_mutex& m);
    ~shared_lock_guard();  // Calls unlock_shared()
    
    void unlock();  // Manual early unlock
};
```

### `unique_lock_guard`

RAII guard for exclusive (writer) locks.

```cpp
class unique_lock_guard {
public:
    explicit unique_lock_guard(shared_mutex& m);
    ~unique_lock_guard();  // Calls unlock()
    
    void unlock();  // Manual early unlock
};
```

### `spinlock`

Lightweight spinlock using TTAS (Test-and-Test-and-Set) algorithm. Suitable for short critical sections with low contention where the overhead of coroutine suspension would exceed the spin time.

```cpp
class spinlock {
public:
    spinlock();

    // Acquire the lock (spins until acquired)
    void lock() noexcept;

    // Try to acquire without spinning
    bool try_lock() noexcept;

    // Release the lock
    void unlock() noexcept;

    // Check if locked (debugging only)
    bool is_locked() const noexcept;
};
```

### `spinlock_guard`

RAII guard for spinlock. Movable, non-copyable.

```cpp
class spinlock_guard {
public:
    explicit spinlock_guard(spinlock& s);  // Locks on construction
    ~spinlock_guard();                      // Unlocks on destruction

    spinlock_guard(spinlock_guard&& other) noexcept;
    spinlock_guard& operator=(spinlock_guard&& other) noexcept;

    void unlock();  // Manual early unlock (safe to call multiple times)
};
```

### `condition_variable`

Coroutine-aware condition variable that suspends coroutines instead of blocking threads.

Supports three modes of use:
- With `elio::sync::mutex` (coroutine-aware async re-lock)
- With `elio::sync::spinlock` or any lockable type (synchronous re-lock)
- Without any lock (`wait_unlocked()`) for single-worker scenarios

```cpp
class condition_variable {
public:
    condition_variable();

    /// Wait with elio::sync::mutex (single co_await)
    coro::task<void> wait(mutex& m);

    // Wait with a generic lockable (e.g., spinlock)
    // Re-acquires the lock synchronously before resuming
    template<lockable Lock>
    /* awaitable */ wait(Lock& lock);

    // Wait without external lock (single-worker-thread only)
    /* awaitable */ wait_unlocked();

    // Wake one waiting coroutine
    void notify_one();

    // Wake all waiting coroutines
    void notify_all();

    // Check if there are waiting coroutines
    bool has_waiters() const noexcept;
};
```

**Usage with mutex (single co_await):**
```cpp
sync::mutex mtx;
sync::condition_variable cv;
bool ready = false;

coro::task<void> waiter() {
    co_await mtx.lock();
    while (!ready) {
        co_await cv.wait(mtx);  // single co_await
    }
    mtx.unlock();
}

coro::task<void> notifier() {
    co_await mtx.lock();
    ready = true;
    mtx.unlock();
    cv.notify_one();
}
```

**Usage with spinlock (single co_await):**
```cpp
sync::spinlock sl;
sync::condition_variable cv;
bool ready = false;

coro::task<void> waiter() {
    sl.lock();
    while (!ready) {
        co_await cv.wait(sl);
    }
    sl.unlock();
}
```

### `event`

One-shot signaling primitive. One or more coroutines wait for the event to be set.

```cpp
class event {
public:
    event();

    // Wait for the event to be set (awaitable)
    /* awaitable */ wait();

    // Set the event (wakes all waiters)
    void set();

    // Check if the event is set
    bool is_set() const noexcept;

    // Reset the event
    void reset();
};
```

### `channel<T>`

Multi-producer multi-consumer channel for passing values between coroutines. Supports rendezvous (synchronous), bounded, and unbounded modes.

```cpp
template<typename T>
class channel {
public:
    // Create a rendezvous channel (default, capacity 0)
    channel();

    // Create a bounded channel with specified capacity
    explicit channel(size_t capacity);

    // Create an unbounded channel
    static channel unbounded();

    // Send a value (awaitable, blocks if full for bounded channels)
    /* awaitable */ send(T value);

    // Receive a value (awaitable, blocks if empty)
    /* awaitable<std::optional<T>> */ recv();

    // Close the channel
    void close();

    // Check if closed
    bool is_closed() const noexcept;
};
```

### `semaphore`

Counting semaphore.

```cpp
class semaphore {
public:
    explicit semaphore(int initial_count);

    // Acquire (awaitable)
    /* awaitable */ acquire();

    // Try acquire without waiting
    bool try_acquire();

    // Release
    void release();
};
```

---

## Timers (`elio::time`)

```cpp
// Sleep for duration
template<typename Rep, typename Period>
/* awaitable */ sleep_for(std::chrono::duration<Rep, Period> duration);

// Sleep for duration with cancellation support
// Returns coro::cancel_result::completed or coro::cancel_result::cancelled
template<typename Rep, typename Period>
/* awaitable<coro::cancel_result> */ sleep_for(std::chrono::duration<Rep, Period> duration,
                                               coro::cancel_token token);

// Sleep until time point
template<typename Clock, typename Duration>
/* awaitable */ sleep_until(std::chrono::time_point<Clock, Duration> tp);

// Yield execution to other coroutines
/* awaitable */ yield();
```

**Example:**
```cpp
coro::task<void> example(coro::cancel_token token) {
    // Simple sleep
    co_await time::sleep_for(100ms);
    
    // Cancellable sleep
    auto result = co_await time::sleep_for(5s, token);
    if (result == coro::cancel_result::cancelled) {
        // Cancelled early
    }
    
    // Yield to other coroutines
    co_await time::yield();
}
```

---

## Logging (`elio::log`)

### Macros

```cpp
ELIO_LOG_DEBUG(fmt, args...)
ELIO_LOG_INFO(fmt, args...)
ELIO_LOG_WARNING(fmt, args...)
ELIO_LOG_ERROR(fmt, args...)
```

### `logger`

```cpp
class logger {
public:
    static logger& instance();
    
    void set_level(level lvl);
    level get_level() const;
    
    template<typename... Args>
    void log(level lvl, const char* fmt, Args&&... args);
};

enum class level {
    debug,
    info,
    warning,
    error
};
```

---

## Hash (`elio::hash`)

### CRC32

```cpp
// Compute CRC32 checksum
uint32_t crc32(const void* data, size_t length, uint32_t crc = 0xFFFFFFFF);
uint32_t crc32(std::span<const uint8_t> data, uint32_t crc = 0xFFFFFFFF);

// CRC32 over scatter-gather buffers
uint32_t crc32_iovec(const struct iovec* iov, size_t count);

// Incremental CRC32
uint32_t crc32_update(const void* data, size_t length, uint32_t crc);
uint32_t crc32_finalize(uint32_t crc);
```

### SHA-1

```cpp
// Constants
constexpr size_t sha1_digest_size = 20;
constexpr size_t sha1_block_size = 64;

// Digest type
using sha1_digest = std::array<uint8_t, sha1_digest_size>;

// Compute SHA-1 hash
sha1_digest sha1(const void* data, size_t length);
sha1_digest sha1(std::span<const uint8_t> data);
sha1_digest sha1(std::string_view str);

// Get hex string
std::string sha1_hex(const sha1_digest& digest);
std::string sha1_hex(const void* data, size_t length);
std::string sha1_hex(std::string_view str);

// Incremental hashing
class sha1_context {
public:
    sha1_context() noexcept;
    void reset() noexcept;
    void update(const void* data, size_t length) noexcept;
    void update(std::span<const uint8_t> data) noexcept;
    void update(std::string_view str) noexcept;
    sha1_digest finalize() noexcept;
};
```

### SHA-256

```cpp
// Constants
constexpr size_t sha256_digest_size = 32;
constexpr size_t sha256_block_size = 64;

// Digest type
using sha256_digest = std::array<uint8_t, sha256_digest_size>;

// Compute SHA-256 hash
sha256_digest sha256(const void* data, size_t length);
sha256_digest sha256(std::span<const uint8_t> data);
sha256_digest sha256(std::string_view str);

// Get hex string
std::string sha256_hex(const sha256_digest& digest);
std::string sha256_hex(const void* data, size_t length);
std::string sha256_hex(std::string_view str);

// Incremental hashing
class sha256_context {
public:
    sha256_context() noexcept;
    void reset() noexcept;
    void update(const void* data, size_t length) noexcept;
    void update(std::span<const uint8_t> data) noexcept;
    void update(std::string_view str) noexcept;
    sha256_digest finalize() noexcept;
};
```

### Utilities

```cpp
// Convert digest to hex string
template<size_t N>
std::string to_hex(const std::array<uint8_t, N>& digest);

// Convert raw bytes to hex
std::string to_hex(const void* data, size_t length);
```

---

## RPC (`elio::rpc`)

### Buffer Types

#### `buffer_view`

Read-only view into serialized data.

```cpp
class buffer_view {
public:
    buffer_view(const void* data, size_t size);
    buffer_view(std::span<const uint8_t> span);
    
    const uint8_t* data() const noexcept;
    size_t size() const noexcept;
    size_t remaining() const noexcept;
    size_t position() const noexcept;
    
    void seek(size_t pos);
    void skip(size_t n);
    
    template<typename T> T read();           // Read primitive
    template<typename T> void read_into(T& value);
    template<typename T> T peek() const;     // Peek without advancing
    
    std::string_view read_string();          // Zero-copy string read
    std::span<const uint8_t> read_blob();    // Zero-copy blob read
    uint32_t read_array_size();
    
    std::span<const uint8_t> remaining_span() const noexcept;
};
```

#### `buffer_writer`

Growable buffer for serialization.

```cpp
class buffer_writer {
public:
    explicit buffer_writer(size_t initial_capacity = 256);
    
    void clear() noexcept;
    size_t size() const noexcept;
    const uint8_t* data() const noexcept;
    std::span<const uint8_t> span() const noexcept;
    struct iovec to_iovec() const noexcept;
    
    template<typename T> void write(T value);  // Write primitive
    void write_bytes(const void* src, size_t n);
    void write_string(std::string_view str);
    void write_blob(std::span<const uint8_t> blob);
    void write_array_size(uint32_t count);
    
    size_t reserve_space(size_t n);            // For back-patching
    template<typename T> void write_at(size_t offset, T value);
    
    buffer_view view() const noexcept;
    std::vector<uint8_t> release() noexcept;
};
```

#### `buffer_ref`

Zero-copy reference to external buffer data.

```cpp
class buffer_ref {
public:
    buffer_ref() noexcept;
    buffer_ref(const void* data, size_t size) noexcept;
    buffer_ref(std::span<const uint8_t> span) noexcept;
    buffer_ref(const struct iovec& iov) noexcept;
    
    const uint8_t* data() const noexcept;
    size_t size() const noexcept;
    bool empty() const noexcept;
    
    std::span<const uint8_t> span() const noexcept;
    struct iovec to_iovec() const noexcept;
    std::string_view as_string_view() const noexcept;
};
```

#### `iovec_buffer`

Discontinuous buffer for scatter-gather I/O.

```cpp
class iovec_buffer {
public:
    void add(const void* data, size_t size);
    void add(std::span<const uint8_t> span);
    void add(const buffer_writer& writer);
    void clear() noexcept;
    
    struct iovec* iovecs() noexcept;
    size_t count() const noexcept;
    size_t total_size() const noexcept;
    
    std::vector<uint8_t> flatten() const;  // Copies all data
};
```

### CRC32 Checksum

```cpp
// Compute CRC32 checksum
uint32_t crc32(const void* data, size_t length, uint32_t crc = 0xFFFFFFFF);
uint32_t crc32(std::span<const uint8_t> data, uint32_t crc = 0xFFFFFFFF);
uint32_t crc32_iovec(const struct iovec* iov, size_t count);
```

### Serialization

```cpp
// Serialize value to buffer
template<typename T>
void serialize(buffer_writer& writer, const T& value);

// Deserialize value from buffer
template<typename T>
void deserialize(buffer_view& reader, T& value);

// Convenience functions
template<typename T>
buffer_writer serialize(const T& value);

template<typename T>
T deserialize(buffer_view& reader);
```

### Schema Definition Macros

```cpp
// Define serializable fields for a struct
ELIO_RPC_FIELDS(ClassName, field1, field2, ...)

// Define empty struct (no fields)
ELIO_RPC_EMPTY_FIELDS(ClassName)

// Define RPC method
ELIO_RPC_METHOD(method_id, RequestType, ResponseType)
```

### Protocol Types

#### `frame_header`

```cpp
constexpr uint8_t protocol_version = 1;
constexpr size_t frame_header_size = 19;

struct frame_header {
    uint32_t magic;           // 0x454C494F ("ELIO")
    uint32_t request_id;
    message_type type;
    message_flags flags;
    method_id_t method_id;
    uint32_t payload_length;
    uint8_t version;          // protocol_version
    
    bool is_valid() const noexcept;
    std::array<uint8_t, frame_header_size> to_bytes() const;
    static frame_header from_bytes(const uint8_t* data);
};
```

The fixed 19-byte wire header includes the final 1-byte `version` field.
Frames are valid only when `version == protocol_version`.

#### `message_type`

```cpp
enum class message_type : uint8_t {
    request = 0,
    response = 1,
    error = 2,
    ping = 3,
    pong = 4,
    cancel = 5,
};
```

#### `message_flags`

```cpp
enum class message_flags : uint8_t {
    none = 0,
    has_timeout = 1 << 0,
    has_checksum = 1 << 1,
    compressed = 1 << 2,    // reserved
    streaming = 1 << 3,     // reserved
};

bool has_flag(message_flags flags, message_flags flag);
```

#### `rpc_error`

```cpp
enum class rpc_error : uint32_t {
    success = 0,
    timeout = 1,
    connection_closed = 2,
    invalid_message = 3,
    method_not_found = 4,
    serialization_error = 5,
    internal_error = 6,
    cancelled = 7,
};

const char* rpc_error_str(rpc_error err);
```

### Message Builders

```cpp
// Build request frame
template<typename Request>
std::pair<frame_header, buffer_writer> build_request(
    uint32_t request_id,
    method_id_t method_id,
    const Request& request,
    std::optional<uint32_t> timeout_ms = std::nullopt,
    bool enable_checksum = false);

// Build response frame
template<typename Response>
std::pair<frame_header, buffer_writer> build_response(
    uint32_t request_id,
    const Response& response,
    bool enable_checksum = false);

// Build error response
std::pair<frame_header, buffer_writer> build_error_response(
    uint32_t request_id,
    rpc_error error_code,
    std::string_view error_message = "",
    bool enable_checksum = false);

// Build ping/pong/cancel
frame_header build_ping(uint32_t ping_id);
frame_header build_pong(uint32_t ping_id);
frame_header build_cancel(uint32_t request_id);
```

### Server Types

#### `rpc_context`

```cpp
struct rpc_context {
    uint32_t request_id;
    method_id_t method_id;
    std::optional<uint32_t> timeout_ms;
    coro::cancel_token cancel_token;
    
    bool has_timeout() const noexcept;
};
```

`cancel_token` is cancelled when the client sends an RPC cancel frame for this
request or when the session is closed. Cancellation is cooperative; handlers
that need to stop early should poll the token or pass it to cancellable Elio
operations.

#### `cleanup_callback_t`

```cpp
using cleanup_callback_t = std::function<void()>;
```

#### `rpc_server_config`

```cpp
struct rpc_server_config {
    size_t max_sessions = 1024;
    std::chrono::seconds frame_read_timeout{30};
    uint32_t max_message_size = elio::rpc::max_message_size;
};
```

- `max_sessions`: Maximum concurrent sessions accepted by `serve()`.
  `0` disables the cap.
- `frame_read_timeout`: Per-frame deadline for receiving the header,
  payload, and optional checksum. `0s` disables the deadline.
- `max_message_size`: Maximum payload bytes accepted per frame. The
  default is the protocol-wide 16 MiB limit.

#### `rpc_server<Stream>`

```cpp
template<typename Stream>
class rpc_server {
public:
    rpc_server() = default;
    explicit rpc_server(rpc_server_config config);

    const rpc_server_config& config() const noexcept;

    // Register async handler
    template<typename Method, typename Handler>
    void register_method(Handler handler);
    
    // Register async handler with context
    template<typename Method, typename Handler>
    void register_method_with_context(Handler handler);
    
    // Register sync handler
    template<typename Method, typename Handler>
    void register_sync_method(Handler handler);
    
    // Register handler with cleanup callback
    template<typename Method, typename Handler>
    void register_method_with_cleanup(Handler handler);
    
    // Register handler with context and cleanup
    template<typename Method, typename Handler>
    void register_method_with_context_and_cleanup(Handler handler);
    
    // Serve connections (awaitable)
    /* awaitable */ serve(net::tcp_listener& listener);
    /* awaitable */ serve(net::uds_listener& listener);
    
    // Handle single client
    /* awaitable */ handle_client(Stream stream);
    
    void stop();
    bool is_running() const noexcept;
    size_t session_count() const;
};

// Type aliases
using tcp_rpc_server = rpc_server<net::tcp_stream>;
using uds_rpc_server = rpc_server<net::uds_stream>;
```

### Client Types

#### `rpc_result<T>`

```cpp
template<typename T>
class rpc_result {
public:
    explicit rpc_result(T value);         // Success
    explicit rpc_result(rpc_error err);   // Error
    
    bool ok() const noexcept;
    explicit operator bool() const noexcept;
    
    rpc_error error() const noexcept;
    const char* error_message() const noexcept;
    
    T& value() &;
    const T& value() const&;
    T&& value() &&;
    
    template<typename U> T value_or(U&& default_value) const&;
    
    T* operator->();
    T& operator*();
};

// Specialization for void
template<>
class rpc_result<void> {
    static rpc_result success();
    bool ok() const noexcept;
    rpc_error error() const noexcept;
};
```

### Type Traits

```cpp
template<typename T> inline constexpr bool is_primitive_v;
template<typename T> inline constexpr bool is_string_type_v;
template<typename T> inline constexpr bool is_vector_v;
template<typename T> inline constexpr bool is_std_array_v;
template<typename T> inline constexpr bool is_map_type_v;
template<typename T> inline constexpr bool is_optional_v;
template<typename T> inline constexpr bool is_buffer_ref_v;
template<typename T> inline constexpr bool has_rpc_fields_v;
```
