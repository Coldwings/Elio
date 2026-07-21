# API Reference

This page provides a reference for Elio's public API.

For responsibility boundaries, thread-safety defaults, lifetime preconditions,
and audit triage rules, see [[API Contracts]]. The reference below describes
surface area and usage; the contract page defines what Elio guarantees and
what callers must provide for each public interface.

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
| `elio::websocket` | Convenience re-export of WebSocket support |
| `elio::sse` | Convenience re-export of Server-Sent Events support |
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
    
    // Non-copyable, move-only
    task(const task&) = delete;
    task& operator=(const task&) = delete;
    task(task&&) noexcept;
    task& operator=(task&&) noexcept;
    
    ~task();  // Destroys the coroutine frame

    bool valid() const noexcept;
    explicit operator bool() const noexcept;
    
    // Awaitable interface (use with co_await)
    bool await_ready() const noexcept;
    template<typename AwaiterPromise>
    std::coroutine_handle<> await_suspend(
        std::coroutine_handle<AwaiterPromise> awaiter);
    T await_resume();  // Returns result or rethrows exception
};
```

`task<T>` is a single-shot lazy owner. Moving it transfers only ownership of
the unstarted coroutine frame and leaves the source empty; it does not migrate
running work or pending I/O. An unstarted task does not remain in creator-thread
virtual-stack state; ancestry is bound to the actual awaiter when execution
starts. Destroying a non-empty task destroys the frame if ownership has not
been transferred to the runtime. Do not await an empty task or await the same
task more than once.

In 0.6, `await_suspend` is a potentially throwing template so an Elio child can
link to the actual Elio awaiter's cancellation context before first resume.
Allocation failure while registering that link propagates from the `co_await`
expression and the child does not start. Normal `co_await task` source remains
unchanged, but code that names the exact `await_suspend` member type or requires
it to be `noexcept` must be updated. A foreign coroutine promise does not
implicitly inherit an Elio runtime token; adapter code must bridge cancellation
explicitly when that behavior is required.

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

#### `elio::go_to()` - Affinity Fire-and-Forget

Spawn a coroutine with affinity to a specific worker thread. The affinity is set before the task first resumes, so the task is not executed on another worker; if a steal attempt observes the task on another queue, the scheduler bounces it back to the target worker.

```cpp
template<typename F, typename... Args>
void go_to(size_t worker_id, F&& f, Args&&... args);
```

`worker_id` should be less than the scheduler's current `num_threads()` for
deterministic placement. Out-of-range values are not rejected, but exact pinning
to that numeric worker is not guaranteed; the scheduler may enqueue through a
fallback path if the selected slot is unavailable during resizing.

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

> **Note:** `go_to()` differs from spawning with `go()` + `co_await set_affinity()`. With `go_to()`, worker affinity is set before the task is scheduled and before it ever runs. A later steal attempt may briefly remove the task from a queue, but the scheduler checks affinity and requeues it instead of running it on the wrong worker. With `go()` + `set_affinity()`, the task may briefly run on any worker before migrating.

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

    // Observe or wait for coroutine-frame destruction
    bool is_destroyed() const noexcept;
    void wait_destroyed() const;

    // Cooperative, best-effort cancellation request
    void request_cancel() const;
    bool is_cancellation_requested() const noexcept;
};
```

`is_ready()` reports result publication and may become true before the spawned
wrapper frame is destroyed. `wait_destroyed()` is for non-coroutine callers that
must wait until a normally completed wrapper has released its frame parameters,
callable, arguments, and captures.

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

`request_cancel()` publishes through state shared with the spawned task; it does
not access the coroutine frame and remains valid after frame destruction. The
request propagates through direct lazy-task awaits between Elio tasks. It does
not forcibly destroy the task, roll back side effects, or guarantee prompt
completion. Foreign coroutine promises, separately spawned tasks, and explicit
token arguments remain independent unless deliberately bridged. Registered
cancellation callback exceptions are rethrown after callback dispatch.

### `task_group` and `task_scope()`

Structured ownership for child tasks submitted to one scheduler domain.

```cpp
enum class task_group_failure_policy {
    fail_fast,
    collect_all,
};

struct task_group_options {
    task_group_failure_policy failure_policy =
        task_group_failure_policy::fail_fast;
    size_t max_concurrency = 0;  // Zero means unlimited
};

class task_group_error : public std::exception {
public:
    const char* what() const noexcept override;
    const std::vector<std::exception_ptr>& failures() const noexcept;
};

class task_group {
public:
    class join_awaitable;

    explicit task_group(task_group_options options = {});
    explicit task_group(runtime::scheduler& scheduler,
                        task_group_options options = {});

    template<typename F, typename... Args>
    void spawn(F&& function, Args&&... args);

    join_awaitable join() noexcept;
    void request_cancel();
    bool is_cancellation_requested() const noexcept;
    size_t outstanding_children() const noexcept;
    std::vector<std::exception_ptr> failures() const;
    task_group_options options() const noexcept;
    runtime::scheduler& scheduler_domain() const noexcept;
};

template<typename F>
task<void> task_scope(F&& body, task_group_options options = {});

template<typename F>
task<void> task_scope(runtime::scheduler& scheduler, F&& body,
                      task_group_options options = {});
```

The explicit-scheduler `task_scope()` overload must initially be awaited on a
worker of that scheduler. It does not dispatch initial entry from another
scheduler or from an external thread, including the thread that called
`scheduler::start()`; violating this precondition throws `std::logic_error`.

The default constructor binds the group to the current scheduler worker and
throws `std::logic_error` when there is none. Merely calling
`scheduler::start()` does not make its calling thread a scheduler worker. The
explicit constructor submits every
child to the selected scheduler. When construction occurs while executing on
that same scheduler, the group links to the current task's cancellation token;
otherwise the group starts with independent cancellation authority.

`spawn()` accepts a callable returning `task<T>`, stores decayed callable and
argument values in scheduler-owned frames, and discards the result value. It
throws after joining has started. Scheduler rejection is recorded as a child
failure and reported by `join()`. A nonzero `max_concurrency` limits child body
execution, not the number of registered or scheduler-queued child frames.
`outstanding_children()` reports those accepted child submissions and does not
include the callback body of an active `task_scope()`.

`join()` is `[[nodiscard]]`, returns a direct single-use awaitable, and must be
awaited while executing on a worker of the group's scheduler. It does not create
or link a nested task, so beginning the join does not allocate a parent
cancellation registration. It stops further spawning, waits until every
registered child frame has left the group, and then reports failures according
to the selected policy. Its
continuation resumes in that scheduler domain; normal completion is queued, and
a same-domain enqueue rejection falls back to direct resumption with the saved
frame context. `join()` must
be awaited exactly once. Under `fail_fast`, the first child failure requests
cooperative sibling cancellation and is rethrown directly. Under `collect_all`,
siblings continue and `join()` throws `task_group_error`; its `failures()` view
contains every recorded exception. A bare group's `failures()` accessor exposes
the same records before or after join. Child code must pass
`this_coro::cancel_token()` to token-aware waits when prompt cancellation is
required.

Children that have not entered their body when group cancellation is already
visible finish without invoking the user callable. Cancellation that races with
body entry remains cooperative; once the callable starts, it must observe or
forward its runtime token.

The `task_group` destructor requests cancellation for unfinished children but
cannot synchronously join from a scheduler worker. Always await `join()` before
destroying a bare group. Prefer `task_scope()` for lexical ownership: it joins
on normal body completion, and on body failure it first requests cancellation,
joins all children, and then rethrows the body failure under `fail_fast`. Under
`collect_all`, concurrent body and child failures are combined into one
`task_group_error`, with the body exception first. If the body succeeds but a
child fails, the child failure is reported after all children finish. The
scope body itself inherits group cancellation, so fail-fast can wake a body that
passes `this_coro::cancel_token()` to its current wait. The body must not call
`join()` on the supplied group; returning from the body initiates the scope's
single join. If a body awaitable resumes on another executor, the scope wrapper
hands execution back to the selected scheduler before joining and resuming its
caller. Both `task_scope()` overloads are `[[nodiscard]]` lazy tasks.
The scope retains the body callable and its captures through child joining.
Automatic local objects in the returned body coroutine are still destroyed
when that coroutine returns; a child that can continue after body return must
not retain references to those locals.

`request_cancel()` runs group cancellation callbacks on the selected scheduler.
An ordinary external thread, including the thread that called
`scheduler::start()`, waits for that dispatch and can rethrow a callback
exception. A worker belonging to another scheduler posts the request and returns
without waiting, preventing reciprocal single-worker scheduler deadlocks;
callback failures from that asynchronous path are recorded as group failures.
Keep the selected scheduler running while cancellation or join is pending.

```cpp
coro::task<void> process_batch(std::span<const item> items) {
    co_await coro::task_scope(
        [&](coro::task_group& group) -> coro::task<void> {
            for (const auto& item : items) {
                group.spawn([&item]() -> coro::task<void> {
                    co_await process(item,
                        coro::this_coro::cancel_token());
                });
            }
            co_return;
        },
        {.max_concurrency = 8});
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
    
    // Request cancellation (invokes callbacks not reentrantly suppressed)
    void cancel();
    
    // Check if cancelled
    bool is_cancelled() const noexcept;
};

} // namespace elio::coro

namespace elio::coro::this_coro {

// Current runtime task token, or a never-cancelled token outside Elio execution
cancel_token cancel_token() noexcept;

} // namespace elio::coro::this_coro
```

`cancel()` synchronously selects and invokes callbacks on the requesting thread,
then rethrows the first callback exception after dispatching the rest.
Registration teardown suppresses a callback not yet selected by cancellation;
outside callback dispatch, teardown waits once another thread has selected or
started it. During callback reentry, cross-dispatch teardown is deferred instead
of waiting so mutually unregistering callbacks cannot deadlock; the target
payload remains alive through dispatch, but external captured state still needs
synchronization. Self-unregistration and reentrant removal of a later callback
selected by the same synchronous dispatcher are supported. Callbacks can also
overlap when registration races with an already cancelled source.

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

Cancellation is best-effort: actual completion may win a race, I/O side effects
are not rolled back, and a backend that cannot actively abort accepted work may
wait for natural completion. A task-level token only affects operations that
receive or inspect it.

### Structured Combinators

```cpp
class combinator_cancelled : public std::runtime_error;

template<typename... Fs>
coro::task<std::tuple</* branch results */...>> when_all(Fs... callables);

template<typename... Fs>
coro::task<std::pair<size_t, /* winner result */>>
when_any(Fs... callables);

template<typename Rep, typename Period, typename F>
coro::task<timeout_result</* callable result */>>
with_timeout(std::chrono::duration<Rep, Period> timeout, F callable);
```

All three helpers return concrete move-only lazy `coro::task` objects, replacing
the dedicated combinator awaitable types from 0.5. They own their callable
objects by value and require execution in a scheduler domain when awaited. Their
branches are registered in one internal `task_group`; no accepted branch is
implicitly detached. Move-only callables must be passed as rvalues.

`when_all()` registers every callable as a group child. A child whose body has
not started when cancellation is already visible may finish without invoking
its callable. After the first failure, `when_all()` requests cancellation of
unfinished siblings, waits for all accepted child frames to leave the group,
and then rethrows the first child failure. Later child failures are
reported through the scheduler's unhandled-exception handler before return. A
failure while transferring a callable into a launched child takes precedence
over child failures because the complete branch set was never accepted. A parent
cancellation request is propagated to the group. If cancellation prevents a
branch from producing the value required by the result tuple, `when_all()` throws
`combinator_cancelled` after drain.

`when_any()` atomically selects the first successful or exceptional branch
completion. It then requests cancellation of every loser and waits for all of
them to terminate before returning the winner. A token-accepting callable
receives its structured child token; a no-argument callable can use
`this_coro::cancel_token()`. A launch-time callable-transfer failure takes
precedence over an already selected winner because the complete branch set was
never accepted. Otherwise, the winner's exception is rethrown. A loser that
throws after winner selection is reported through the current scheduler's
unhandled-exception handler before the combinator returns. If parent cancellation
drains every branch before a winner exists, the operation throws
`combinator_cancelled`.

`with_timeout()` is a structured `when_any()` between the callable and a
cancellable timer. When the timer reports expiry, it selects a timed-out result
and requests cancellation of the callable, but the returned task remains
suspended until the callable reaches a terminal state. Consequently, the
requested duration is not a hard upper bound on return latency. Parent
cancellation is not reported as
timer expiry; if it prevents a result, `combinator_cancelled` is thrown.
If deadline expiry and parent cancellation race before the timer branch publishes
its result, the timer's best-effort cancellation contract applies and either a
timed-out result or `combinator_cancelled` may be observed.

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

    // Stop scheduler workers without graceful coroutine/I/O drain.
    // Accepted scheduler-owned blocking work is drained first.
    void shutdown_force();

    // Inspect or wait for tracked work
    size_t active_tasks() const noexcept;
    bool wait_for_idle(std::chrono::milliseconds timeout = std::chrono::milliseconds::max());
    
    // Hand independent work to the scheduler for its first execution.
    // Construction-time virtual-stack ancestry is detached.
    void spawn(std::coroutine_handle<> handle);
    bool try_spawn(std::coroutine_handle<> handle);

    // Re-enqueue an already-suspended coroutine. Logical await ancestry is
    // preserved so continuation transfer restores the correct parent frame.
    // Rejection leaves the borrowed handle live.
    bool try_schedule(std::coroutine_handle<> handle);
    
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

    // Initial ownership handoff toward a worker; detaches construction ancestry.
    void spawn_to(size_t worker_id, std::coroutine_handle<> handle);

    // Re-enqueue suspended work toward a worker; preserves await ancestry.
    bool try_schedule_to(size_t worker_id, std::coroutine_handle<> handle);

    // For targeted spawn and schedule APIs, exact placement requires
    // worker_id in [0, num_threads()) and an available target worker. Larger
    // values or unavailable targets use fallback scheduling.

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

    // Dynamically resize the thread pool.
    // Must be called from outside scheduler worker threads.
    void set_thread_count(size_t count);

    // Get the current scheduler (thread-local)
    static scheduler* current() noexcept;

    // Advanced accessors
    worker_thread* get_worker(size_t index);
    const wait_strategy& get_wait_strategy() const noexcept;
    blocking_pool* get_blocking_pool() noexcept;

    // Unhandled exception reporting for detached tasks and discarded
    // structured-combinator branches
    using unhandled_exception_handler = std::function<void(std::exception_ptr)>;
    void set_unhandled_exception_handler(unhandled_exception_handler handler);
    const unhandled_exception_handler* get_unhandled_exception_handler() const noexcept;
    void report_unhandled_exception(std::exception_ptr ex) noexcept;
};
```

The unhandled-exception handler APIs are thread-safe with each other. A report
keeps its selected handler alive and invokes it without holding the publication
lock, so replacing the handler concurrently affects later snapshots and a
handler may replace itself. Applications remain responsible for synchronizing
mutable state captured by the handler. A pointer returned by
`get_unhandled_exception_handler()` is backed by a shared snapshot owned by the
calling thread. It remains valid until that same thread calls the getter again
or exits, even if another thread replaces or clears the scheduler handler.

The raw-handle APIs have distinct ownership and virtual-stack contracts.
`spawn()`/`try_spawn()` and `spawn_to()` are for an independent handle before
its first execution; they detach construction-time ancestry.
`try_schedule()` and `try_schedule_to()` are for a coroutine that has already
suspended and must preserve the logical parent established by its await chain.
The `try_` APIs borrow the handle: rejection leaves it live, and the caller must
resume, retain, destroy, or otherwise resolve it. `spawn()` and `spawn_to()`
consume ownership and destroy a handle rejected before execution. Internal wake
and affinity-migration paths handle rejection explicitly; callers must not
substitute one family for the other.

`shutdown()` is the graceful path. Before waiting, it atomically closes
independent initial task admission against `go()`, `go_to()`, `go_joinable()`,
`go_joinable_to()`, `spawn()`, `try_spawn()`, and `spawn_to()`. Work accepted
before that boundary may still enqueue continuations through `try_schedule()` or
`try_schedule_to()` and register structured task-group children while the
scheduler drains. Task-group submission from outside the scheduler remains
independent admission and is rejected after closure. Before teardown, shutdown
seals linked child registration and rechecks the tracked accepted set under the
lifecycle lock. A running raw task that is not itself tracked therefore cannot
register a new child after shutdown has observed the accepted set empty. The call
waits for tracked work, including work suspended on scheduler-owned I/O, and
returns whether the drain completed before the timeout. A rejected `go()` body
does not run; a rejected `go_joinable()` handle reports `std::logic_error`;
rejected raw-handle calls keep or destroy ownership according to the
`try_`/consuming distinction above. Use `shutdown_force()` only when
non-graceful teardown is required: it does not wait for tracked coroutine or
pending-I/O drain, but it may still wait for already-accepted scheduler-owned
blocking work before stopping workers.

A scheduler has a one-shot lifecycle. Once either shutdown path begins, later
`start()` calls are no-ops; create a new scheduler for another run. Concurrent
shutdown callers share one teardown, and external callers wait for a teardown
initiated by a scheduler worker before returning. Destroying a scheduler from
one of its own worker threads is unsupported and terminates the process because
the scheduler cannot safely join and release the currently executing worker.
Once shutdown begins, `set_thread_count()` also leaves the configured worker
count unchanged. If a draining worker requests force shutdown while an external
grow is waiting to join that same worker, the worker records the request and
returns; the grow controller performs teardown after the worker unwinds. Use an
external lifecycle owner when the caller must observe complete teardown.

`set_thread_count()` must be called from outside scheduler worker threads. If a
worker thread calls it, Elio logs a warning and leaves the worker count
unchanged to avoid deadlocking the resize path while joining worker threads.

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
    // Schedule a task to this worker (thread-safe, wakes worker if sleeping).
    // A full bounded inbox spills to a locked overflow queue; false means the
    // worker has stopped and the caller retains the handle.
    bool schedule(std::coroutine_handle<> handle);

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
- The bounded MPSC inbox remains the fast path; a locked overflow queue absorbs rare bursts without resuming worker-bound coroutines on submitter threads
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

Thread affinity allows you to bind vthreads (coroutines) to specific worker threads so they run on a designated worker. Steal attempts that encounter an affinity-bound task are redirected to the bound worker instead of executing the task on the wrong worker.

### Constants

```cpp
// Constant indicating no caller affinity (internal ownership may still pin)
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
    
    // Now subsequent code runs on worker 0 while affinity remains valid
    // Steal attempts are bounced back to the affinity worker
    co_return;
}
```

### `clear_affinity()`

Remove caller-requested affinity, allowing the vthread to migrate freely when
no internal ownership constraint is active. This does not clear or migrate an
active worker-local I/O operation.

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

### Task Execution Context And Promise Affinity

`promise_base` is the frame-resident anchor for a shared
`task_execution_context`. The task object itself does not carry scheduler
policy. External runtime owners may share the context without keeping the frame
alive.

```cpp
namespace elio::coro {

class task_execution_context {
public:
    // Allocates cancellation state and may throw std::bad_alloc in 0.6
    task_execution_context();

    size_t user_affinity() const noexcept;
    size_t effective_affinity() const noexcept;
    void set_user_affinity(size_t worker_id) noexcept;
    bool has_user_affinity() const noexcept;
    void clear_user_affinity() noexcept;

    // Read-only operation-local I/O ownership diagnostics
    bool has_active_io_pin() const noexcept;
    size_t io_owner_worker() const noexcept;
    uint64_t io_context_generation() const noexcept;
    size_t active_io_pin_count() const noexcept;

    // Internal scheduler-maintenance policy
    void set_worker_local(bool worker_local = true) noexcept;
    bool is_worker_local() const noexcept;

    // Task-chain cancellation control
    cancel_token get_cancel_token() const noexcept;
    void request_cancel();
    bool is_cancellation_requested() const noexcept;
};

} // namespace elio::coro
```

`io_owner_worker()` and `io_context_generation()` expose the last recorded
identity. Treat them as an active placement constraint only when
`has_active_io_pin()` is true; `active_io_pin_count()` is authoritative.

The context's cancellation source survives independently of the frame through
shared context ownership. Starting a lazy task with direct `co_await` from
another Elio task links its context one-way to that awaiter's token. A foreign
coroutine promise is a cancellation boundary unless it deliberately bridges a
token. A `join_handle` shares the spawned wrapper context, so cancellation
remains race-safe without storing a raw promise pointer. Completion and
cancellation of individual operations remain in their awaitable/backend state
machines.

In 0.6, construction is no longer `noexcept` because each context allocates its
cancellation state. Allocation failure propagates to the code creating the
context; downstream code that explicitly relied on the old `noexcept`
constructor contract must be updated.

The promise affinity methods delegate caller-requested affinity to that shared
context. `effective_affinity()` is the scheduler placement boundary and is kept
distinct from caller affinity. An active I/O owner takes precedence over user
affinity; clearing or changing user affinity does not clear the operation pin:

```cpp
class promise_base {
public:
    std::shared_ptr<task_execution_context> execution_context() const noexcept;

    // Get current affinity (NO_AFFINITY if not set)
    size_t affinity() const noexcept;

    // Get the scheduler placement constraint
    size_t effective_affinity() const noexcept;
    
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

    // Scheduler ownership diagnostics. Standalone contexts report no owner.
    bool is_worker_owned() const noexcept;
    size_t owner_worker_id() const noexcept;
    uint64_t generation() const noexcept;
    size_t active_pin_count() const noexcept;

    // Low-level mutation. Scheduler contexts require their exact owner thread.
    bool prepare(const io_request& request);
    int submit();
    bool cancel(void* user_data);
    
    // Poll for I/O completions (with optional timeout)
    int poll(std::chrono::milliseconds timeout = std::chrono::milliseconds::zero());

    // Cross-thread-safe wakeup entry
    void notify() noexcept;
    
    // Read-only diagnostics
    bool has_pending() const noexcept;
    size_t pending_count() const noexcept;

    // Most recent built-in-backend completion on the calling thread.
    static io_result get_last_result() noexcept;
};

// Get the current scheduler worker's I/O context, or the global fallback
// outside a scheduler worker.
io_context& current_io_context() noexcept;

// Get the default global I/O context.
io_context& default_io_context();
```

Scheduler workers own their contexts. I/O awaitables validate that a
scheduler-owned context is used only on its owner worker and pin the awaiting
coroutine to that context generation until backend completion or orphan
cleanup. Work stealing and affinity migration cannot move the continuation
while the pin is active. A retiring worker continues polling until both its
backend pending count and active pin count are zero.

A directly constructed `io_context` is standalone. The caller must serialize
access, poll it, and keep it alive for all pending operations. Scheduler
coroutines must use their current worker context; they cannot submit through a
standalone context or another worker's context. Mutating context operations
enforce the exact owner thread and throw `std::logic_error` on violation, while
`notify()` is a non-throwing cross-thread wakeup. Mutable raw-backend access is
not exposed.

Standard awaitables used from a non-Elio coroutine promise retain context-level
drain accounting and complete on the backend owner, but cannot install an Elio
task execution-context pin. A custom coroutine runtime that independently
re-enqueues such a suspended handle must preserve that owner itself.

I/O pinning protects backend ownership only. It does not serialize concurrent
operations on the same stream or fd. Follow the concrete stream contract and
externally serialize conflicting reads, writes, close, or destruction.

`get_last_result()` is the legacy result channel used by raw-handle awaitables.
It reports the latest completion published by either built-in backend on the
calling thread, including an epoll runtime fallback in a build that also
supports io_uring. Read it from the resumed completion path before another
context on the same thread can publish a newer result. Standard `op_state`-based
awaitables return their operation-owned `io_result` directly.

### Custom I/O Awaitables

`io_awaitable_base` is a protected integration surface for awaitables that use
Elio's owner-controlled `op_state`. In 0.6, derived awaitables must select and
validate an `io_context` while installing that state. The old
`bind_to_worker()` / `restore_affinity()` pair is removed; the operation guard
now releases the internal pin when backend ownership reaches a terminal state,
allowing the unchanged caller affinity to become effective again.

Use this submission pattern:

```cpp
template<typename Promise>
void await_suspend(std::coroutine_handle<Promise> awaiter) {
    auto& ctx = current_io_context();

    io_request req{};
    req.op = io_op::read;
    req.fd = fd_;
    req.buffer = buffer_;
    req.length = length_;
    req.awaiter = awaiter;
    req.state = setup_op_state(awaiter, ctx);

    if (!prepare_op_state(ctx, req)) {
        clear_op_state();
        result_ = io_result{-EAGAIN, 0};
        awaiter.resume();
        return;
    }
    ctx.submit();
}
```

If custom cancellation state stores `req.state`, use the rollback-callback
overload of `prepare_op_state()` to clear that pointer and unregister callbacks
before an exception escapes. A `false` prepare result means no backend
ownership was transferred and still requires `clear_op_state()`. Once prepare
returns `true`, leave the state attached to the awaitable; normal completion or
the orphan protocol releases the operation guard.

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
auto result = co_await async_sendmsg(fd, iovecs, count, flags);

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

`async_send()` and `async_sendmsg()` are socket operations. `async_sendmsg()`
is a scatter-gather send over an `iovec` array, not a general `sendmsg(2)`
wrapper for destination addresses or ancillary/control data. On platforms with
per-call `SIGPIPE` suppression, they apply it so peer-close failures are
reported through `io_result` rather than process-level signal delivery.

### Batch I/O

Submit multiple file operations. Segments with non-negative offsets are
positioned operations that can be submitted together with io_uring. Segments
with negative offsets use the file descriptor's current position and are
executed in segment order to preserve file-position semantics.

```cpp
// Batch read: read multiple file regions at once
struct batch_read_segment {
    int64_t offset;   // File offset (negative for current position)
    void* buffer;     // Destination buffer
    size_t length;    // Bytes to read
};

std::vector<batch_read_segment> segments = { ... };
auto results = co_await batch_read(fd, segments);
// results[i] > 0: bytes read; results[i] < 0: -errno

// Batch write: write multiple file regions at once
struct batch_write_segment {
    int64_t offset;      // File offset (negative for current position)
    const void* buffer;  // Source data
    size_t length;       // Bytes to write
};

std::vector<batch_write_segment> segments = { ... };
auto results = co_await batch_write(fd, segments);
```

**How it works:**
1. Positioned segments are prepared in the io_uring submission queue
2. Single `io_uring_submit()` syscall dispatches positioned operations
3. Current-position segments fall back to ordered `read()` / `write()` calls
4. Each io_uring completion is tracked via tagged `user_data` encoding
5. Results are returned in a `std::vector<int>` matching segment order

**Fallback:** When io_uring is unavailable (epoll backend), positioned
segments fall back to sequential synchronous `pread`/`pwrite`.
Current-position segments use ordered `read()` / `write()` calls.

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
    int block_error(sigset_t* old_mask = nullptr) const; // Direct pthread error
    bool block(sigset_t* old_mask = nullptr) const;      // Block for thread
    int unblock_error() const;                           // Direct pthread error
    bool unblock() const;                                // Unblock for thread
    int set_mask_error(sigset_t* old_mask = nullptr) const;
    bool set_mask(sigset_t* old_mask = nullptr) const;
    int block_all_threads_error() const;
    // Blocks the current thread. Call before creating workers so they inherit it.
    bool block_all_threads() const;
};
```

The `*_error()` variants return the direct `pthread_sigmask()` error number, or
`0` on success. The boolean wrappers preserve the source-compatible success/fail
API.

### `signal_fd`

Async-friendly signalfd wrapper.

`signal_fd` retains the `io_context` selected at construction. Construct and
await it on the same scheduler worker. A directly constructed standalone
context may be used only when the caller also serializes and polls that context;
standard waits reject crossing between standalone and scheduler execution or
between scheduler workers.

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
    
    // Deprecated no-op; returns false
    [[deprecated("use signal_set::unblock() explicitly")]]
    bool restore_mask() noexcept;
    
    void close();  // Close explicitly
};
```

Automatic blocking by `signal_fd` is acquire-only. `close()`, moves, and
destruction do not restore or unblock the calling thread's mask. Whole-mask
snapshots are not composable with overlapping descriptors or later caller mask
changes, so `restore_mask()` is deprecated, performs no operation, and returns
`false`. Once no descriptor or worker depends on a blocked signal, release it
explicitly with `signal_set::unblock()` on the thread that owns the mask.

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

`ipv4_address(std::string_view, uint16_t)` accepts numeric IPv4 literals.
Invalid text logs an error and falls back to `INADDR_ANY`; callers that need
strict address validation should resolve or validate host strings before
construction.

### `ipv6_address`

IPv6 address with port and optional scope ID.

```cpp
struct ipv6_address {
    struct in6_addr addr = IN6ADDR_ANY_INIT;
    uint16_t port = 0;
    uint32_t scope_id = 0;

    ipv6_address() = default;
    explicit ipv6_address(uint16_t p);
    ipv6_address(std::string_view ip, uint16_t p);

    int family() const noexcept;
    std::string to_string() const;
    bool is_v4_mapped() const;
};
```

`ipv6_address(std::string_view, uint16_t)` accepts numeric IPv6 literals. A
link-local scope suffix such as `%eth0` is converted with `if_nametoindex()`.
Invalid text logs an error and falls back to the IPv6 any address.

### `socket_address`

Generic TCP socket address wrapper for IPv4 or IPv6 endpoints.

```cpp
class socket_address {
public:
    socket_address();
    socket_address(const ipv4_address& addr);
    socket_address(const ipv6_address& addr);
    explicit socket_address(uint16_t port);
    socket_address(std::string_view host, uint16_t port);

    int family() const;
    uint16_t port() const;
    bool is_v4() const;
    bool is_v6() const;
    const ipv4_address& as_v4() const;
    const ipv6_address& as_v6() const;
    std::string to_string() const;
};
```

`socket_address(std::string_view, uint16_t)` auto-selects IPv6 when the host
contains `:` and IPv4 otherwise. Empty, `"::"`, and `"0.0.0.0"` select the
IPv6-any dual-stack address, which listeners use for all-interface binds.

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
    /* awaitable */ accept(coro::cancel_token token);
    
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
    bool is_valid() const noexcept;
    
    // Read data (awaitable)
    /* awaitable */ read(void* buffer, size_t size);
    /* awaitable */ read(void* buffer, size_t size, coro::cancel_token token);
    template<typename T>
    /* awaitable */ read(std::span<T> buffer);
    template<typename T>
    /* awaitable */ read(std::span<T> buffer, coro::cancel_token token);
    
    // Write data (awaitable)
    /* awaitable */ write(const void* data, size_t size);
    /* awaitable */ write(const void* data, size_t size, coro::cancel_token token);
    template<typename T>
    /* awaitable */ write(std::span<const T> buffer);
    template<typename T>
    /* awaitable */ write(std::span<const T> buffer, coro::cancel_token token);
    /* awaitable */ write(std::string_view data);
    /* awaitable */ write(std::string_view data, coro::cancel_token token);

    // Exact-length helpers (awaitable)
    /* awaitable */ read_exactly(void* buffer, size_t size);
    /* awaitable */ read_exactly(void* buffer, size_t size, coro::cancel_token token);
    template<typename T>
    /* awaitable */ read_exactly(std::span<T> buffer);
    template<typename T>
    /* awaitable */ read_exactly(std::span<T> buffer, coro::cancel_token token);
    /* awaitable */ write_exactly(const void* data, size_t size);
    /* awaitable */ write_exactly(const void* data, size_t size, coro::cancel_token token);
    template<typename T>
    /* awaitable */ write_exactly(std::span<const T> buffer);
    template<typename T>
    /* awaitable */ write_exactly(std::span<const T> buffer, coro::cancel_token token);
    /* awaitable */ write_exactly(std::string_view data);
    /* awaitable */ write_exactly(std::string_view data, coro::cancel_token token);
    
    // Scatter-gather socket write attempt (awaitable) - may return a short write
    /* awaitable */ writev(struct iovec* iovecs, size_t count);
    
    // Poll for readability (awaitable)
    /* awaitable */ poll_read();
    /* awaitable */ poll_read(coro::cancel_token token);
    
    // Poll for writability (awaitable)
    /* awaitable */ poll_write();
    /* awaitable */ poll_write(coro::cancel_token token);
    
    // Get file descriptor
    int fd() const noexcept;
    
    // Get peer address
    std::optional<socket_address> peer_address() const;

    // Lifecycle and socket options
    /* awaitable */ close();
    void shutdown_socket() noexcept;
    bool shutdown(int how) noexcept;
    bool set_no_delay(bool enable);
    bool set_keep_alive(bool enable);
};

// peer_address() returns the generic socket_address wrapper so callers can
// inspect IPv4 or IPv6 peers.
// close() transfers the descriptor to the async close operation and invalidates
// the stream object. shutdown_socket() interrupts both socket directions without
// releasing the descriptor; shutdown(int) is the direct half-close wrapper.
// Multiple reads, multiple writes, and close/shutdown racing with I/O require
// external serialization.

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

    // Convert to sockaddr_un for bind/connect.
    // Throws std::invalid_argument if path is too long for sun_path.
    struct sockaddr_un to_sockaddr() const;
    socklen_t sockaddr_len() const;

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
    bool is_valid() const noexcept;

    // Read data (awaitable)
    /* awaitable */ read(void* buffer, size_t size);
    /* awaitable */ read(void* buffer, size_t size, coro::cancel_token token);
    template<typename T>
    /* awaitable */ read(std::span<T> buffer);
    template<typename T>
    /* awaitable */ read(std::span<T> buffer, coro::cancel_token token);

    // Write data (awaitable)
    /* awaitable */ write(const void* data, size_t size);
    /* awaitable */ write(const void* data, size_t size, coro::cancel_token token);
    template<typename T>
    /* awaitable */ write(std::span<const T> buffer);
    template<typename T>
    /* awaitable */ write(std::span<const T> buffer, coro::cancel_token token);
    /* awaitable */ write(std::string_view data);
    /* awaitable */ write(std::string_view data, coro::cancel_token token);

    // Exact-length helpers (awaitable)
    /* awaitable */ read_exactly(void* buffer, size_t size);
    /* awaitable */ read_exactly(void* buffer, size_t size, coro::cancel_token token);
    template<typename T>
    /* awaitable */ read_exactly(std::span<T> buffer);
    template<typename T>
    /* awaitable */ read_exactly(std::span<T> buffer, coro::cancel_token token);
    /* awaitable */ write_exactly(const void* data, size_t size);
    /* awaitable */ write_exactly(const void* data, size_t size, coro::cancel_token token);
    template<typename T>
    /* awaitable */ write_exactly(std::span<const T> buffer);
    template<typename T>
    /* awaitable */ write_exactly(std::span<const T> buffer, coro::cancel_token token);
    /* awaitable */ write_exactly(std::string_view data);
    /* awaitable */ write_exactly(std::string_view data, coro::cancel_token token);

    // Scatter-gather socket write attempt and readiness polling
    /* awaitable */ writev(struct iovec* iovecs, size_t count);
    /* awaitable */ poll_read();
    /* awaitable */ poll_read(coro::cancel_token token);
    /* awaitable */ poll_write();
    /* awaitable */ poll_write(coro::cancel_token token);

    // Socket metadata and Linux credential passing
    int fd() const noexcept;
    std::optional<unix_address> peer_address() const;
    /* awaitable */ close();
    void shutdown_socket() noexcept;
    bool shutdown(int how) noexcept;
    bool set_recv_buffer(int size);
    bool set_send_buffer(int size);
    bool set_pass_credentials(bool enable);
};

// Connect to UDS address/path (awaitable, returns std::optional<uds_stream>)
/* awaitable */ uds_connect(const unix_address& addr,
                            const uds_options& opts = {});
/* awaitable */ uds_connect(const unix_address& addr,
                            coro::cancel_token token,
                            const uds_options& opts = {});
/* awaitable */ uds_connect(std::string_view path,
                            const uds_options& opts = {});
/* awaitable */ uds_connect(std::string_view path,
                            coro::cancel_token token,
                            const uds_options& opts = {});
```

`unix_address` stores the supplied path or Linux abstract name without doing a
syscall. Conversion for `bind()` or `uds_connect()` requires the address to fit
in `sockaddr_un::sun_path`: filesystem paths need room for a trailing NUL, while
abstract addresses include the leading NUL byte in their stored length. If the
address is too long, `unix_address::to_sockaddr()` throws
`std::invalid_argument` before the bind or connect syscall is attempted.
`uds_listener::bind()` reports socket creation failures, and bind/listen
failures after address conversion succeeds, as `std::nullopt` with `errno` set;
`uds_connect()` reports socket creation/connect failures as `std::nullopt` with
`errno` set from the awaited operation. Cancellable overloads return
`std::nullopt` with `errno == ECANCELED` when the token wins the connect wait.

UDS streams share the same concurrency contract as TCP streams: one reader and
one writer may operate concurrently, but multiple concurrent reads, multiple
concurrent writes, or a read racing with `close()` require external
serialization. `shutdown_socket()` interrupts both directions without releasing
the descriptor; `shutdown(int)` is the direct half-close wrapper. Buffer setters
wrap `SO_RCVBUF` and `SO_SNDBUF`, while `set_pass_credentials()` wraps
`SO_PASSCRED`.

### `net::stream`

Type-erased wrapper over `tcp_stream` and, when TLS support is enabled,
`tls_stream`. It delegates I/O to the active underlying stream.

`net::stream` follows the concurrency contract of its active transport.
TCP-backed streams allow one reader and one writer concurrently, matching
`tcp_stream`. After the handshake completes, TLS-backed streams also allow one
read-side operation and one write-side operation to overlap while waiting for
socket readiness; `tls_stream` serializes direct OpenSSL `SSL*` state access
internally. Multiple concurrent reads, multiple concurrent writes,
handshake-starting operations, or `close()` racing with any read/write operation
require external serialization for all variants.

```cpp
class stream {
public:
    stream();
    explicit stream(tcp_stream tcp);
    explicit stream(tls::tls_stream tls);  // When ELIO_HAS_TLS is enabled

    bool is_connected() const noexcept;
    bool is_secure() const noexcept;

    // Read/write data (awaitable)
    /* awaitable */ read(void* buffer, size_t size);
    /* awaitable */ read(void* buffer, size_t size, coro::cancel_token token);
    /* awaitable */ write(const void* data, size_t size);
    /* awaitable */ write(const void* data, size_t size, coro::cancel_token token);
    /* awaitable */ write(std::string_view data);
    /* awaitable */ write(std::string_view data, coro::cancel_token token);

    // Exact-length helpers (awaitable)
    /* awaitable */ read_exactly(void* buffer, size_t size);
    /* awaitable */ read_exactly(void* buffer, size_t size, coro::cancel_token token);
    /* awaitable */ write_exactly(const void* data, size_t size);
    /* awaitable */ write_exactly(const void* data, size_t size, coro::cancel_token token);
    /* awaitable */ write_exactly(std::string_view data);
    /* awaitable */ write_exactly(std::string_view data, coro::cancel_token token);

    // Compatibility aliases for write_exactly(); prefer write_exactly() in new code
    /* awaitable */ write_all(const void* data, size_t size);
    /* awaitable */ write_all(const void* data, size_t size, coro::cancel_token token);
    /* awaitable */ write_all(std::string_view data);
    /* awaitable */ write_all(std::string_view data, coro::cancel_token token);

    // Close/shutdown the active stream (awaitable)
    /* awaitable */ close();
};
```

---

## HTTP (`elio::http`)

### `url`

Parsed HTTP URL components used by HTTP, WebSocket, SSE, and HTTP/2 clients.

```cpp
struct url {
    std::string scheme;
    std::string host;
    uint16_t port = 0;
    std::string path;
    std::string query;
    std::string fragment;
    std::string userinfo;

    std::string path_with_query() const;
    std::string host_authority() const;
    std::string authority() const;
    std::string to_string() const;
    uint16_t effective_port() const;
    uint16_t default_port() const;
    bool is_secure() const;

    static std::optional<url> parse(std::string_view str);
    static std::optional<url> resolve_reference(
        const url& base,
        std::string_view reference);
};
```

`path_with_query()` intentionally excludes `fragment`, because fragments are
client-side URL components and are not sent in HTTP request targets.
`resolve_reference()` resolves redirect-style `Location` references against a
base URL, including relative paths, query-only references, fragment-only
references, scheme-relative references, and dot-segment removal.

### `client`

HTTP client with connection pooling.

```cpp
class client {
public:
    client();
    explicit client(client_config config);

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

When `follow_redirects` is enabled, the client resolves `Location` values with
`url::resolve_reference()`, rejects unsupported schemes, and rejects HTTPS to
HTTP downgrades.

`websocket::client_config` and `sse::client_config` also inherit
`base_client_config`, including timeout, read-buffer, TLS verification, DNS
resolution/cache, address rotation, and response-header limit settings.

`websocket::client_config` does not define automatic reconnect settings.
`websocket::ws_client::connect()` is a single connection attempt. On connect
failure, close, or read error, Elio reports the result through the operation
return value, `errno` where applicable, and connection state. Cancelling an
operation reports cancellation for that operation, but it is not an automatic
reconnect trigger. The caller owns retry/backoff policy, idempotency, message
replay, and any application session restoration. Callers may explicitly call
`connect()` again after the client is closed.

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

`max_request_size` is an aggregate HTTP request-byte cap. It includes the
request line, headers, and body for the request currently being parsed. The
server rejects a request with `413 Payload Too Large` when parsed request bytes,
buffered request bytes, or the accumulated body exceed the configured cap.
`http::server` can also reject an oversized declared `Content-Length` before
allocating the body. For WebSocket upgrades, only the HTTP upgrade request is
counted; bytes buffered after the completed upgrade belong to the WebSocket
stream and are governed by WebSocket frame/message limits instead. Callers must
choose a value that covers the largest accepted combination of request line,
headers, and body, rather than treating the setting as a body-only limit.

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
    void set_version(std::string_view version);
    void set_header(std::string_view name, std::string_view value);
    void set_body(std::string_view body);
    void set_body(std::string&& body);
    void set_host(std::string_view host);
    void set_content_type(std::string_view type);
    headers& get_headers() noexcept;
    const headers& get_headers() const noexcept;
    
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
`set_version()` accepts an empty value for the default `HTTP/1.1` serialization
or a version token of the form `HTTP/<digits>.<digits>`; invalid values throw
`std::invalid_argument`.

### `headers`

Case-insensitive HTTP header collection used by `request` and `response`.

```cpp
class headers {
public:
    void set(std::string_view name, std::string_view value);
    void add(std::string_view name, std::string_view value);
    std::string_view get(std::string_view name) const;
    std::vector<std::string_view> get_all(std::string_view name) const;
    bool contains(std::string_view name) const;
    void remove(std::string_view name);
    void clear();
};
```

`set()` overwrites the named field. `add()` records an additional field line:
list-valued headers keep the existing `get()` behavior by returning a
comma-joined value, while `get_all()` returns each field-line value
individually.
`Set-Cookie` is not comma-joined; `get()` returns the first cookie value and
`get_all()` returns every cookie field line. Conflicting duplicate
`Content-Length` values throw `std::invalid_argument`.

### `response`

HTTP response message.

```cpp
class response {
public:
    uint16_t status_code() const noexcept;
    status get_status() const noexcept;
    std::string_view version() const noexcept;
    headers& get_headers() noexcept;
    const headers& get_headers() const noexcept;
    
    std::string_view header(std::string_view name) const;
    std::string_view content_type() const;
    std::string_view body() const noexcept;
    
    void set_status(status s) noexcept;
    void set_version(std::string_view version);
    void set_header(std::string_view name, std::string_view value);
    void set_body(std::string_view body);
    void set_body(std::string&& body);
    void set_content_type(std::string_view type);
};
```

`set_version()` follows the same validation rules as `request::set_version()`.

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

## WebSocket (`elio::websocket`)

WebSocket support is declared in `elio::http::websocket` and re-exported as
`elio::websocket` by `<elio/http/websocket.hpp>`.

### `websocket::client_config`

```cpp
struct client_config : http::base_client_config {
    size_t max_message_size = 16 * 1024 * 1024;
    std::vector<std::string> subprotocols;
    std::string origin;
};
```

`websocket::client_config` inherits the shared HTTP client timeout, buffer, TLS
verification, DNS, and response-header limit settings. It does not define an
automatic reconnect loop.

### `websocket::server_config`

```cpp
struct server_config {
    size_t max_message_size = 16 * 1024 * 1024;
    size_t read_buffer_size = 8192;
    std::chrono::seconds ping_interval{30};
    std::chrono::seconds ping_timeout{10};
    std::vector<std::string> subprotocols;
    bool enable_logging = true;
};
```

`ping_interval <= 0` disables the automatic server heartbeat. When enabled,
`ws_server` starts a heartbeat task after a successful upgrade. The task has up
to `ping_timeout` to send a server ping and observe a pong from the route
handler's receive loop; if either step misses that window, the connection fails
closed.
`ping_timeout <= 0` keeps periodic pings but disables timeout closure. A
heartbeat timeout may close the transport without delivering a WebSocket close
frame. The heartbeat does not read from the stream; route handlers remain
responsible for running the single `receive()` loop that processes peer frames
and records pongs.

### `websocket::ws_client`

```cpp
class ws_client {
public:
    ws_client();
    explicit ws_client(client_config config);

    /* awaitable */ connect(std::string_view url);
    /* awaitable */ connect(std::string_view url, coro::cancel_token token);

    connection_state state() const noexcept;
    bool is_open() const noexcept;
    std::string_view subprotocol() const noexcept;

    /* awaitable */ send_text(std::string_view message);
    /* awaitable */ send_binary(std::string_view data);
    /* awaitable */ send_ping(std::string_view payload = "");
    /* awaitable */ send_pong(std::string_view payload = "");
    /* awaitable */ close(close_code code = close_code::normal,
                          std::string_view reason = "");
    /* awaitable */ receive();
    /* awaitable */ receive(coro::cancel_token token);

    tls::tls_context& tls_context() noexcept;
    client_config& config() noexcept;
    const client_config& config() const noexcept;
};
```

`connect()` is one connection attempt. If it fails or a later operation observes
a closed/failed connection, callers choose retry, backoff, replay, and
application session restoration policy.

### `websocket::ws_connection`

Server-side WebSocket connection passed to route handlers.

```cpp
class ws_connection {
public:
    connection_state state() const noexcept;
    bool is_open() const noexcept;
    std::string_view subprotocol() const noexcept;

    std::string_view param(std::string_view name) const;
    const std::unordered_map<std::string, std::string>& params() const noexcept;

    /* awaitable */ send_text(std::string_view message);
    /* awaitable */ send_binary(std::string_view data);
    /* awaitable */ send_ping(std::string_view payload = "");
    /* awaitable */ send_pong(std::string_view payload = "");
    /* awaitable */ close(close_code code = close_code::normal,
                          std::string_view reason = "");
    /* awaitable */ receive();
    /* awaitable */ run_heartbeat(std::chrono::milliseconds ping_interval,
                                  std::chrono::milliseconds ping_timeout,
                                  coro::cancel_token token = {});
};
```

Only one coroutine should receive from a connection at a time. Send helpers,
including heartbeat pings and automatic pong/close responses, are serialized so
concurrent senders do not interleave WebSocket frames.

### `websocket::ws_router` and `ws_server`

```cpp
using ws_handler_func = std::function<coro::task<void>(ws_connection&)>;

class ws_router : public http::router {
public:
    void websocket(std::string_view pattern,
                   ws_handler_func handler,
                   server_config config = {});
};

class ws_server {
public:
    explicit ws_server(ws_router router,
                       http::server_config http_config = {});

    /* awaitable */ listen(const net::socket_address& addr,
                           const net::tcp_options& opts = {});
    /* awaitable */ listen_tls(const net::socket_address& addr,
                               tls::tls_context& tls_ctx,
                               const net::tcp_options& opts = {});
    void stop();
    bool is_running() const noexcept;
    size_t active_connections() const noexcept;
};
```

Route pattern syntax matches the HTTP router: literal components, `:name`
parameters, and trailing `*` wildcards.

### Frame Types and Helpers

```cpp
enum class opcode : uint8_t;
enum class close_code : uint16_t;
enum class connection_state {
    connecting, open, closing, closed
};

enum class endpoint_role {
    unspecified, server, client
};

std::vector<uint8_t> encode_text_frame(std::string_view text, bool mask = false);
std::vector<uint8_t> encode_binary_frame(std::string_view data, bool mask = false);
std::pair<close_code, std::string> parse_close_payload(std::string_view payload);

inline constexpr size_t default_max_message_size = 16 * 1024 * 1024;

class frame_parser {
public:
    // Defaults to default_max_message_size (16 MiB); 0 explicitly disables it.
    void set_max_message_size(size_t max_size);
    void set_role(endpoint_role role);
    close_code error_close_code() const noexcept;
};
```

The raw frame parser is a protocol helper. It rejects aggregate messages above
the 16 MiB `default_max_message_size` unless callers choose another limit with
`set_max_message_size()`; passing `0` explicitly enables unlimited messages. It
enforces masking direction only after callers configure an endpoint role
(`server` or `client`). Applications still validate message payload schemas
after a frame has been accepted.

### `websocket::ws_connect`

```cpp
/* awaitable */ ws_connect(std::string_view url,
                           client_config config = {});
```

Returns an optional connected `ws_client`. It performs one connection attempt;
callers own retry and backoff policy.

---

## Server-Sent Events (`elio::sse`)

SSE support is declared in `elio::http::sse` and re-exported as `elio::sse` by
`<elio/http/sse.hpp>`.

### `sse::event`

```cpp
struct event {
    std::string id;
    std::string type;
    std::string data;
    int retry = -1;

    static event message(std::string_view data);
    static event typed(std::string_view type, std::string_view data);
    static event with_id(std::string_view id, std::string_view data);
    static event full(std::string_view id, std::string_view type,
                      std::string_view data, int retry = -1);
};
```

### `sse::sse_connection`

Server-side SSE connection passed to handlers.

```cpp
enum class connection_state {
    active, closed
};

class sse_connection {
public:
    connection_state state() const noexcept;
    bool is_active() const noexcept;
    std::string_view last_event_id() const noexcept;

    /* awaitable */ send(const event& evt);
    /* awaitable */ send_data(std::string_view data);
    /* awaitable */ send_event(std::string_view type, std::string_view data);
    /* awaitable */ send_comment(std::string_view comment = "");
    /* awaitable */ send_retry(int retry_ms);
    void close();
    void set_active();
    /* awaitable */ run_heartbeat(
        std::chrono::milliseconds interval = std::chrono::seconds(30),
        coro::cancel_token token = {},
        std::string_view comment = "ping");
};
```

SSE sends are serialized by the connection object. Application code owns event
schema, authorization, replay, and duplicate-handling policy.

### `sse::sse_endpoint` and Response Helpers

```cpp
using sse_handler_func = std::function<coro::task<void>(sse_connection&)>;

class sse_endpoint {
public:
    explicit sse_endpoint(sse_handler_func handler);
    const sse_handler_func& handler() const;
};

http::response build_sse_response();
```

`build_sse_response()` prepares the HTTP headers for an SSE stream. Applications
still own routing and handler lifetime.

### `sse::client_config`

```cpp
struct client_config : http::base_client_config {
    int default_retry_ms = 3000;
    bool auto_reconnect = true;
    size_t max_reconnect_attempts = 0;
    size_t max_event_buffer_size = 1024 * 1024;
    std::string last_event_id;
};
```

### `sse::sse_client`

```cpp
enum class client_state {
    disconnected, connecting, connected, reconnecting, closed
};

class sse_client {
public:
    sse_client();
    explicit sse_client(client_config config);

    /* awaitable */ connect(std::string_view url);
    /* awaitable */ connect(std::string_view url, coro::cancel_token token);

    client_state state() const noexcept;
    bool is_connected() const noexcept;
    std::string_view last_event_id() const noexcept;

    /* awaitable */ receive();
    /* awaitable */ receive(coro::cancel_token token);
    /* awaitable */ close();

    tls::tls_context& tls_context() noexcept;
    client_config& config() noexcept;
    const client_config& config() const noexcept;
};
```

The client tracks `Last-Event-ID`, applies configured event-buffer limits, and
uses `client_config::auto_reconnect` / `max_reconnect_attempts` for
receive-driven reconnect behavior after an established stream fails while not
closed. Initial `connect()` failures are single attempts; callers own retry and
background reconnect policy for initial connection establishment.

### `sse::sse_connect`

```cpp
/* awaitable */ sse_connect(std::string_view url,
                            client_config config = {});
/* awaitable */ sse_connect(std::string_view url,
                            coro::cancel_token token,
                            client_config config = {});
```

Returns an optional connected `sse_client`.

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
    explicit h2_client(h2_client_config config);

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

`h2_client::send()` accepts ordinary request methods for HTTPS targets. HTTP/2
`CONNECT` is not implemented by the high-level client; passing
`method::CONNECT` fails before opening a connection, sets `errno` to
`EOPNOTSUPP`, and returns `std::nullopt`.

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
    explicit h2_session(tls::tls_stream& stream,
                        h2_session_config config = {});
    
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

`submit_request()` validates HTTPS targets, request-target bytes, authority
bytes, generated header values, and rejects `method::CONNECT` before submitting
to nghttp2. Validation failures return a negative errno-style value and set
`errno`; unsupported `CONNECT` requests return `-EOPNOTSUPP`.

---

## TLS (`elio::tls`)

### `tls_context`

TLS configuration context.

```cpp
enum class tls_version {
    tls_1_2,
    tls_1_3,
    tls_1_2_or_higher,
    tls_1_3_only
};

enum class tls_mode {
    client,
    server
};

enum class verify_mode {
    none,
    peer,
    fail_if_no_cert
};

class tls_context {
public:
    explicit tls_context(tls_mode mode, tls_version version = tls_version::tls_1_2_or_higher);
    
    // Load certificate, key, and trust material
    bool load_certificate(std::string_view cert_file);
    bool load_private_key(std::string_view key_file,
                          std::string_view password = "");
    bool load_verify_locations(std::string_view ca_file = "",
                               std::string_view ca_path = "");
    bool use_default_verify_paths();
    
    // Certificate verification
    void set_verify_mode(verify_mode mode);
    
    // ALPN and cipher configuration
    bool set_alpn_protocols(std::string_view protocols);
    bool set_ciphers(std::string_view ciphers);
    bool set_ciphersuites(std::string_view ciphersuites);

    SSL_CTX* native_handle() noexcept;
    const SSL_CTX* native_handle() const noexcept;
    tls_mode mode() const noexcept;

    static tls_context make_client();
    static tls_context make_server(std::string_view cert_file,
                                   std::string_view key_file);
};
```

### `tls_stream`

TLS-wrapped TCP stream.

After the TLS handshake completes, `tls_stream` serializes direct OpenSSL
`SSL*` state access internally. One read-side operation and one write-side
operation may overlap while either side is suspended on socket readiness.
Callers must still serialize handshake-starting operations, multiple concurrent
reads, multiple concurrent writes, and shutdown/destruction against active I/O
at the protocol layer.

```cpp
class tls_stream {
public:
    tls_stream(net::tcp_stream tcp, tls_context& ctx);
    
    // Set SNI hostname
    void set_hostname(std::string_view hostname);
    
    // Perform TLS handshake (awaitable)
    /* awaitable */ handshake();
    /* awaitable */ handshake(coro::cancel_token token);

    // Graceful TLS close_notify with a bounded wait (awaitable)
    /* awaitable */ shutdown(std::chrono::milliseconds timeout =
                                 std::chrono::milliseconds(2000));
    
    // Read decrypted data (awaitable)
    /* awaitable */ read(void* buffer, size_t size);
    /* awaitable */ read(void* buffer, size_t size, coro::cancel_token token);
    
    // Write data to encrypt (awaitable)
    /* awaitable */ write(const void* data, size_t size);
    /* awaitable */ write(const void* data, size_t size, coro::cancel_token token);
    /* awaitable */ write(std::string_view data);
    /* awaitable */ write(std::string_view data, coro::cancel_token token);

    // Exact-length helpers (awaitable)
    /* awaitable */ read_exactly(void* buffer, size_t size);
    /* awaitable */ read_exactly(void* buffer, size_t size, coro::cancel_token token);
    template<typename T>
    /* awaitable */ read_exactly(std::span<T> buffer);
    template<typename T>
    /* awaitable */ read_exactly(std::span<T> buffer, coro::cancel_token token);
    /* awaitable */ write_exactly(const void* data, size_t size);
    /* awaitable */ write_exactly(const void* data, size_t size, coro::cancel_token token);
    template<typename T>
    /* awaitable */ write_exactly(std::span<const T> buffer);
    template<typename T>
    /* awaitable */ write_exactly(std::span<const T> buffer, coro::cancel_token token);
    /* awaitable */ write_exactly(std::string_view data);
    /* awaitable */ write_exactly(std::string_view data, coro::cancel_token token);
    
    // Get negotiated ALPN protocol
    std::string_view alpn_protocol() const;
    const char* version() const;
    const char* cipher() const;

    int fd() const noexcept;
    const net::tcp_stream& tcp() const noexcept;
    bool is_handshake_complete() const noexcept;

    // Watchdog helpers for externally interrupted sockets
    void mark_externally_shut_down() noexcept;
    void shutdown_socket() noexcept;

    X509* peer_certificate() const;
    long verify_result() const;
};
```

`shutdown()` is the graceful TLS close path. It sends `close_notify` and waits
for the peer's `close_notify` within the supplied wall-clock budget. Callers
must still serialize shutdown and destruction against active reads/writes.
`shutdown_socket()` and `mark_externally_shut_down()` are watchdog-oriented
helpers for code that has already interrupted the underlying TCP socket and
needs the stream to skip a later `SSL_shutdown()` write.

`peer_certificate()` returns a caller-owned `X509*` when a peer certificate is
available; callers must release each non-null result with `X509_free()` when
done.

### `tls_connect()`

```cpp
coro::task<std::optional<tls_stream>>
tls_connect(tls_context& ctx,
            std::string_view host,
            uint16_t port,
            net::resolve_options resolve_opts =
                net::default_cached_resolve_options());
```

`tls_connect()` resolves the host, connects TCP, applies SNI from `host`, and
performs the TLS handshake. It returns `std::optional<tls_stream>`; an empty
optional means resolution, TCP connect, or TLS handshake failed.

### `tls_listener`

```cpp
class tls_listener {
public:
    tls_listener(net::tcp_listener tcp, tls_context& ctx);

    // Accept TCP and complete the server-side TLS handshake
    coro::task<std::optional<tls_stream>> accept();

    int fd() const noexcept;

    static std::optional<tls_listener>
    bind(const net::ipv4_address& addr, tls_context& ctx);
    static std::optional<tls_listener>
    bind(const net::ipv6_address& addr, tls_context& ctx);
    static std::optional<tls_listener>
    bind(const net::socket_address& addr, tls_context& ctx);
};
```

`tls_listener` keeps a pointer to the supplied `tls_context`; callers must keep
that context alive while accepts are pending and while accepted streams use it.

---

## Synchronization (`elio::sync`)

### Cancellation Safety

Coroutine-aware synchronization primitives (`mutex`, `shared_mutex`, `event`, `semaphore`, `condition_variable`, `channel`) track suspended waiters through intrusive nodes embedded in coroutine frames. If a frame is destroyed while its waiter is still linked, the awaiter's destructor unlinks the node from the primitive.

This cleanup does not make every operation cancellable. `with_timeout()`
requests cooperative cancellation but does not forcibly destroy its losing
child. It joins the child before returning, so the child must pass the supplied
`cancel_token` or `this_coro::cancel_token()` to a token-aware operation to stop
promptly. Every suspending core synchronization primitive provides an explicit
token-aware wait. Their no-token overloads remain non-cancellable and preserve
their existing await results.

Cancellation and normal notification atomically select one terminal result. A cancellation winner does not acquire a lock or permit, transfer a pending send value into a channel, or consume a channel receive value. Once normal notification wins, the operation returns `completed` and any acquired resource or completed transfer remains caller-owned. A condition wait that released an associated lock re-acquires it before returning either result. Channel result objects carry both `cancel_result` and the existing sent/value result so cancellation remains distinct from closure.

Callers must inspect the `cancel_result` from every token-aware wait before assuming that notification occurred, a lock or permit was acquired, or a channel operation observed closure rather than cancellation.

### `mutex`

Coroutine-aware mutex.

```cpp
class mutex {
public:
    mutex();
    
    // Acquire lock (awaitable)
    /* awaitable */ lock();

    // Acquire or return cancel_result::cancelled
    /* awaitable<cancel_result> */ lock(coro::cancel_token token);
    
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

    // Acquire shared lock or return cancel_result::cancelled
    /* awaitable<cancel_result> */ lock_shared(coro::cancel_token token);
    
    // Acquire exclusive (write) lock (awaitable)
    /* awaitable */ lock();

    // Acquire exclusive lock or return cancel_result::cancelled
    /* awaitable<cancel_result> */ lock(coro::cancel_token token);
    
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

RAII unlock guard for an already-held shared (reader) lock. Callers must
`co_await lock_shared()` before constructing the guard.

```cpp
class shared_lock_guard {
public:
    explicit shared_lock_guard(shared_mutex& m);
    ~shared_lock_guard();  // Calls unlock_shared()
    
    void unlock();  // Manual early unlock
};
```

### `unique_lock_guard`

RAII unlock guard for an already-held exclusive (writer) lock. Callers must
`co_await lock()` before constructing the guard.

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

    // Cancellation-aware wait; re-acquires m if the wait released it
    coro::task<coro::cancel_result> wait(mutex& m, coro::cancel_token token);

    // Wait with a generic lockable (e.g., spinlock)
    // Re-acquires the lock synchronously before resuming
    template<lockable Lock>
    /* awaitable */ wait(Lock& lock);

    template<lockable Lock>
    /* awaitable<cancel_result> */ wait(Lock& lock, coro::cancel_token token);

    // Wait without external lock (single-worker-thread only)
    /* awaitable */ wait_unlocked();

    /* awaitable<cancel_result> */ wait_unlocked(coro::cancel_token token);

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

    // Wait or return cancel_result::cancelled
    /* awaitable<cancel_result> */ wait(coro::cancel_token token);

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
    struct cancellable_send_result {
        bool sent;
        coro::cancel_result cancel;

        bool success() const noexcept;
        bool was_cancelled() const noexcept;
        bool was_closed() const noexcept;
    };

    struct cancellable_recv_result {
        std::optional<T> value;
        coro::cancel_result cancel;

        bool success() const noexcept;
        bool was_cancelled() const noexcept;
        bool was_closed() const noexcept;
    };

    // Create a rendezvous channel (default, capacity 0)
    channel();

    // Create a bounded channel with specified capacity
    explicit channel(size_t capacity);

    // Create an unbounded channel
    static channel unbounded();

    // Send a value (awaitable, blocks if full for bounded channels)
    /* awaitable */ send(T value);

    // Send, report cancellation separately from channel closure
    /* awaitable<cancellable_send_result> */
    send(T value, coro::cancel_token token);

    // Receive a value (awaitable, blocks if empty)
    /* awaitable<std::optional<T>> */ recv();

    // Receive, report cancellation separately from channel closure
    /* awaitable<cancellable_recv_result> */
    recv(coro::cancel_token token);

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

    // Acquire or return cancel_result::cancelled
    /* awaitable<cancel_result> */ acquire(coro::cancel_token token);

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

`sleep_for()` and `sleep_until()` normally use the current worker's I/O
backend. If timer preparation fails, they route the wait through the scheduler
blocking pool while preserving worker affinity. If that pool is unavailable,
the await continues on its current worker and throws `std::runtime_error`;
this rejection is not reported as token cancellation.

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

### CRC32C

The header-only CRC32C entry points use translation-unit-local linkage so
sources compiled with different ISA flags keep independent dispatch paths.

```cpp
// Compute CRC32C (Castagnoli polynomial)
uint32_t crc32c(const void* data, size_t length, uint32_t crc = 0xFFFFFFFF);
uint32_t crc32c(std::span<const uint8_t> data, uint32_t crc = 0xFFFFFFFF);

// Check whether this translation unit has a compiled hardware CRC32C path
// supported by the current CPU
bool crc32c_hw_available() noexcept;
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

Non-owning reference to external buffer data. Deserialization can return a
`buffer_ref` view into the received payload without copying. Serialization of a
`buffer_ref` currently copies the referenced bytes into the outgoing
`buffer_writer`; true send-side zero-copy/iovec-tail support is not implemented.

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

RPC CRC32 checksums are non-cryptographic corruption checks. They are not
authentication, authorization, or tamper protection against adversarial peers.
Use TLS/mTLS or an application MAC/signature layer when the protocol needs a
security boundary.

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
    no_response = 1 << 4,   // one-way request: suppress response/error
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
    resource_exhausted = 8,
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

// Build one-way request frame
template<typename Request>
std::pair<frame_header, buffer_writer> build_oneway_request(
    uint32_t request_id,
    method_id_t method_id,
    const Request& request,
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

Cleanup callbacks returned by `register_method_with_cleanup()` and
`register_method_with_context_and_cleanup()` run only after the response has
been successfully sent. For `no_response` one-way requests, no response is sent;
the callback runs after the handler result has been serialized and the unsent
payload can be discarded. They are not failure-path finalizers: handler errors,
serialization errors, disconnects, write failures, and `send_response`
exceptions skip cleanup, so handlers must not rely on the callback for mandatory
local failure cleanup.

#### `rpc_server_config`

```cpp
enum class rpc_request_overload_policy {
    reject_request,
    close_session,
};
```

```cpp
struct rpc_server_config {
    size_t max_sessions = 1024;
    std::chrono::seconds frame_read_timeout{30};
    uint32_t max_message_size = elio::rpc::max_message_size;
    size_t max_in_flight_requests_per_session = 0;
    rpc_request_overload_policy request_overload_policy =
        rpc_request_overload_policy::reject_request;
};
```

- `max_sessions`: Maximum concurrent sessions accepted by `serve()`.
  `0` disables the cap.
- `frame_read_timeout`: Per-frame deadline for receiving the header,
  payload, and optional checksum. `0s` disables the deadline.
- `max_message_size`: Maximum payload bytes accepted per frame. The
  default is the protocol-wide 16 MiB limit.
- `max_in_flight_requests_per_session`: Maximum active request slots for one
  client session. A response-capable request holds its slot until its handler
  has produced a result and the response/error send path has completed or
  failed; a `no_response` request holds its slot until its handler finishes.
  `0` preserves legacy unlimited concurrency.
- `request_overload_policy`: Strategy after the per-session in-flight cap is
  reached. `reject_request` sends `rpc_error::resource_exhausted` for a
  response-capable excess request when no previous overload rejection is still
  being written, and drops over-limit `no_response` requests. If the peer is not
  draining responses and an overload rejection is already in flight, additional
  excess requests may also be dropped; a waiting client call for such a dropped
  request completes only through its own timeout or connection close.
  `close_session` closes the session.
  Accepted `no_response` requests still count against the cap until their
  handlers finish. Duplicate active request IDs close the session and do not
  consume capacity.

Server pong writes are bounded to one in-flight pong per session. If a peer
sends more pings while a previous pong is still being written, later pings may
not receive a pong; clients should rely on their own ping timeout and connection
policy.

Every session owns accepted request handlers, pong writes, and overload
responses through one scheduler-bound task scope. When the session closes or
the task awaiting `handle_client()` is cancelled, the server wakes pending
frame reads, requests the explicit `rpc_context::cancel_token` and the handler's
runtime `coro::this_coro::cancel_token()`, cancels pending frame writes and
response-lock waits, and joins all accepted children before `handle_client()`
returns or the session slot is released. Cancellation is cooperative: handlers
must pass one of these tokens to waits that should stop, and code that ignores
both tokens can delay session teardown. This structured ownership does not
change request admission or overload policy. Session tracking and slot state are
shared with draining sessions. Keep the server facade alive until active
`serve()` and `handle_client()` calls return; after a stopped `serve()` call has
returned, the facade may be released before cooperative handler drain completes.

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

#### `rpc_client_config`

```cpp
struct rpc_client_config {
    std::chrono::seconds frame_read_timeout{30};
    uint32_t max_message_size = elio::rpc::max_message_size;
};
```

- `frame_read_timeout`: Inactivity deadline for each client receive-loop frame
  read. The timer starts while waiting for the next frame header and continues
  until the complete response/control frame, including payload and optional
  checksum, is read. Expiry closes the client connection and completes pending
  calls as `rpc_error::connection_closed`. `0s` disables the deadline; use a
  larger value or `0s` for long-lived idle clients or calls whose server may be
  silent longer than the default.
- `max_message_size`: Maximum payload bytes accepted per frame. The default is
  the protocol-wide 16 MiB limit.

#### `rpc_client<Stream>`

```cpp
template<typename Stream>
class rpc_client {
public:
    using ptr = std::shared_ptr<rpc_client>;

    static std::shared_ptr<rpc_client> create(
        Stream stream,
        rpc_client_config config = {});

    template<typename... Args>
    static coro::task<std::optional<std::shared_ptr<rpc_client>>>
        connect(Args&&... args);

    template<typename... Args>
    static coro::task<std::optional<std::shared_ptr<rpc_client>>>
        connect_with_config(rpc_client_config config, Args&&... args);

    bool is_connected() const noexcept;
    bool start();
    void close();

    template<typename Method>
    coro::task<rpc_result<typename Method::response_type>>
        call(const typename Method::request_type& request);

    template<typename Method, typename Rep, typename Period>
    coro::task<rpc_result<typename Method::response_type>>
        call(const typename Method::request_type& request,
             std::chrono::duration<Rep, Period> timeout);

    template<typename Method>
    coro::task<rpc_result<typename Method::response_type>>
        call(const typename Method::request_type& request,
             coro::cancel_token token);

    template<typename Method, typename Rep, typename Period>
    coro::task<rpc_result<typename Method::response_type>>
        call(const typename Method::request_type& request,
             std::chrono::duration<Rep, Period> timeout,
             coro::cancel_token token);

    template<typename Method>
    coro::task<bool> send_oneway(
        const typename Method::request_type& request);

    coro::task<bool> ping(
        std::chrono::milliseconds timeout = std::chrono::milliseconds(5000));

    Stream& stream() noexcept;
    const Stream& stream() const noexcept;
    const rpc_client_config& config() const noexcept;
};

using tcp_rpc_client = rpc_client<net::tcp_stream>;
using uds_rpc_client = rpc_client<net::uds_stream>;
```

`create()` constructs a client from an existing stream and attempts to start the
background receive loop immediately. If it is called outside a scheduler,
`start()` returns `false`; call `start()` from a scheduler before issuing
response-bearing operations such as `call()` or `ping()`.

`connect()` and `connect_with_config()` perform the transport connection from a
scheduler context and return a client with its receive loop started on success.

Call `close()` explicitly when the client is no longer needed. The receive loop
can hold a strong reference while blocked in a read, so dropping the last
external `shared_ptr` is not a substitute for closing the underlying stream and
waking pending calls.

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
