# API Reference

This page provides a reference for Elio's public API.

## Namespaces

| Namespace | Description |
|-----------|-------------|
| `elio` | Root namespace |
| `elio::coro` | Coroutine types |
| `elio::runtime` | Scheduler and workers |
| `elio::io` | I/O context and backends |
| `elio::net` | TCP networking |
| `elio::http` | HTTP client/server |
| `elio::tls` | TLS/SSL support |
| `elio::sync` | Synchronization primitives |
| `elio::time` | Timers |
| `elio::log` | Logging |

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
    
    // Release ownership of the coroutine handle
    std::coroutine_handle<> release() noexcept;
    
    // Check if task holds a valid coroutine
    explicit operator bool() const noexcept;
};
```

---

## Runtime (`elio::runtime`)

### `scheduler`

Manages coroutine execution across worker threads.

```cpp
class scheduler {
public:
    // Create scheduler with specified number of workers
    explicit scheduler(size_t num_workers = std::thread::hardware_concurrency());
    ~scheduler();
    
    // Start worker threads
    void start();
    
    // Stop all workers and wait for completion
    void shutdown();
    
    // Spawn a coroutine for execution
    void spawn(std::coroutine_handle<> handle);
    
    // Set the I/O context for workers to poll
    void set_io_context(io::io_context* ctx);
    
    // Get number of worker threads
    size_t worker_count() const noexcept;
    
    // Get the current scheduler (thread-local)
    static scheduler* current() noexcept;
};
```

### `run()`

Run a coroutine to completion.

```cpp
// Run task with default thread count (hardware concurrency)
template<typename T>
T run(coro::task<T> task, size_t num_threads = std::thread::hardware_concurrency());
```

**Example:**
```cpp
coro::task<int> async_main() {
    co_return 42;
}

int main() {
    return elio::run(async_main());
}
```

### `ELIO_ASYNC_MAIN`

Macro to define main() that runs an async_main coroutine.

```cpp
// For async_main returning int (used as exit code)
ELIO_ASYNC_MAIN(async_main_func)

// For async_main returning void (exits with 0)
ELIO_ASYNC_MAIN_VOID(async_main_func)
```

**Example:**
```cpp
coro::task<int> async_main() {
    co_await do_work();
    co_return 0;
}

ELIO_ASYNC_MAIN(async_main)
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
        io_context& ctx
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
/* awaitable */ tcp_connect(const ipv4_address& addr, io_context& ctx);
```

---

## HTTP (`elio::http`)

### `client`

HTTP client with connection pooling.

```cpp
class client {
public:
    explicit client(io_context& ctx);
    client(io_context& ctx, const client_config& config);
    
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
/* awaitable */ get(io_context& ctx, const std::string& url);
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
    explicit h2_client(io_context& ctx);
    h2_client(io_context& ctx, const h2_client_config& config);
    
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
/* awaitable */ h2_get(io_context& ctx, const std::string& url);

// Convenience function for one-off HTTP/2 POST
/* awaitable */ h2_post(io_context& ctx, const std::string& url,
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
    tls_stream(tcp_stream tcp, tls_context& ctx, io_context& io_ctx);
    
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
// Sleep for duration (awaitable)
/* awaitable */ sleep(io_context& ctx, std::chrono::nanoseconds duration);

// Sleep until time point (awaitable)
template<typename Clock, typename Duration>
/* awaitable */ sleep_until(io_context& ctx, 
                            std::chrono::time_point<Clock, Duration> tp);
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
