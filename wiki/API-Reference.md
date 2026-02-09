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

---

## Coroutines (`elio::coro`)

### `task<T>`

The primary coroutine type.

```cpp
template<typename T = void>
class task {
public:
    using promise_type = /* implementation */;
    
    task(task&& other) noexcept;
    task& operator=(task&& other) noexcept;
    
    // Get the coroutine handle (does not transfer ownership)
    std::coroutine_handle<> handle() const noexcept;
    
    // Release ownership of the coroutine handle (marks as detached)
    std::coroutine_handle<> release() noexcept;
    
    // Spawn on current scheduler (fire-and-forget)
    void go();
    
    // Spawn on current scheduler (joinable, returns handle to await)
    join_handle<T> spawn();
    
    // Check if task holds a valid coroutine
    explicit operator bool() const noexcept;
};
```

**Task Spawning Examples:**
```cpp
// Fire-and-forget: spawn and don't wait for result
some_task().go();

// Joinable spawn: get a handle to await later
auto handle = compute_value().spawn();
// ... do other work concurrently ...
int result = co_await handle;  // Wait and get result

// Multiple concurrent tasks
auto h1 = task_a().spawn();
auto h2 = task_b().spawn();
auto h3 = task_c().spawn();
// All three run concurrently
int a = co_await h1;
int b = co_await h2;
int c = co_await h3;
```

### `join_handle<T>`

Handle for awaiting spawned tasks. Returned by `task<T>::spawn()`.

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
    auto handle = compute().spawn();
    
    // Check completion without blocking
    if (!handle.is_ready()) {
        // Do other work while waiting...
    }
    
    // Await the result
    int result = co_await handle;
    std::cout << "Result: " << result << std::endl;
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
    [[nodiscard]] registration on_cancel_resume(std::coroutine_handle<> h) const;
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
task<void> cancellable_work(cancel_token token) {
    while (!token.is_cancelled()) {
        // Do some work...
        
        // Cancellable sleep
        auto result = co_await time::sleep_for(100ms, token);
        if (result == cancel_result::cancelled) {
            break;  // Exit early
        }
    }
}

task<void> controller() {
    cancel_source source;
    
    // Start work with token
    cancellable_work(source.get_token()).go();
    
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

---

## Runtime (`elio::runtime`)

### `scheduler`

Manages coroutine execution across worker threads.

```cpp
class scheduler {
public:
    // Create scheduler with specified number of workers and wait strategy
    explicit scheduler(size_t num_workers = std::thread::hardware_concurrency(),
                       wait_strategy strategy = wait_strategy::blocking());
    ~scheduler();
    
    // Start worker threads
    void start();
    
    // Stop all workers and wait for completion
    void shutdown();
    
    // Spawn a coroutine for execution
    void spawn(std::coroutine_handle<> handle);
    
    // Spawn a task directly (convenience overload)
    template<typename Task>
    void spawn(Task&& t);  // Accepts any type with release() method
    
    // Get number of worker threads
    size_t worker_count() const noexcept;
    
    // Get the current scheduler (thread-local)
    static scheduler* current() noexcept;
};
```

**Example:**
```cpp
runtime::scheduler sched(4);
sched.start();

// Spawn tasks directly
sched.spawn(my_coroutine());  // Accepts task directly

sched.shutdown();
```

### `worker_thread`

Individual worker that executes tasks. Workers use an efficient idle mechanism with futex-based wake-up.

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
- Workers block efficiently on futex when no tasks are available
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
};
```

### `run()`

Run a coroutine to completion.

```cpp
// Run task with configuration
template<typename T>
T run(coro::task<T> task, const run_config& config = {});

// Run task with specified thread count
template<typename T>
T run(coro::task<T> task, size_t num_threads);
```

**Example:**
```cpp
coro::task<int> async_main(int argc, char* argv[]) {
    co_return 42;
}

int main(int argc, char* argv[]) {
    return elio::run(async_main(argc, argv));
}

// With configuration
int main(int argc, char* argv[]) {
    elio::run_config config;
    config.num_threads = 4;
    return elio::run(async_main(argc, argv), config);
}
```

### `ELIO_ASYNC_MAIN` Macros

Macros to define main() that runs an async_main coroutine.

| Macro | async_main signature | Description |
|-------|---------------------|-------------|
| `ELIO_ASYNC_MAIN` | `task<int>(int, char**)` | With args, returns exit code |
| `ELIO_ASYNC_MAIN_VOID` | `task<void>(int, char**)` | With args, always exits 0 |
| `ELIO_ASYNC_MAIN_NOARGS` | `task<int>()` | No args, returns exit code |
| `ELIO_ASYNC_MAIN_VOID_NOARGS` | `task<void>()` | No args, always exits 0 |

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
template<typename Server, typename ListenTask>
coro::task<void> serve(Server& server, ListenTask listen_task,
                       std::initializer_list<int> signals = {SIGINT, SIGTERM});
```

The function:
1. Spawns the listen task in the background
2. Waits for a shutdown signal (SIGINT or SIGTERM by default)
3. Calls `server.stop()` when signal is received
4. Waits for the listen task to complete

**Example:**
```cpp
coro::task<int> async_main(int argc, char* argv[]) {
    http::router r;
    r.get("/", handler);

    http::server srv(r);

    // serve() handles everything: listen, wait for Ctrl+C, stop cleanly
    co_await elio::serve(srv, srv.listen(addr));

    co_return 0;
}

ELIO_ASYNC_MAIN(async_main)
```

### `serve_all()`

Run multiple servers until shutdown.

```cpp
template<typename... Servers, typename... ListenTasks>
coro::task<void> serve_all(std::tuple<Servers&...> servers,
                           std::tuple<ListenTasks...> listen_tasks,
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
            http_srv.listen(http_addr),
            ws_srv.listen(ws_addr)
        )
    );
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
    size_t poll(std::chrono::milliseconds timeout = std::chrono::milliseconds::zero());
    
    // Check if there are pending operations
    bool has_pending() const noexcept;
    
    // Get the I/O backend
    io_backend& backend() noexcept;
};

// Get the default global I/O context
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
    bool block_all_threads() const;  // Block process-wide (call before threads)
};
```

### `signal_fd`

Async-friendly signalfd wrapper.

```cpp
class signal_fd {
public:
    // Create signalfd (auto_block=true blocks signals automatically)
    explicit signal_fd(const signal_set& signals, bool auto_block = true);
    
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
/* awaitable */ wait_signal(const signal_set& signals, bool auto_block = true);

/* awaitable */ wait_signal(int signo);

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
    
    auto sig_handler = signal_handler_task();
    sched.spawn(sig_handler.release());
    
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
class ipv4_address {
public:
    // Bind to all interfaces on port
    explicit ipv4_address(uint16_t port);
    
    // Specific IP and port (IP can be hostname)
    ipv4_address(const std::string& ip, uint16_t port);
    
    // Get components
    uint32_t ip() const noexcept;
    uint16_t port() const noexcept;
    
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
    
    // Accept a connection (awaitable, returns std::optional<tcp_stream>)
    /* awaitable */ accept();
    
    // Get file descriptor
    int fd() const noexcept;
    
    // Get local address
    std::optional<ipv4_address> local_address() const;
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
    
    // Write data (awaitable)
    /* awaitable */ write(const void* data, size_t size);
    
    // Scatter-gather write (awaitable) - writes multiple buffers atomically
    /* awaitable */ writev(struct iovec* iovecs, size_t count);
    
    // Poll for readability (awaitable)
    /* awaitable */ poll_read();
    
    // Poll for writability (awaitable)
    /* awaitable */ poll_write();
    
    // Get file descriptor
    int fd() const noexcept;
    
    // Get peer address
    std::optional<ipv4_address> peer_address() const;
    
    // Get local address
    std::optional<ipv4_address> local_address() const;
};

// Connect to address (awaitable, returns std::optional<tcp_stream>)
/* awaitable */ tcp_connect(const ipv4_address& addr);
```

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
    /* awaitable */ get(const std::string& url);

    // POST request (awaitable)
    /* awaitable */ post(const std::string& url,
                         const std::string& body,
                         const std::string& content_type);

    // HEAD request (awaitable)
    /* awaitable */ head(const std::string& url);

    // Send custom request (awaitable)
    /* awaitable */ send(const request& req, const url& target);
};

// Convenience function for one-off GET
/* awaitable */ get(const std::string& url);
```

### `client_config`

```cpp
struct client_config {
    std::string user_agent = "elio-http-client/1.0";
    bool follow_redirects = true;
    int max_redirects = 10;
    std::chrono::seconds timeout{30};
};
```

### `request`

HTTP request message.

```cpp
class request {
public:
    request(method m, const std::string& path);
    
    void set_host(const std::string& host);
    void set_header(const std::string& name, const std::string& value);
    void set_body(const std::string& body);
    
    method method() const;
    const std::string& path() const;
    std::string header(const std::string& name) const;
    const std::string& body() const;
};
```

### `response`

HTTP response message.

```cpp
class response {
public:
    int status_code() const;
    status get_status() const;
    
    std::string header(const std::string& name) const;
    std::string content_type() const;
    const std::string& body() const;
    
    void set_status(status s);
    void set_header(const std::string& name, const std::string& value);
    void set_body(const std::string& body);
};
```

### HTTP Enums

```cpp
enum class method {
    GET, HEAD, POST, PUT, DELETE, PATCH, OPTIONS
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

HTTP/2 client with connection multiplexing.

```cpp
class h2_client {
public:
    h2_client();
    explicit h2_client(const h2_client_config& config);

    // GET request (awaitable)
    /* awaitable */ get(const std::string& url);

    // POST request (awaitable)
    /* awaitable */ post(const std::string& url,
                         const std::string& body,
                         const std::string& content_type);

    // PUT request (awaitable)
    /* awaitable */ put(const std::string& url,
                        const std::string& body,
                        const std::string& content_type);

    // DELETE request (awaitable)
    /* awaitable */ del(const std::string& url);

    // PATCH request (awaitable)
    /* awaitable */ patch(const std::string& url,
                          const std::string& body,
                          const std::string& content_type);

    // Send custom request (awaitable)
    /* awaitable */ send(method m, const url& target,
                         std::string_view body = {},
                         std::string_view content_type = {});

    // Access TLS context for configuration
    tls_context& tls_context();
};

// Convenience function for one-off HTTP/2 GET
/* awaitable */ h2_get(const std::string& url);

// Convenience function for one-off HTTP/2 POST
/* awaitable */ h2_post(const std::string& url,
                        const std::string& body, const std::string& content_type);
```

### `h2_client_config`

```cpp
struct h2_client_config {
    std::chrono::seconds connect_timeout{10};
    std::chrono::seconds read_timeout{30};
    size_t max_concurrent_streams = 100;
    uint32_t initial_window_size = 65535;
    std::string user_agent = "elio-http2/1.0";
    bool enable_push = false;  // Server push (rarely needed)
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
    explicit tls_context(tls_method method);
    
    // Load certificate and key
    bool use_certificate_file(const std::string& path);
    bool use_private_key_file(const std::string& path);
    
    // Certificate verification
    bool use_default_verify_paths();
    void set_verify_mode(bool verify);
    
    // ALPN protocol negotiation
    void set_alpn_protocols(const std::vector<std::string>& protocols);
};

enum class tls_method {
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

### `condition_variable`

Coroutine-aware condition variable.

```cpp
class condition_variable {
public:
    condition_variable();
    
    // Wait for notification (awaitable)
    /* awaitable */ wait(unique_lock<mutex>& lock);
    
    // Wait with predicate (awaitable)
    template<typename Pred>
    /* awaitable */ wait(unique_lock<mutex>& lock, Pred pred);
    
    // Notify one waiter
    void notify_one();
    
    // Notify all waiters
    void notify_all();
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
// Returns cancel_result::completed or cancel_result::cancelled
template<typename Rep, typename Period>
/* awaitable<cancel_result> */ sleep_for(std::chrono::duration<Rep, Period> duration,
                                          coro::cancel_token token);

// Sleep until time point
template<typename Clock, typename Duration>
/* awaitable */ sleep_until(std::chrono::time_point<Clock, Duration> tp);

// Yield execution to other coroutines
/* awaitable */ yield();
```

**Example:**
```cpp
task<void> example(coro::cancel_token token) {
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
struct frame_header {
    uint32_t magic;           // 0x454C494F ("ELIO")
    uint32_t request_id;
    message_type type;
    message_flags flags;
    method_id_t method_id;
    uint32_t payload_length;
    
    bool is_valid() const noexcept;
    std::array<uint8_t, 18> to_bytes() const;
    static frame_header from_bytes(const uint8_t* data);
};
```

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
    
    bool has_timeout() const noexcept;
};
```

#### `cleanup_callback_t`

```cpp
using cleanup_callback_t = std::function<void()>;
```

#### `rpc_server<Stream>`

```cpp
template<typename Stream>
class rpc_server {
public:
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
