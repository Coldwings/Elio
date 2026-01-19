# Networking

Elio provides async networking support for TCP, HTTP/1.1, and TLS/HTTPS.

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

coro::task<void> server(uint16_t port, runtime::scheduler& sched) {
    auto& ctx = io::default_io_context();
    
    auto listener = tcp_listener::bind(ipv4_address(port), ctx);
    if (!listener) {
        ELIO_LOG_ERROR("Bind failed: {}", strerror(errno));
        co_return;
    }
    
    while (true) {
        auto stream = co_await listener->accept();
        if (!stream) continue;
        
        auto handler = handle_client(std::move(*stream));
        sched.spawn(handler.release());
    }
}
```

### TCP Client

```cpp
coro::task<void> client(const std::string& host, uint16_t port) {
    auto& ctx = io::default_io_context();
    
    // Connect to server (hostname is resolved automatically)
    auto stream = co_await tcp_connect(ipv4_address(host, port), ctx);
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

```cpp
// IPv4 address with port
ipv4_address addr1(8080);                    // 0.0.0.0:8080
ipv4_address addr2("192.168.1.1", 8080);     // 192.168.1.1:8080
ipv4_address addr3("example.com", 80);       // DNS resolved

// Get address info
auto peer = stream.peer_address();
if (peer) {
    ELIO_LOG_INFO("Connected to: {}", peer->to_string());
}
```

## HTTP

HTTP support requires linking with `elio_http` and OpenSSL.

### HTTP Client

```cpp
#include <elio/http/http.hpp>
#include <elio/tls/tls.hpp>

using namespace elio::http;

coro::task<void> fetch_url(io::io_context& ctx) {
    // Simple one-off request
    auto result = co_await http::get(ctx, "https://httpbin.org/get");
    
    if (result) {
        ELIO_LOG_INFO("Status: {}", result->status_code());
        ELIO_LOG_INFO("Body: {}", result->body());
    }
    co_return;
}
```

### HTTP Client with Configuration

```cpp
coro::task<void> advanced_client(io::io_context& ctx) {
    client_config config;
    config.user_agent = "MyApp/1.0";
    config.follow_redirects = true;
    config.max_redirects = 5;
    
    client c(ctx, config);
    
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
    auto& ctx = io::default_io_context();
    
    server_config config;
    config.port = port;
    config.handler = handle_request;
    
    server srv(ctx, config);
    co_await srv.run();
}
```

## TLS/HTTPS

TLS support is built on OpenSSL.

### TLS Client

```cpp
#include <elio/tls/tls.hpp>

using namespace elio::tls;

coro::task<void> secure_connection(io::io_context& ctx) {
    // Create TLS context
    tls_context tls_ctx(tls_method::client);
    tls_ctx.use_default_verify_paths();
    tls_ctx.set_verify_mode(true);
    
    // Connect TCP
    auto tcp = co_await tcp_connect(ipv4_address("example.com", 443), ctx);
    if (!tcp) co_return;
    
    // Wrap with TLS
    tls_stream stream(std::move(*tcp), tls_ctx, ctx);
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
    auto& ctx = io::default_io_context();
    
    // Create TLS context with certificate
    tls_context tls_ctx(tls_method::server);
    tls_ctx.use_certificate_file("server.crt");
    tls_ctx.use_private_key_file("server.key");
    
    auto listener = tcp_listener::bind(ipv4_address(port), ctx);
    if (!listener) co_return;
    
    while (true) {
        auto tcp = co_await listener->accept();
        if (!tcp) continue;
        
        // Wrap accepted connection with TLS
        tls_stream stream(std::move(*tcp), tls_ctx, ctx);
        
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
coro::task<void> pooled_requests(io::io_context& ctx) {
    client c(ctx);
    
    // These requests reuse connections when possible
    for (int i = 0; i < 10; ++i) {
        auto resp = co_await c.get("https://api.example.com/data");
        // Connection is kept alive and reused
    }
    co_return;
}
```

## Next Steps

- See [[Examples]] for complete networking examples
- Check [[API Reference]] for detailed documentation
