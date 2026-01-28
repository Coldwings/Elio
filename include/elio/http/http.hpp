#pragma once

/// @file http.hpp
/// @brief HTTP client and server support for Elio
/// 
/// This header provides HTTP/1.1 client and server functionality with:
/// - Full HTTP/1.1 protocol support
/// - TLS/HTTPS support via OpenSSL
/// - Connection pooling for clients
/// - Router-based request handling for servers
/// - Keep-alive connection support
/// - Chunked transfer encoding
/// - Automatic redirect following
///
/// For HTTP/2 support, include <elio/http/http2.hpp> and link with elio_http2.

#include <elio/http/http_common.hpp>
#include <elio/http/http_parser.hpp>
#include <elio/http/http_message.hpp>
#include <elio/http/http_server.hpp>
#include <elio/http/http_client.hpp>

#if defined(ELIO_HAS_HTTP2) && ELIO_HAS_HTTP2
#include <elio/http/http2.hpp>
#endif

namespace elio {

// Re-export http namespace types for convenience
namespace http {

/// @example HTTP Server Example
/// @code
/// #include <elio/elio.hpp>
/// #include <elio/http/http.hpp>
/// 
/// using namespace elio;
/// using namespace elio::http;
/// 
/// coro::task<response> hello_handler(context& ctx) {
///     co_return response::ok("Hello, World!");
/// }
/// 
/// int main() {
///     router r;
///     r.get("/", hello_handler);
///     r.get("/users/:id", [](context& ctx) -> coro::task<response> {
///         auto id = ctx.param("id");
///         co_return response::json("{\"id\": \"" + std::string(id) + "\"}");
///     });
///     
///     server srv(std::move(r));
///     
///     runtime::scheduler sched(4);
///     sched.start();
///     
///     auto task = srv.listen(net::ipv4_address(8080), io::default_io_context(), sched);
///     sched.spawn(task.release());
///     
///     // Wait...
///     sched.shutdown();
///     return 0;
/// }
/// @endcode

/// @example HTTP Client Example
/// @code
/// #include <elio/elio.hpp>
/// #include <elio/http/http.hpp>
/// 
/// using namespace elio;
/// using namespace elio::http;
/// 
/// coro::task<void> fetch_example() {
///     auto& ctx = io::default_io_context();
///     client c(ctx);
///     
///     // Simple GET request
///     auto resp = co_await c.get("https://example.com/");
///     if (resp) {
///         std::cout << "Status: " << resp->status_code() << std::endl;
///         std::cout << "Body: " << resp->body() << std::endl;
///     }
///     
///     // POST with JSON body
///     auto post_resp = co_await c.post(
///         "https://api.example.com/users",
///         R"({"name": "John", "email": "john@example.com"})",
///         mime::application_json
///     );
/// }
/// @endcode

} // namespace http

} // namespace elio
