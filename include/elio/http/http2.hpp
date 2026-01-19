#pragma once

/// @file http2.hpp
/// @brief HTTP/2 client support for Elio
/// 
/// This header provides HTTP/2 client functionality with:
/// - Full HTTP/2 protocol support via nghttp2
/// - TLS/ALPN negotiation (h2)
/// - Multiplexed streams over a single connection
/// - Header compression (HPACK)
/// - Flow control
/// 
/// Requirements:
/// - OpenSSL with ALPN support
/// - nghttp2 library (fetched automatically via CMake FetchContent)
/// 
/// Note: HTTP/2 requires HTTPS (h2 over TLS). For plaintext HTTP, use the
/// HTTP/1.1 client instead. While h2c (HTTP/2 cleartext) exists, it is
/// rarely supported by servers.

#include <elio/http/http_common.hpp>
#include <elio/http/http_message.hpp>
#include <elio/http/http2_session.hpp>
#include <elio/http/http2_client.hpp>

namespace elio {

namespace http {

/// @example HTTP/2 Client Example
/// @code
/// #include <elio/elio.hpp>
/// #include <elio/http/http2.hpp>
/// 
/// using namespace elio;
/// using namespace elio::http;
/// 
/// coro::task<void> fetch_example() {
///     auto& ctx = io::default_io_context();
///     h2_client client(ctx);
///     
///     // Simple GET request (HTTP/2 requires HTTPS)
///     auto resp = co_await client.get("https://example.com/");
///     if (resp) {
///         std::cout << "Status: " << static_cast<int>(resp->get_status()) << std::endl;
///         std::cout << "Body: " << resp->body() << std::endl;
///     }
///     
///     // POST with JSON body
///     auto post_resp = co_await client.post(
///         "https://api.example.com/users",
///         R"({"name": "John", "email": "john@example.com"})",
///         mime::application_json
///     );
/// }
/// @endcode

} // namespace http

} // namespace elio
