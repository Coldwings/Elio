#pragma once

/// @file sse.hpp
/// @brief Server-Sent Events (SSE) support for Elio
///
/// This header provides complete SSE functionality including:
/// - Event serialization/parsing
/// - Client connections with auto-reconnection
/// - Server-side event streaming
/// - Last-Event-ID tracking
///
/// SSE is a simpler alternative to WebSocket for scenarios where only
/// server-to-client communication is needed (real-time updates, notifications).
///
/// @example SSE Server Example
/// @code
/// #include <elio/elio.hpp>
/// #include <elio/http/http.hpp>
/// #include <elio/http/sse.hpp>
///
/// using namespace elio;
/// using namespace elio::http::sse;
///
/// // Server-owned producer: borrowed event data stays alive through each await.
/// coro::task<http::send_result> event_stream(event_writer& out,
///                                          coro::cancel_token token) {
///     int count = 0;
///     while (!token.is_cancelled()) {
///         const auto data = std::to_string(++count);
///         const auto sent = co_await out.send_event({{}, "counter", data}, token);
///         if (!sent.success()) co_return sent;
///         const auto waited = co_await time::sleep_for(std::chrono::seconds(1), token);
///         if (waited == coro::cancel_result::cancelled) break;
///     }
///     co_return http::send_result{http::send_errc::cancelled, ECANCELED};
/// }
///
/// coro::task<void> server_main() {
///     http::router routes;
///     routes.get("/events", [](http::context&) {
///         return make_streaming_response(event_stream);
///     });
///     http::server server(std::move(routes));
///     const auto address = net::socket_address(net::ipv4_address(8080));
///     co_await elio::serve(server, [&] { return server.listen(address); });
/// }
/// // Block shutdown signals before starting scheduler threads, as shown in
/// // examples/sse_server.cpp. serve() joins the listener and drains sessions;
/// // never detach an event_writer or write HTTP framing manually.
/// @endcode
///
/// @example SSE Client Example
/// @code
/// #include <elio/elio.hpp>
/// #include <elio/http/sse.hpp>
///
/// using namespace elio;
/// using namespace elio::http::sse;
///
/// coro::task<void> listen_to_events() {
///     sse_client client;
///     if (!co_await client.connect("http://localhost:8080/events")) {
///         ELIO_LOG_ERROR("Failed to connect");
///         co_return;
///     }
///     
///     while (client.is_connected()) {
///         auto evt = co_await client.receive();
///         if (!evt) break;
///         
///         ELIO_LOG_INFO("Event: type={} data={}", evt->type, evt->data);
///     }
/// }
/// @endcode

#include <elio/http/sse_server.hpp>
#include <elio/http/sse_writer.hpp>
#include <elio/http/sse_client.hpp>

namespace elio {

// Re-export sse namespace types for convenience
namespace sse {
    using namespace http::sse;
} // namespace sse

} // namespace elio
