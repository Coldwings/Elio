# TLS Configuration Guide

Elio provides TLS/SSL support via OpenSSL. This guide covers TLS configuration for secure connections.

## Overview

TLS (Transport Layer Security) provides:
- **Encryption**: Data is encrypted in transit
- **Authentication**: Verify server (and optionally client) identity
- **Integrity**: Detect tampering with data

## Why OpenSSL

Elio uses OpenSSL as its TLS backend. OpenSSL is mature, widely available on Linux distributions, and supports modern TLS 1.3 with ALPN for HTTP/2 negotiation. It provides industry-standard certificate management and is the most broadly deployed TLS library, ensuring compatibility with existing infrastructure and tooling.

## Requirements

TLS support requires:
- OpenSSL library (1.1.1+ recommended)
- CMake option: `-DELIO_ENABLE_TLS=ON`

## TLS Context

The `tls_context` manages TLS configuration:

### Client Mode

```cpp
#include <elio/tls/tls_context.hpp>
#include <elio/tls/tls_stream.hpp>

using namespace elio::tls;

// Create client TLS context
tls_context ctx(tls_mode::client);

// Use system CA certificates for verification
ctx.use_default_verify_paths();

// Enable peer verification (recommended)
ctx.set_verify_mode(verify_mode::peer);
```

### Server Mode

```cpp
// Create server TLS context
tls_context ctx(tls_mode::server);

// Load server certificate and private key
ctx.load_certificate("/path/to/server.crt");
ctx.load_private_key("/path/to/server.key");

// Optionally require client certificates
ctx.set_verify_mode(verify_mode::fail_if_no_cert);
ctx.load_verify_locations("/path/to/ca.crt");
```

## Certificate Verification

### Verification Modes

```cpp
enum class verify_mode {
    none,               // No verification (insecure!)
    peer,               // Verify peer certificate
    fail_if_no_cert     // Fail if peer has no certificate (server mode)
};

ctx.set_verify_mode(verify_mode::fail_if_no_cert);
```

### Custom Verification

```cpp
// Load specific CA certificate file
ctx.load_verify_locations("/path/to/ca-bundle.crt");

// Load directory of CA certificates
ctx.load_verify_locations("", "/etc/ssl/certs/");

// Load both file and directory
ctx.load_verify_locations("/path/to/ca-bundle.crt", "/etc/ssl/certs/");

// Use system default CA store
ctx.use_default_verify_paths();
```

## Client Certificates (Mutual TLS)

For mutual TLS authentication:

```cpp
tls_context ctx(tls_mode::client);

// Load client certificate
ctx.load_certificate("/path/to/client.crt");

// Load client private key (with optional password for encrypted keys)
ctx.load_private_key("/path/to/client.key", "my_secret_password");
```

## ALPN (Application-Layer Protocol Negotiation)

ALPN is required for HTTP/2:

```cpp
// For HTTP/2 client (comma-separated protocol list)
ctx.set_alpn_protocols("h2,http/1.1");

// After connection, check negotiated protocol
tls_stream stream = /* ... */;
std::string_view proto = stream.alpn_protocol();
if (proto == "h2") {
    // Use HTTP/2
} else {
    // Fall back to HTTP/1.1
}
```

## Connecting with TLS

### Using tls_connect

```cpp
coro::task<void> connect_example() {
    tls_context ctx(tls_mode::client);
    ctx.use_default_verify_paths();
    ctx.set_verify_mode(verify_mode::peer);

    // Connect with TLS
    auto stream = co_await tls_connect(ctx, "example.com", 443);
    if (!stream) {
        std::cerr << "TLS connection failed: " << strerror(errno) << std::endl;
        co_return;
    }

    // Use the stream
    co_await stream->write_exactly("GET / HTTP/1.1\r\nHost: example.com\r\n\r\n");

    char buffer[4096];
    auto result = co_await stream->read(buffer, sizeof(buffer));
    if (result.success()) {
        std::cout.write(buffer, result.bytes_transferred());
    }

    co_await stream->shutdown();
}
```

### Wrapping an Existing Async Byte Stream

`tls_stream` is the TCP convenience facade. The TLS engine itself is available
as `basic_tls_stream<Lower>` for any move-owned lower stream that provides
cancellable asynchronous `read(void*, size_t, cancel_token)` and
`write(const void*, size_t, cancel_token)` operations:

```cpp
// The lower stream can be a tunnel, buffered channel, or another TLS stream.
auto lower = co_await open_lower_channel();
tls_context origin_tls(tls_mode::client);
origin_tls.use_default_verify_paths();
origin_tls.set_verify_mode(verify_mode::peer);

basic_tls_stream<decltype(lower)> stream(std::move(lower), origin_tls);
stream.set_hostname("origin.example");

if (!co_await stream.handshake(token)) {
    // errno describes the local TLS/lower-stream failure.
    co_return;
}
```

Each TLS layer has its own context, SNI, verification mode, ALPN and session
state. For example, HTTPS-through-HTTPS proxying uses an outer TLS stream for
the proxy connection, then an inner `basic_tls_stream<tls_stream>` for the
origin after the CONNECT tunnel is established. The inner layer's ciphertext is
written through the outer stream; `fd()` diagnostics never authorize bypassing
the lower protocol.

### TLS Listener (Server)

```cpp
coro::task<void> server_example() {
    tls_context ctx(tls_mode::server);
    ctx.load_certificate("/path/to/server.crt");
    ctx.load_private_key("/path/to/server.key");

    auto listener = tls_listener::bind(
        net::ipv4_address("0.0.0.0", 8443),
        ctx
    );

    if (!listener) {
        std::cerr << "Failed to bind" << std::endl;
        co_return;
    }

    while (true) {
        auto client = co_await listener->accept();
        if (client) {
            elio::go([client = std::move(*client)]() mutable {
                return handle_client(std::move(client));
            });
        }
    }
}
```

## Protocol Configuration

### TLS Version

The TLS version is configured via the `tls_version` enum passed to the `tls_context` constructor:

```cpp
// Require TLS 1.2 or higher (default)
tls_context ctx(tls_mode::client, tls_version::tls_1_2_or_higher);

// Require TLS 1.3 only
tls_context ctx(tls_mode::client, tls_version::tls_1_3_only);

// Exactly TLS 1.2
tls_context ctx(tls_mode::client, tls_version::tls_1_2);
```

### Cipher Suites

```cpp
// Set preferred cipher suites (TLS 1.2)
ctx.set_ciphers("ECDHE+AESGCM:DHE+AESGCM:ECDHE+CHACHA20");

// Set cipher suites for TLS 1.3
ctx.set_ciphersuites("TLS_AES_256_GCM_SHA384:TLS_CHACHA20_POLY1305_SHA256");
```

## Error Handling

```cpp
coro::task<void> handle_tls_errors() {
    tls_context ctx(tls_mode::client);
    ctx.use_default_verify_paths();
    ctx.set_verify_mode(verify_mode::peer);

    auto stream = co_await tls_connect(ctx, "expired.badssl.com", 443);

    if (!stream) {
        // Check errno for specific error
        // Common TLS-related errors:
        // - Certificate verification failed
        // - Connection refused
        // - Handshake timeout
        std::cerr << "TLS error: " << strerror(errno) << std::endl;
        co_return;
    }

    // Check certificate verification result
    long verify_result = stream->verify_result();
    if (verify_result != X509_V_OK) {
        std::cerr << "Certificate error: "
                  << X509_verify_cert_error_string(verify_result)
                  << std::endl;
    }
}
```

## TLS Stream Properties

```cpp
coro::task<void> inspect_connection() {
    auto stream = co_await tls_connect(ctx, "example.com", 443);
    if (!stream) co_return;

    // TLS version
    std::cout << "TLS Version: " << stream->version() << std::endl;

    // Cipher suite
    std::cout << "Cipher: " << stream->cipher() << std::endl;

    // ALPN protocol
    std::cout << "ALPN: " << stream->alpn_protocol() << std::endl;

    // Peer certificate
    X509* cert = stream->peer_certificate();
    if (cert) {
        // Extract certificate info
        X509_NAME* subject = X509_get_subject_name(cert);
        char buf[256];
        X509_NAME_oneline(subject, buf, sizeof(buf));
        std::cout << "Subject: " << buf << std::endl;
        X509_free(cert);
    }
}
```

## Integration with HTTP Clients

### HTTP Client TLS Configuration

```cpp
http::transport_config transport_config;
transport_config.configure_tls = [](http::transport_tls_config& tls_ctx) {
    tls_ctx.load_verify_locations("/path/to/custom-ca.crt");
};

auto transport = std::make_shared<http::transport>(transport_config);
http::client client(transport);

auto resp = co_await client.get("https://example.com/");
```

HTTP/1 transports seal their active TLS context at construction so shared
clients cannot mutate a published security identity. Use
`transport_config::configure_tls` before constructing the transport. The
callback receives `http::transport_tls_config`, a construction-only builder that
forwards common TLS policy mutators and exposes only copied diagnostics such as
mode and verification flags. It does not expose `SSL_CTX*`, `X509_STORE*`, or
the published TLS context. This is a breaking HTTP/1 client migration from older
`client.tls_context().load_*` or `set_*` customization calls; use
`client.tls_diagnostics()` only for post-publication value inspection.
WebSocket, SSE, and HTTP/2 clients continue to expose mutable per-client TLS
contexts.

### HTTP/2 Client TLS Configuration

```cpp
h2_client client;

// Configure TLS for HTTP/2
auto& tls_ctx = client.tls_context();

// HTTP/2 requires ALPN
// (automatically configured by h2_client)

// Get negotiated protocol after connection
// h2_client uses HTTP/2 if server supports it
```

## Best Practices

1. **Always verify certificates in production**
   - Only disable for testing/development
   - Use `verify_mode::peer` at minimum

2. **Use modern TLS versions**
   - Require TLS 1.2+ for security
   - Consider TLS 1.3 for best security and performance

3. **Configure strong cipher suites**
   - Prefer ECDHE for forward secrecy
   - Use AESGCM or ChaCha20 for encryption

4. **Handle errors gracefully**
   - Check return values
   - Log certificate verification failures

5. **Keep certificates updated**
   - Monitor certificate expiration
   - Use automated renewal (Let's Encrypt, etc.)

6. **Protect private keys**
   - Use file permissions (chmod 600)
   - Consider hardware security modules for high-security applications

## Testing TLS

Use badssl.com for testing various TLS scenarios:

```cpp
// Test certificate verification
co_await tls_connect(ctx, "expired.badssl.com", 443);      // Should fail
co_await tls_connect(ctx, "wrong.host.badssl.com", 443);   // Should fail
co_await tls_connect(ctx, "self-signed.badssl.com", 443);  // Should fail
co_await tls_connect(ctx, "sha256.badssl.com", 443);       // Should succeed
```

## See Also

- [HTTP/2 Guide](HTTP2-Guide.md)
- [Networking Guide](Networking.md)
- [Performance Tuning](Performance-Tuning.md)
