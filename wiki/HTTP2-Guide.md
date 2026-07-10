# HTTP/2 Guide

Elio provides full HTTP/2 client support via the nghttp2 library. This guide covers HTTP/2 usage, configuration, and best practices.

## Overview

HTTP/2 offers several advantages over HTTP/1.1:
- **Multiplexing**: Multiple requests/responses over a single connection
- **Header compression**: HPACK compression reduces overhead
- **Binary framing**: More efficient parsing
- **Stream prioritization**: Clients can hint at request importance

## Why nghttp2

Elio uses the nghttp2 library for HTTP/2 support. nghttp2 is the reference implementation of the HTTP/2 protocol (RFC 7540) and handles the substantial complexity of HTTP/2 streams, flow control, and HPACK header compression. It is well-tested and widely deployed -- used by curl, Apache httpd, and many other projects. Elio wraps nghttp2 to expose a simple coroutine-based interface, letting the library manage the protocol state machine while Elio handles I/O scheduling and connection lifecycle.

## Requirements

HTTP/2 support requires:
- OpenSSL with ALPN support
- nghttp2 library (fetched automatically by CMake)
- CMake option: `-DELIO_ENABLE_HTTP2=ON`

Note: HTTP/2 requires HTTPS (h2 over TLS). For plaintext HTTP, use the HTTP/1.1 client.

Link the HTTP/2 feature target so nghttp2 headers and libraries propagate to
your application:

```cmake
# FetchContent or add_subdirectory
set(ELIO_ENABLE_TLS ON CACHE BOOL "" FORCE)
set(ELIO_ENABLE_HTTP ON CACHE BOOL "" FORCE)
set(ELIO_ENABLE_HTTP2 ON CACHE BOOL "" FORCE)

# The options must be set before either of these calls:
# FetchContent_MakeAvailable(elio)
# add_subdirectory(path/to/elio)
target_link_libraries(your_target PRIVATE elio_http2)

# Installed package built with TLS, HTTP, and HTTP/2 enabled
# find_package(Elio REQUIRED CONFIG)
target_link_libraries(your_target PRIVATE Elio::elio_http2)
```

## Basic Usage

### Simple GET Request

```cpp
#include <elio/elio.hpp>
#include <elio/http/http2.hpp>

using namespace elio;
using namespace elio::http;

coro::task<void> fetch_example() {
    h2_client client;

    // Simple GET request
    auto resp = co_await client.get("https://example.com/");
    if (resp) {
        std::cout << "Status: " << resp->status_code() << std::endl;
        std::cout << "Body: " << resp->body() << std::endl;
    }
}
```

### POST Request with JSON

```cpp
coro::task<void> post_example() {
    h2_client client;

    auto resp = co_await client.post(
        "https://api.example.com/users",
        R"({"name": "John", "email": "john@example.com"})",
        mime::application_json
    );

    if (resp) {
        std::cout << "Created: " << resp->body() << std::endl;
    }
}
```

## Client Configuration

### h2_client_config

```cpp
struct h2_client_config {
    std::chrono::seconds connect_timeout{10};
    std::chrono::seconds read_timeout{30};
    size_t max_concurrent_streams = 100;
    uint32_t initial_window_size = 65535;
    std::string user_agent = "elio-http2/1.0";
    bool enable_push = false;  // Server push (rarely needed)
    net::resolve_options resolve_options = net::default_cached_resolve_options();
    bool rotate_resolved_addresses = true;
};

// Usage
h2_client_config config;
config.max_concurrent_streams = 50;

h2_client client(config);
```

### TLS Configuration

```cpp
h2_client client;

// Access the underlying TLS context
auto& tls_ctx = client.tls_context();

// Use custom CA certificate
tls_ctx.load_verify_locations("/path/to/ca-bundle.crt");

// Use client certificate (for mutual TLS)
tls_ctx.load_certificate("/path/to/client.crt");
tls_ctx.load_private_key("/path/to/client.key");
```

## Concurrent Requests

HTTP/2 excels at handling multiple concurrent requests:

```cpp
coro::task<void> parallel_requests() {
    h2_client client;

    // Spawn multiple requests concurrently
    auto h1 = elio::spawn([&]() { return client.get("https://api.example.com/users/1"); });
    auto h2 = elio::spawn([&]() { return client.get("https://api.example.com/users/2"); });
    auto h3 = elio::spawn([&]() { return client.get("https://api.example.com/users/3"); });

    // Wait for all responses
    auto r1 = co_await h1;
    auto r2 = co_await h2;
    auto r3 = co_await h3;

    // All requests used the same TCP connection!
}
```

## Response Handling

```cpp
coro::task<void> handle_response() {
    h2_client client;

    auto resp = co_await client.get("https://example.com/api/data");

    if (!resp) {
        std::cerr << "Request failed: " << strerror(errno) << std::endl;
        co_return;
    }

    // Check status
    if (!resp->is_success()) {
        std::cerr << "HTTP error: " << resp->status_code() << std::endl;
        co_return;
    }

    // Access headers
    auto content_type = resp->header("Content-Type");
    auto cache_control = resp->header("Cache-Control");

    // Get body (returns string_view)
    std::string_view body = resp->body();

    // If you need an owned copy:
    std::string body_owned(resp->body());
}
```

## Error Handling

```cpp
coro::task<void> error_handling() {
    h2_client client;

    auto resp = co_await client.get("https://example.com/");

    if (!resp) {
        switch (errno) {
            case ECONNREFUSED:
                std::cerr << "Connection refused" << std::endl;
                break;
            case ETIMEDOUT:
                std::cerr << "Connection timeout" << std::endl;
                break;
            case ECONNRESET:
                std::cerr << "Connection reset" << std::endl;
                break;
            default:
                std::cerr << "Error: " << strerror(errno) << std::endl;
        }
        co_return;
    }

    // Handle HTTP-level errors
    if (resp->status_code() >= 400) {
        std::cerr << "HTTP " << resp->status_code() << ": "
                  << resp->body() << std::endl;
    }
}
```

## Cancellation

```cpp
coro::task<void> cancellable_request() {
    h2_client client;

    // Note: h2_client does not currently support per-request cancellation
    // via cancel_token. Use task-level cancellation instead.
    auto resp = co_await client.get("https://slow-api.example.com/");

    if (!resp) {
        std::cout << "Request failed" << std::endl;
    }
}
```

## Connection Management

The h2_client maintains a connection pool internally:

```cpp
coro::task<void> connection_lifecycle() {
    h2_client client;

    // First request establishes connection
    co_await client.get("https://api.example.com/ping");

    // Subsequent requests reuse the same connection
    for (int i = 0; i < 100; i++) {
        co_await client.get("https://api.example.com/data/" +
                           std::to_string(i));
    }

    // Connection is automatically closed when client is destroyed
}
```

## HTTP/2 vs HTTP/1.1

| Feature | HTTP/2 | HTTP/1.1 |
|---------|--------|----------|
| Multiplexing | Yes | No (pipelining limited) |
| Header compression | HPACK | None |
| Binary protocol | Yes | Text-based |
| Server push | Supported | No |
| TLS required | Yes (in practice) | No |
| Connection reuse | Single connection | Connection pool |

### When to Use HTTP/2

**Use HTTP/2 when:**
- Making many concurrent requests to the same host
- Bandwidth is limited (header compression helps)
- Low latency is critical
- Server supports HTTP/2

**Use HTTP/1.1 when:**
- Server doesn't support HTTP/2
- Making single requests
- HTTP (non-TLS) is required
- Compatibility with older infrastructure

## Debugging

Enable debug logging to see HTTP/2 frame details:

```cpp
// Set log level before creating client
elio::log::logger::instance().set_level(elio::log::level::debug);

h2_client client;
auto resp = co_await client.get("https://example.com/");

// Output shows frame exchanges:
// [DEBUG] Sending SETTINGS frame
// [DEBUG] Received SETTINGS frame
// [DEBUG] Sending HEADERS frame (stream 1)
// [DEBUG] Received HEADERS frame (stream 1)
// [DEBUG] Received DATA frame (stream 1, 1234 bytes)
```

## Best Practices

1. **Reuse clients**: Create one h2_client per host and reuse it
2. **Use concurrency**: Take advantage of multiplexing with parallel requests
3. **Handle errors**: Always check response validity
4. **Set timeouts**: Configure appropriate timeouts for your use case
5. **Verify certificates**: Only disable certificate verification for testing

## See Also

- [HTTP/1.1 Client](Networking.md#http-client)
- [TLS Configuration](TLS-Configuration.md)
- [WebSocket Guide](WebSocket-SSE.md)
