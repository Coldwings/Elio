# WebSocket and Server-Sent Events

Elio provides full support for WebSocket (RFC 6455) and Server-Sent Events (SSE) for real-time bidirectional and unidirectional communication.

## Design Notes

The **WebSocket** implementation follows RFC 6455, including frame masking for all client-to-server messages as required by the specification. Masking prevents cache poisoning attacks on intermediary proxies that might misinterpret WebSocket frames as HTTP. The server-side does not mask frames, as the RFC only mandates client-to-server masking.

**Server-Sent Events (SSE)** provides a simpler alternative to WebSocket for server-to-client streaming. SSE operates over standard HTTP, requires no protocol upgrade, and benefits from automatic reconnection built into the browser EventSource API. The tradeoff is that SSE is unidirectional (server to client only), but for many use cases -- notifications, live feeds, progress updates -- this is sufficient and avoids the complexity of a bidirectional protocol.

## WebSocket

WebSocket provides full-duplex communication channels over a single TCP connection.

### WebSocket Server

```cpp
#include <elio/elio.hpp>
#include <elio/http/websocket.hpp>

using namespace elio;
using namespace elio::http::websocket;

// WebSocket message handler
coro::task<void> echo_handler(ws_connection& conn) {
    while (conn.is_open()) {
        auto msg = co_await conn.receive();
        if (!msg) break;
        
        // Echo back the message
        if (msg->type == opcode::text) {
            co_await conn.send_text(msg->data);
        } else if (msg->type == opcode::binary) {
            co_await conn.send_binary(msg->data);
        }
    }
}

coro::task<int> async_main(int argc, char* argv[]) {
    (void)argc;
    (void)argv;

    // Create WebSocket-enabled router
    ws_router router;
    
    // Regular HTTP routes
    router.get("/", [](http::context& ctx) -> coro::task<http::response> {
        co_return http::response::ok("Hello!");
    });
    
    // WebSocket routes
    server_config ws_config;
    ws_config.max_message_size = 1024 * 1024;  // 1MB
    
    router.websocket("/ws", echo_handler, ws_config);
    
    // Create and start server
    ws_server srv(std::move(router));
    
    auto bind_addr = net::socket_address(net::ipv4_address(8080));

    // Run until SIGINT/SIGTERM, then stop the listener cleanly.
    co_await elio::serve(srv, [&]() { return srv.listen(bind_addr); });
    co_return 0;
}

int main(int argc, char* argv[]) {
    elio::signal::signal_set shutdown_signals(elio::default_shutdown_signals);
    shutdown_signals.block_all_threads();
    return elio::run(async_main, argc, argv);
}
```

### WebSocket Client

```cpp
#include <elio/elio.hpp>
#include <elio/http/websocket.hpp>

using namespace elio;
using namespace elio::http::websocket;

coro::task<void> connect_example() {
    // Create client
    client_config config;
    config.subprotocols = {"chat", "json"};  // Optional subprotocols
    config.connect_timeout = std::chrono::seconds(10);
    config.read_timeout = std::chrono::seconds(30);  // Upgrade response deadline

    ws_client client(config);
    coro::cancel_source cancel;
    
    // Connect to server
    if (!co_await client.connect("ws://localhost:8080/ws",
                                 cancel.get_token())) {
        ELIO_LOG_ERROR("Failed to connect");
        co_return;
    }
    
    ELIO_LOG_INFO("Connected! Subprotocol: {}", client.subprotocol());
    
    // Send messages
    co_await client.send_text("Hello, server!");
    co_await client.send_binary("\x01\x02\x03");
    
    // Receive messages
    while (client.is_open()) {
        auto msg = co_await client.receive(cancel.get_token());
        if (!msg) break;
        
        if (msg->type == opcode::text) {
            ELIO_LOG_INFO("Received: {}", msg->data);
        }
    }
    
    // Close connection
    co_await client.close(close_code::normal, "Done");
}
```

### WebSocket Frame Types

| Opcode | Name | Description |
|--------|------|-------------|
| 0x0 | continuation | Continuation of fragmented message |
| 0x1 | text | UTF-8 text data |
| 0x2 | binary | Binary data |
| 0x8 | close | Connection close |
| 0x9 | ping | Ping (keep-alive) |
| 0xA | pong | Pong (response to ping) |

### Close Codes

| Code | Name | Description |
|------|------|-------------|
| 1000 | normal | Normal closure |
| 1001 | going_away | Endpoint going away |
| 1002 | protocol_error | Protocol error |
| 1003 | unsupported | Unsupported data type |
| 1008 | policy_violation | Policy violation |
| 1009 | too_large | Message too big |
| 1011 | unexpected | Unexpected condition |

### Secure WebSocket (WSS)

For secure connections, use `wss://` URLs and configure TLS:

```cpp
// Server with TLS
auto tls_ctx = tls::tls_context::make_server("cert.pem", "key.pem");
auto task = srv.listen_tls(net::ipv4_address(8443), tls_ctx);

// Client with TLS
ws_client client;
client.tls_context().use_default_verify_paths();
co_await client.connect("wss://example.com/ws");
```

The server-side TLS handshake is bounded by the `http::server_config`
passed to `ws_server`; `keep_alive_timeout` controls both the inbound TLS
handshake and the HTTP upgrade request read. Set it to a non-positive duration
to disable those deadlines.

## Server-Sent Events (SSE)

SSE provides server-to-client event streaming over HTTP.

### SSE Server

```cpp
#include <elio/elio.hpp>
#include <elio/http/sse.hpp>

using namespace elio;
using namespace elio::http::sse;

// Event stream handler
coro::task<void> event_stream(sse_connection& conn) {
    int counter = 0;
    
    // Send retry interval
    co_await conn.send_retry(3000);  // 3 seconds
    
    while (conn.is_active()) {
        ++counter;
        
        // Send a simple event
        co_await conn.send_data("Counter: " + std::to_string(counter));
        
        // Send a typed event
        event evt = event::full(
            std::to_string(counter),  // id
            "update",                  // type
            R"({"count":)" + std::to_string(counter) + "}"
        );
        co_await conn.send(evt);
        
        // Wait before next event
        co_await time::sleep_for(std::chrono::seconds(1));
    }
}
```

### SSE Client

```cpp
#include <elio/elio.hpp>
#include <elio/http/sse.hpp>

using namespace elio;
using namespace elio::http::sse;

coro::task<void> listen_events() {
    auto& ctx = io::default_io_context();
    
    // Configure client
    client_config config;
    config.auto_reconnect = true;
    config.default_retry_ms = 3000;
    config.connect_timeout = std::chrono::seconds(10);
    config.read_timeout = std::chrono::seconds(30);  // Response header deadline
    
    sse_client client(config);
    coro::cancel_source cancel;
    
    // Connect
    if (!co_await client.connect("http://localhost:8080/events",
                                 cancel.get_token())) {
        ELIO_LOG_ERROR("Failed to connect");
        co_return;
    }
    
    // Receive events
    while (client.is_connected()) {
        auto evt = co_await client.receive(cancel.get_token());
        if (!evt) break;
        
        ELIO_LOG_INFO("Event: type={} id={} data={}", 
                      evt->type.empty() ? "message" : evt->type,
                      evt->id,
                      evt->data);
    }
}
```

For WebSocket and SSE clients, `connect_timeout` bounds TCP connect and TLS
handshake setup. `read_timeout` bounds the protocol response headers read by
`connect()` -- the WebSocket `101 Switching Protocols` response or the SSE
`text/event-stream` response headers. Values less than or equal to zero disable
these client-side read deadlines.

Both clients also provide cancellation-token overloads. Cancelling the token
passed to `connect()` aborts pending TCP connect, TLS handshake, request write,
and response header reads. Cancelling the token passed to `receive()` aborts a
blocked WebSocket frame or SSE event read. Cancellation returns `false` or
`std::nullopt` and sets `errno` to `ECANCELED`.

### SSE Event Format

SSE events are formatted as text with specific fields:

```
id: event-123
event: notification
retry: 3000
data: Hello, World!
data: This is line 2

```

- `id:` - Event ID for reconnection tracking
- `event:` - Event type (default: "message")
- `retry:` - Reconnection interval in milliseconds
- `data:` - Event data (can span multiple lines)
- Empty line marks end of event

### SSE vs WebSocket

| Feature | SSE | WebSocket |
|---------|-----|-----------|
| Direction | Server → Client only | Bidirectional |
| Protocol | HTTP | Custom over TCP |
| Reconnection | Automatic | Manual |
| Browser support | EventSource API | WebSocket API |
| Complexity | Simple | More complex |
| Best for | Notifications, feeds | Chat, games, real-time |

### When to Use SSE

- Real-time notifications
- Live feeds (news, stocks)
- Progress updates
- Log streaming
- Any server-push scenario without client messages

### When to Use WebSocket

- Chat applications
- Real-time games
- Collaborative editing
- Any scenario requiring bidirectional communication

## Browser Integration

### WebSocket (JavaScript)

```javascript
const ws = new WebSocket('ws://localhost:8080/ws');

ws.onopen = () => {
    console.log('Connected');
    ws.send('Hello, server!');
};

ws.onmessage = (event) => {
    console.log('Received:', event.data);
};

ws.onclose = (event) => {
    console.log('Disconnected:', event.code);
};

// Send messages
ws.send('Hello');
ws.send(new Blob([1, 2, 3]));

// Close
ws.close(1000, 'Done');
```

### SSE (JavaScript)

```javascript
const es = new EventSource('/events');

es.onmessage = (event) => {
    console.log('Message:', event.data);
};

es.addEventListener('notification', (event) => {
    console.log('Notification:', event.data);
    console.log('ID:', event.lastEventId);
});

es.onerror = (error) => {
    console.error('Error:', error);
    // EventSource will auto-reconnect
};

// Close
es.close();
```

## Examples

Complete examples are available:

- `examples/websocket_server.cpp` - WebSocket server with echo and chat
- `examples/websocket_client.cpp` - WebSocket client demo
- `examples/sse_server.cpp` - SSE event streaming server
- `examples/sse_client.cpp` - SSE event receiver

## Next Steps

- See [[API Reference]] for detailed API documentation
- See [[Networking]] for TCP and HTTP documentation
- See [[Examples]] for more code examples
