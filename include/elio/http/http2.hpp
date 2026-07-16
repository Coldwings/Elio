#pragma once

/// @file http2.hpp
/// @brief HTTP/2 client support for Elio
/// 
/// This header provides HTTP/2 client functionality with:
/// - Full HTTP/2 protocol support via nghttp2
/// - TLS/ALPN negotiation (h2)
/// - HTTP/2 stream support in the session layer
/// - Header compression (HPACK)
/// - Flow control
/// - Sequential pooled connection reuse in the high-level client
///
/// The high-level h2_client facade does not coordinate multiple in-flight
/// requests over one shared connection. Use separate clients or serialize
/// calls until shared-session multiplexing is implemented.
/// 
/// Requirements:
/// - OpenSSL with ALPN support
/// - nghttp2 library. Source builds can fetch it via CMake FetchContent;
///   installed-package consumers must provide the bundled export or a
///   discoverable system nghttp2 library/header.
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
///     h2_client client;
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
