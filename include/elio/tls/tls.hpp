#pragma once

/// @file tls.hpp
/// @brief TLS/SSL support for Elio using OpenSSL
/// 
/// This header provides TLS functionality including:
/// - TLS context configuration (certificates, ciphers, verification)
/// - TLS stream wrapper for encrypted TCP connections
/// - ALPN protocol negotiation (for HTTP/2)
/// - Support for TLS 1.2 and TLS 1.3
/// - Full IPv4 and IPv6 support

#include <elio/tls/tls_context.hpp>
#include <elio/tls/tls_stream.hpp>

namespace elio::tls {

/// @example TLS Client Example (IPv4)
/// @code
/// #include <elio/elio.hpp>
/// #include <elio/tls/tls.hpp>
/// 
/// using namespace elio;
/// using namespace elio::tls;
/// 
/// coro::task<void> tls_client() {
///     auto& ctx = io::default_io_context();
///     
///     // Create client TLS context with system CA certificates
///     auto tls_ctx = tls_context::make_client();
///     
///     // Connect to HTTPS server
///     auto stream = co_await tls_connect(tls_ctx, ctx, "example.com", 443);
///     if (!stream) {
///         std::cerr << "Connection failed" << std::endl;
///         co_return;
///     }
///     
///     // Send HTTP request
///     co_await stream->write("GET / HTTP/1.1\r\nHost: example.com\r\n\r\n");
///     
///     // Read response
///     char buffer[4096];
///     auto result = co_await stream->read(buffer, sizeof(buffer));
///     if (result.result > 0) {
///         std::cout << std::string_view(buffer, result.result) << std::endl;
///     }
///     
///     co_await stream->shutdown();
/// }
/// @endcode

/// @example TLS Server Example (IPv6 dual-stack)
/// @code
/// #include <elio/elio.hpp>
/// #include <elio/tls/tls.hpp>
/// 
/// using namespace elio;
/// using namespace elio::tls;
/// 
/// coro::task<void> tls_server() {
///     auto& ctx = io::default_io_context();
///     
///     // Create server TLS context with certificate
///     auto tls_ctx = tls_context::make_server("server.crt", "server.key");
///     
///     // Create TLS listener on IPv6 (accepts both IPv4 and IPv6 by default)
///     auto listener = tls_listener::bind(net::ipv6_address(8443), ctx, tls_ctx);
///     if (!listener) {
///         std::cerr << "Failed to bind" << std::endl;
///         co_return;
///     }
///     
///     // Or bind to specific address:
///     // auto listener = tls_listener::bind(net::socket_address("::1", 8443), ctx, tls_ctx);
///     // auto listener = tls_listener::bind(net::ipv4_address("0.0.0.0", 8443), ctx, tls_ctx);
///     
///     while (true) {
///         auto stream = co_await listener->accept();
///         if (stream) {
///             // Handle connection...
///         }
///     }
/// }
/// @endcode

} // namespace elio::tls
