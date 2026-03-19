# Networking

Elio provides async networking support for TCP, Unix Domain Sockets (UDS), HTTP/1.1, HTTP/2, and TLS/HTTPS.

## Design Rationale

### Per-Worker I/O Contexts

Each worker thread in the Elio scheduler owns its own io_uring or epoll backend instance. When a coroutine performs an I/O operation, the request is submitted to the worker's local backend. This means I/O operations never require locking or cross-thread synchronization. The result is lower latency and higher throughput, especially under heavy concurrent I/O loads. If a task migrates to a different worker (via work-stealing), subsequent I/O operations simply use the new worker's local context.

### Separate TCP and UDS Modules

TCP and Unix Domain Sockets live in separate modules (`net/tcp.hpp` and `net/uds.hpp`) because they serve different socket domains with distinct setup and configuration needs. TCP deals with IPv4/IPv6 addressing, DNS resolution, Nagle's algorithm, and keep-alive. UDS, on the other hand, supports filesystem paths, Linux-specific abstract sockets (which have no filesystem entry and are automatically cleaned up), and credential passing via `SO_PASSCRED`. Keeping them separate avoids leaking domain-specific concerns into a shared abstraction.

### The `tcp_options` Struct

Rather than requiring separate `setsockopt()` calls after creating a listener or connection, Elio consolidates socket configuration into a single `tcp_options` struct. This struct is passed at bind or connect time and supports `reuse_addr`, `reuse_port`, `no_delay`, `keep_alive`, `recv_buffer`, `send_buffer`, `backlog`, and `ipv6_only`. This declarative approach reduces boilerplate and makes it harder to forget important options:

```cpp
tcp_options opts;
opts.reuse_port = true;     // SO_REUSEPORT for load balancing across processes
opts.no_delay = true;       // TCP_NODELAY (enabled by default)
opts.keep_alive = true;     // SO_KEEPALIVE
opts.recv_buffer = 65536;   // SO_RCVBUF
opts.backlog = 512;         // Listen backlog

auto listener = tcp_listener::bind(ipv4_address(8080), opts);
```

## TCP

### TCP Server

```cpp
#include <elio/elio.hpp>

using namespace elio;
using namespace elio::net;

coro::task<void> handle_client(tcp_stream stream) {
    char buffer[1024];
    
    while (true) {
        auto result = co_await stream.read(buffer, sizeof(buffer));
        if (result.result <= 0) break;
        
        co_await stream.write(buffer, result.result);
    }
    co_return;
}

coro::task<void> server(uint16_t port) {
    auto* sched = runtime::scheduler::current();

    auto listener = tcp_listener::bind(ipv4_address(port));
    if (!listener) {
        ELIO_LOG_ERROR("Bind failed: {}", strerror(errno));
        co_return;
    }

    while (true) {
        auto stream = co_await listener->accept();
        if (!stream) continue;

        auto handler = handle_client(std::move(*stream));
        sched->spawn(handler.release());
    }
}
```

### TCP Client

```cpp
coro::task<void> client(const std::string& host, uint16_t port) {
    // Connect to server (hostname is resolved automatically)
    auto stream = co_await tcp_connect(ipv4_address(host, port));
    if (!stream) {
        ELIO_LOG_ERROR("Connect failed: {}", strerror(errno));
        co_return;
    }
    
    // Send data
    const char* msg = "Hello, server!";
    co_await stream->write(msg, strlen(msg));
    
    // Receive response
    char buffer[1024];
    auto result = co_await stream->read(buffer, sizeof(buffer));
    if (result.result > 0) {
        ELIO_LOG_INFO("Received: {}", std::string_view(buffer, result.result));
    }
    co_return;
}
```

### Address Types

Elio provides three address types for TCP networking: `ipv4_address`, `ipv6_address`, and `socket_address` (a variant wrapper that holds either).

```cpp
// IPv4 address with port
ipv4_address addr1(8080);                    // 0.0.0.0:8080
ipv4_address addr2("192.168.1.1", 8080);     // 192.168.1.1:8080
ipv4_address addr3("example.com", 80);       // DNS resolved

// IPv6 address with port
ipv6_address addr4(8080);                    // [::]:8080
ipv6_address addr5("::1", 8080);             // [::1]:8080
ipv6_address addr6("fe80::1%eth0", 8080);   // Link-local with scope ID
ipv6_address addr7("example.com", 443);      // DNS resolved (AAAA)

// Generic socket_address (variant of ipv4_address | ipv6_address)
socket_address sa1(ipv4_address(8080));              // From IPv4
socket_address sa2(ipv6_address("::1", 8080));       // From IPv6
socket_address sa3("example.com", 443);              // Auto-detects v4/v6

// Inspect address type
if (sa3.is_v6()) {
    const auto& v6 = sa3.as_v6();
    ELIO_LOG_INFO("IPv6: {}", v6.to_string());
}

// Get peer address from a connected stream
auto peer = stream.peer_address();
if (peer) {
    ELIO_LOG_INFO("Connected to: {}", peer->to_string());
}
```

Both `tcp_listener::bind()` and `tcp_connect()` accept any of the three address types.

### Scatter-Gather I/O (writev)

For efficient writing of multiple buffers without copying, use `writev()`. Both `tcp_stream` and `uds_stream` support this method.

```cpp
coro::task<void> send_message(tcp_stream& stream) {
    // Prepare header and payload separately
    std::string header = "HEADER:";
    std::string body = "Hello, World!";

    // Write both in a single syscall using scatter-gather I/O
    struct iovec iov[2] = {
        {(void*)header.data(), header.size()},
        {(void*)body.data(), body.size()}
    };

    auto result = co_await stream.writev(iov, 2);
    if (result.result > 0) {
        ELIO_LOG_INFO("Sent {} bytes", result.result);
    }
}
```

**Benefits of writev:**
- Reduces syscall overhead by combining multiple writes into one
- Avoids buffer copying when you have data in separate locations
- More atomic writes - better behavior under high concurrency
- Used internally by the RPC framework for efficient frame writing

## Unix Domain Sockets (UDS)

Unix Domain Sockets provide high-performance local inter-process communication. Elio supports both filesystem sockets and abstract sockets (Linux-specific).

### UDS Server

```cpp
#include <elio/elio.hpp>

using namespace elio;
using namespace elio::net;

coro::task<void> handle_client(uds_stream stream) {
    char buffer[1024];
    
    while (true) {
        auto result = co_await stream.read(buffer, sizeof(buffer));
        if (result.result <= 0) break;
        
        co_await stream.write(buffer, result.result);
    }
    co_return;
}

coro::task<void> server(const unix_address& addr) {
    auto* sched = runtime::scheduler::current();

    uds_options opts;
    opts.unlink_on_bind = true;  // Remove existing socket file

    auto listener = uds_listener::bind(addr, opts);
    if (!listener) {
        ELIO_LOG_ERROR("Bind failed: {}", strerror(errno));
        co_return;
    }

    while (true) {
        auto stream = co_await listener->accept();
        if (!stream) continue;

        auto handler = handle_client(std::move(*stream));
        sched->spawn(handler.release());
    }
}
```

### UDS Client

```cpp
coro::task<void> client(const unix_address& addr) {
    // Connect to server
    auto stream = co_await uds_connect(addr);
    if (!stream) {
        ELIO_LOG_ERROR("Connect failed: {}", strerror(errno));
        co_return;
    }
    
    // Send data
    const char* msg = "Hello via UDS!";
    co_await stream->write(msg, strlen(msg));
    
    // Receive response
    char buffer[1024];
    auto result = co_await stream->read(buffer, sizeof(buffer));
    if (result.result > 0) {
        ELIO_LOG_INFO("Received: {}", std::string_view(buffer, result.result));
    }
    co_return;
}
```

### UDS Address Types

```cpp
// Filesystem socket - creates a file at the specified path
unix_address fs_addr("/tmp/my_server.sock");

// Abstract socket (Linux-specific) - no filesystem entry
auto abstract_addr = unix_address::abstract("my_service");

// Check socket type
if (addr.is_abstract()) {
    ELIO_LOG_INFO("Using abstract socket");
}

// Get address string representation
// Filesystem: "/tmp/my_server.sock"
// Abstract: "@my_service"
ELIO_LOG_INFO("Address: {}", addr.to_string());
```

Abstract sockets are useful when you want IPC without creating files on the filesystem. They are automatically cleaned up when all references are closed:

```cpp
// Abstract socket (Linux-specific, no filesystem path)
auto addr = net::unix_address::abstract("my_service");
auto listener = net::uds_listener::bind(addr);
```

### UDS vs TCP

| Feature | UDS | TCP |
|---------|-----|-----|
| Scope | Local machine only | Network-wide |
| Performance | Faster (no network stack) | Slower (full TCP/IP stack) |
| Address | Filesystem path or abstract name | IP address + port |
| Security | Filesystem permissions | Firewall rules |
| Credential passing | Supported (SO_PASSCRED) | Not available |

### When to Use UDS

- Local inter-process communication (IPC)
- Microservices on the same host
- Database connections (e.g., PostgreSQL, MySQL)
- Container-to-container communication within a pod
- When you need higher throughput than TCP for local connections

## HTTP

HTTP support requires linking with `elio_http` and OpenSSL.

### HTTP Client

```cpp
#include <elio/http/http.hpp>
#include <elio/tls/tls.hpp>

using namespace elio::http;

coro::task<void> fetch_url() {
    // Simple one-off request
    auto result = co_await http::get("https://httpbin.org/get");

    if (result) {
        ELIO_LOG_INFO("Status: {}", result->status_code());
        ELIO_LOG_INFO("Body: {}", result->body());
    }
    co_return;
}
```

### HTTP Client with Configuration

```cpp
coro::task<void> advanced_client() {
    client_config config;
    config.user_agent = "MyApp/1.0";
    config.follow_redirects = true;
    config.max_redirects = 5;

    client c(config);
    
    // GET request
    auto resp = co_await c.get("https://api.example.com/data");
    
    // POST JSON
    std::string json = R"({"key": "value"})";
    auto post_resp = co_await c.post(
        "https://api.example.com/data",
        json,
        mime::application_json
    );
    
    // Custom request
    request req(method::PUT, "/resource");
    req.set_host("api.example.com");
    req.set_header("Authorization", "Bearer token123");
    req.set_body(R"({"update": true})");
    
    url target;
    target.scheme = "https";
    target.host = "api.example.com";
    target.path = "/resource";
    
    auto custom_resp = co_await c.send(req, target);
    co_return;
}
```

### HTTP Server

```cpp
#include <elio/http/http.hpp>

using namespace elio::http;

coro::task<void> handle_request(request& req, response& resp) {
    if (req.method() == method::GET && req.path() == "/") {
        resp.set_status(status::ok);
        resp.set_header("Content-Type", "text/html");
        resp.set_body("<h1>Hello from Elio!</h1>");
    } else if (req.path() == "/api/data") {
        resp.set_status(status::ok);
        resp.set_header("Content-Type", "application/json");
        resp.set_body(R"({"message": "Hello, World!"})");
    } else {
        resp.set_status(status::not_found);
        resp.set_body("Not Found");
    }
    co_return;
}

coro::task<void> run_server(uint16_t port) {
    router r;
    r.get("/", [](context& ctx) {
        return response::ok("<h1>Hello from Elio!</h1>", "text/html");
    });
    r.get("/api/data", [](context& ctx) {
        return response::ok(R"({"message": "Hello, World!"})", "application/json");
    });

    server srv(r);
    co_await srv.listen(net::ipv4_address(port));
}
```

## TLS/HTTPS

TLS support is built on OpenSSL.

### HTTP/2

HTTP/2 support requires linking with `elio_http2`, OpenSSL, and nghttp2 (fetched automatically).

> **Note**: HTTP/2 requires HTTPS (TLS with ALPN h2 negotiation). For plaintext HTTP, use the HTTP/1.1 client.

### HTTP/2 Client

```cpp
#include <elio/http/http2.hpp>

using namespace elio::http;

coro::task<void> fetch_url_h2() {
    // Create HTTP/2 client
    h2_client client;

    // Simple GET request (must use HTTPS)
    auto result = co_await client.get("https://nghttp2.org/");

    if (result) {
        ELIO_LOG_INFO("Status: {}", static_cast<int>(result->get_status()));
        ELIO_LOG_INFO("Body: {}", result->body());
    }
    co_return;
}
```

### HTTP/2 Client with Configuration

```cpp
coro::task<void> advanced_h2_client() {
    h2_client_config config;
    config.user_agent = "MyApp/1.0";
    config.max_concurrent_streams = 100;

    h2_client client(config);
    
    // GET request
    auto resp = co_await client.get("https://api.example.com/data");
    
    // POST JSON
    std::string json = R"({"key": "value"})";
    auto post_resp = co_await client.post(
        "https://api.example.com/data",
        json,
        mime::application_json
    );
    
    // Multiple requests reuse the same HTTP/2 connection (multiplexing)
    for (int i = 0; i < 10; ++i) {
        auto r = co_await client.get("https://api.example.com/items/" + std::to_string(i));
    }
    
    co_return;
}
```

### HTTP/2 vs HTTP/1.1

| Feature | HTTP/1.1 (`client`) | HTTP/2 (`h2_client`) |
|---------|---------------------|----------------------|
| Protocol | HTTP/1.1 | HTTP/2 (h2) |
| TLS Required | No (supports both) | Yes (HTTPS only) |
| Multiplexing | No (one request per connection) | Yes (multiple streams) |
| Header Compression | No | Yes (HPACK) |
| CMake Target | `elio_http` | `elio_http2` |

### When to Use HTTP/2

- Use `h2_client` when connecting to modern APIs that support HTTP/2
- Use `client` (HTTP/1.1) for plaintext HTTP or legacy servers
- HTTP/2 provides better performance for multiple concurrent requests to the same host



### TLS Client

```cpp
#include <elio/tls/tls.hpp>

using namespace elio::tls;

coro::task<void> secure_connection() {
    // Create TLS context
    tls_context tls_ctx(tls_method::client);
    tls_ctx.use_default_verify_paths();
    tls_ctx.set_verify_mode(true);

    // Connect TCP
    auto tcp = co_await tcp_connect(ipv4_address("example.com", 443));
    if (!tcp) co_return;

    // Wrap with TLS
    tls_stream stream(std::move(*tcp), tls_ctx);
    stream.set_hostname("example.com");  // SNI
    
    // Perform handshake
    auto hs = co_await stream.handshake();
    if (!hs) {
        ELIO_LOG_ERROR("TLS handshake failed");
        co_return;
    }
    
    // Send HTTPS request
    const char* req = "GET / HTTP/1.1\r\nHost: example.com\r\n\r\n";
    co_await stream.write(req, strlen(req));
    
    // Read response
    char buffer[4096];
    auto result = co_await stream.read(buffer, sizeof(buffer));
    if (result.result > 0) {
        ELIO_LOG_INFO("Response: {}", std::string_view(buffer, result.result));
    }
    co_return;
}
```

### TLS Server

```cpp
coro::task<void> tls_server(uint16_t port) {
    // Create TLS context with certificate
    tls_context tls_ctx(tls_method::server);
    tls_ctx.use_certificate_file("server.crt");
    tls_ctx.use_private_key_file("server.key");

    auto listener = tcp_listener::bind(ipv4_address(port));
    if (!listener) co_return;

    while (true) {
        auto tcp = co_await listener->accept();
        if (!tcp) continue;

        // Wrap accepted connection with TLS
        tls_stream stream(std::move(*tcp), tls_ctx);

        auto hs = co_await stream.handshake();
        if (hs) {
            // Handle secure connection
        }
    }
}
```

## Connection Pooling

The HTTP client automatically manages connection pooling for keep-alive connections:

```cpp
coro::task<void> pooled_requests() {
    client c;
    
    // These requests reuse connections when possible
    for (int i = 0; i < 10; ++i) {
        auto resp = co_await c.get("https://api.example.com/data");
        // Connection is kept alive and reused
    }
    co_return;
}
```

## WebSocket and Server-Sent Events

For real-time communication, see the dedicated [[WebSocket-SSE]] documentation:

- **WebSocket**: Full-duplex communication for chat, games, and real-time apps
- **Server-Sent Events (SSE)**: Server-to-client event streaming for notifications and feeds

```cpp
#include <elio/http/websocket.hpp>
#include <elio/http/sse.hpp>
```

## Next Steps

- See [[WebSocket-SSE]] for WebSocket and SSE documentation
- See [[Examples]] for complete networking examples
- Check [[API Reference]] for detailed documentation
