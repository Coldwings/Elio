# TLS Configuration Guide

Elio provides TLS/SSL support via OpenSSL. This guide covers TLS configuration for secure connections.

## Overview

TLS (Transport Layer Security) provides:
- **Encryption**: Data is encrypted in transit
- **Authentication**: Verify server (and optionally client) identity
- **Integrity**: Detect tampering with data

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
ctx.use_certificate_file("/path/to/server.crt");
ctx.use_private_key_file("/path/to/server.key");

// Optionally require client certificates
ctx.set_verify_mode(verify_mode::peer | verify_mode::fail_if_no_peer_cert);
ctx.load_verify_file("/path/to/ca.crt");
```

## Certificate Verification

### Verification Modes

```cpp
enum class verify_mode {
    none,               // No verification (insecure!)
    peer,               // Verify peer certificate
    fail_if_no_peer_cert,  // Fail if peer has no certificate
    client_once         // Only verify client cert once
};

// Combine modes with bitwise OR
ctx.set_verify_mode(verify_mode::peer | verify_mode::fail_if_no_peer_cert);
```

### Custom Verification

```cpp
// Load specific CA certificate
ctx.load_verify_file("/path/to/ca-bundle.crt");

// Load directory of CA certificates
ctx.load_verify_dir("/etc/ssl/certs/");

// Use system default CA store
ctx.use_default_verify_paths();
```

## Client Certificates (Mutual TLS)

For mutual TLS authentication:

```cpp
tls_context ctx(tls_mode::client);

// Load client certificate
ctx.use_certificate_file("/path/to/client.crt");

// Load client private key
ctx.use_private_key_file("/path/to/client.key");

// Optionally set password callback for encrypted keys
ctx.set_password_callback([](int max_len, int purpose) -> std::string {
    return "my_secret_password";
});
```

## ALPN (Application-Layer Protocol Negotiation)

ALPN is required for HTTP/2:

```cpp
// For HTTP/2 client
std::vector<std::string> protocols = {"h2", "http/1.1"};
ctx.set_alpn_protocols(protocols);

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
    co_await stream->write("GET / HTTP/1.1\r\nHost: example.com\r\n\r\n");

    char buffer[4096];
    auto result = co_await stream->read(buffer, sizeof(buffer));
    if (result.success()) {
        std::cout.write(buffer, result.bytes_transferred());
    }

    co_await stream->shutdown();
}
```

### TLS Listener (Server)

```cpp
coro::task<void> server_example() {
    tls_context ctx(tls_mode::server);
    ctx.use_certificate_file("/path/to/server.crt");
    ctx.use_private_key_file("/path/to/server.key");

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
            handle_client(std::move(*client)).go();
        }
    }
}
```

## Protocol Configuration

### Minimum TLS Version

```cpp
// Require TLS 1.2 or higher
ctx.set_min_protocol_version(TLS1_2_VERSION);

// Require TLS 1.3 only
ctx.set_min_protocol_version(TLS1_3_VERSION);
```

### Cipher Suites

```cpp
// Set preferred cipher suites (TLS 1.2)
ctx.set_cipher_list("ECDHE+AESGCM:DHE+AESGCM:ECDHE+CHACHA20");

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
http::client_config config;
config.verify_certificate = true;  // Enable verification

http::client client(ctx, config);

// Access and customize TLS context
auto& tls_ctx = client.tls_context();
tls_ctx.load_verify_file("/path/to/custom-ca.crt");

auto resp = co_await client.get("https://example.com/");
```

### HTTP/2 Client TLS Configuration

```cpp
h2_client client(ctx);

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
