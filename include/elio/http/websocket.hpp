#pragma once

/// @file websocket.hpp
/// @brief WebSocket support for Elio
///
/// This header provides complete WebSocket functionality including:
/// - WebSocket frame encoding/decoding (RFC 6455)
/// - Client connections (ws:// and wss://)
/// - Server with HTTP upgrade handling
/// - Automatic ping/pong handling
/// - Message fragmentation support
///
/// @example WebSocket Server Example
/// @code
/// #include <elio/elio.hpp>
/// #include <elio/http/websocket.hpp>
///
/// using namespace elio;
/// using namespace elio::http::websocket;
///
/// coro::task<void> handle_websocket(ws_connection& conn) {
///     while (conn.is_open()) {
///         auto msg = co_await conn.receive();
///         if (!msg) break;
///         
///         if (msg->type == opcode::text) {
///             co_await conn.send_text("Echo: " + msg->data);
///         }
///     }
/// }
///
/// int main() {
///     ws_router router;
///     router.websocket("/ws", handle_websocket);
///     router.get("/", [](context& ctx) -> coro::task<response> {
///         co_return response::ok("Hello!");
///     });
///     
///     ws_server srv(std::move(router));
///     
///     runtime::scheduler sched(4);
///     sched.start();
///     
///     auto task = srv.listen(net::ipv4_address(8080), 
///                           io::default_io_context(), sched);
///     sched.spawn(task.release());
///     
///     // Wait for shutdown...
///     sched.shutdown();
///     return 0;
/// }
/// @endcode
///
/// @example WebSocket Client Example
/// @code
/// #include <elio/elio.hpp>
/// #include <elio/http/websocket.hpp>
///
/// using namespace elio;
/// using namespace elio::http::websocket;
///
/// coro::task<void> connect_to_server() {
///     auto& ctx = io::default_io_context();
///     ws_client client(ctx);
///     
///     if (!co_await client.connect("ws://localhost:8080/ws")) {
///         ELIO_LOG_ERROR("Failed to connect");
///         co_return;
///     }
///     
///     // Send a message
///     co_await client.send_text("Hello, server!");
///     
///     // Receive response
///     auto msg = co_await client.receive();
///     if (msg) {
///         ELIO_LOG_INFO("Received: {}", msg->data);
///     }
///     
///     co_await client.close();
/// }
/// @endcode

#include <elio/http/websocket_frame.hpp>
#include <elio/http/websocket_handshake.hpp>
#include <elio/http/websocket_server.hpp>
#include <elio/http/websocket_client.hpp>

namespace elio {

// Re-export websocket namespace types for convenience
namespace websocket {
    using namespace http::websocket;
} // namespace websocket

} // namespace elio
