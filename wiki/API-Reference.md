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
    // Bind to address
    static std::expected<tcp_listener, int> bind(
        const ipv4_address& addr,
        io_context& ctx
    );
    
    // Accept a connection (awaitable)
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

// Connect to address (awaitable)
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
