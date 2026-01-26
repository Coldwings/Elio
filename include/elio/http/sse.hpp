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
/// #include <elio/http/sse.hpp>
///
/// using namespace elio;
/// using namespace elio::http::sse;
///
/// // SSE endpoint handler
/// coro::task<void> event_stream(sse_connection& conn) {
///     int count = 0;
///     while (conn.is_active()) {
///         // Send event every second
///         co_await time::sleep_for(std::chrono::seconds(1));
///         co_await conn.send_event("counter", std::to_string(++count));
///     }
/// }
///
/// // Note: Full SSE server integration requires custom HTTP handler
/// // See examples/sse_server.cpp for complete example
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
///     auto& ctx = io::default_io_context();
///     
///     sse_client client(ctx);
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
#include <elio/http/sse_client.hpp>

namespace elio {

// Re-export sse namespace types for convenience
namespace sse {
    using namespace http::sse;
} // namespace sse

} // namespace elio
