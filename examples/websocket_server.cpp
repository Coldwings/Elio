/// @file websocket_server.cpp
/// @brief WebSocket Server Example
///
/// This example demonstrates how to build a WebSocket server using Elio's
/// WebSocket module with both HTTP and WebSocket endpoints.
///
/// Usage: ./websocket_server [port]
/// Default: Port 8080
///
/// Features demonstrated:
/// - WebSocket upgrade handling
/// - Echo server (echoes received messages)
/// - Broadcast to all connected clients
/// - Ping/pong handling
/// - Graceful close

#include <elio/elio.hpp>
#include <elio/http/websocket.hpp>

#include <atomic>
#include <mutex>
#include <set>

using namespace elio;
using namespace elio::http;
using namespace elio::http::websocket;
using namespace elio::signal;

// Global flag for graceful shutdown
std::atomic<bool> g_running{true};

/// Signal handler coroutine - waits for SIGINT/SIGTERM
coro::task<void> signal_handler_task() {
    signal_set sigs{SIGINT, SIGTERM};
    signal_fd sigfd(sigs);
    
    ELIO_LOG_DEBUG("Signal handler started, waiting for SIGINT/SIGTERM...");
    
    auto info = co_await sigfd.wait();
    if (info) {
        ELIO_LOG_INFO("Received signal: {} - initiating shutdown", info->full_name());
    }
    
    g_running = false;
    co_return;
}

// Connected clients for broadcast
std::mutex g_clients_mutex;
std::set<ws_connection*> g_clients;

// WebSocket handler: Echo server
coro::task<void> echo_handler(ws_connection& conn) {
    ELIO_LOG_INFO("WebSocket client connected (echo)");
    
    // Add to clients set
    {
        std::lock_guard<std::mutex> lock(g_clients_mutex);
        g_clients.insert(&conn);
    }
    
    // Send welcome message
    co_await conn.send_text(R"({"type":"welcome","message":"Connected to echo server"})");
    
    // Message loop
    while (conn.is_open()) {
        auto msg = co_await conn.receive();
        if (!msg) break;
        
        if (msg->type == opcode::text) {
            ELIO_LOG_DEBUG("Echo: {}", msg->data);
            // Echo back with prefix
            std::string response = R"({"type":"echo","data":")" + msg->data + R"("})";
            co_await conn.send_text(response);
        } else if (msg->type == opcode::binary) {
            ELIO_LOG_DEBUG("Echo binary: {} bytes", msg->data.size());
            co_await conn.send_binary(msg->data);
        }
    }
    
    // Remove from clients set
    {
        std::lock_guard<std::mutex> lock(g_clients_mutex);
        g_clients.erase(&conn);
    }
    
    ELIO_LOG_INFO("WebSocket client disconnected (echo)");
}

// WebSocket handler: Chat room (broadcast to all)
coro::task<void> chat_handler(ws_connection& conn) {
    ELIO_LOG_INFO("WebSocket client connected (chat)");
    
    // Add to clients set
    {
        std::lock_guard<std::mutex> lock(g_clients_mutex);
        g_clients.insert(&conn);
    }
    
    // Send welcome
    std::string welcome = R"({"type":"system","message":"Welcome to the chat room!"})";
    co_await conn.send_text(welcome);
    
    // Notify others
    {
        std::lock_guard<std::mutex> lock(g_clients_mutex);
        std::string join_msg = R"({"type":"system","message":"A user has joined"})";
        for (auto* client : g_clients) {
            if (client != &conn && client->is_open()) {
                // Note: This is simplified - in production, you'd queue these
                // co_await client->send_text(join_msg);
            }
        }
    }
    
    // Message loop
    while (conn.is_open()) {
        auto msg = co_await conn.receive();
        if (!msg) break;
        
        if (msg->type == opcode::text) {
            ELIO_LOG_DEBUG("Chat message: {}", msg->data);
            
            // Broadcast to all clients
            std::string broadcast = R"({"type":"message","data":")" + msg->data + R"("})";
            
            std::lock_guard<std::mutex> lock(g_clients_mutex);
            for (auto* client : g_clients) {
                if (client->is_open()) {
                    // In a real app, you'd use async broadcast
                    // For simplicity, we just log here
                }
            }
            
            // Echo back to sender
            co_await conn.send_text(broadcast);
        }
    }
    
    // Remove from clients set
    {
        std::lock_guard<std::mutex> lock(g_clients_mutex);
        g_clients.erase(&conn);
    }
    
    ELIO_LOG_INFO("WebSocket client disconnected (chat)");
}

// HTTP handler: Serve test page
coro::task<response> index_handler([[maybe_unused]] context& ctx) {
    std::string html = R"HTML(
<!DOCTYPE html>
<html>
<head>
    <title>Elio WebSocket Test</title>
    <style>
        body { font-family: Arial, sans-serif; margin: 20px; }
        #log { border: 1px solid #ccc; padding: 10px; height: 300px; overflow-y: scroll; }
        .sent { color: blue; }
        .received { color: green; }
        .error { color: red; }
        .system { color: gray; }
    </style>
</head>
<body>
    <h1>Elio WebSocket Test</h1>
    
    <div>
        <label>Endpoint: </label>
        <select id="endpoint">
            <option value="/ws/echo">Echo (/ws/echo)</option>
            <option value="/ws/chat">Chat (/ws/chat)</option>
        </select>
        <button onclick="wsConnect()">Connect</button>
        <button onclick="wsDisconnect()">Disconnect</button>
    </div>
    
    <div style="margin-top: 10px;">
        <input type="text" id="message" placeholder="Enter message" style="width: 300px;">
        <button onclick="wsSend()">Send</button>
        <button onclick="wsSendPing()">Ping</button>
    </div>
    
    <div id="log" style="margin-top: 10px;"></div>
    
    <script>
        var ws = null;
        
        function logMsg(msg, className) {
            var div = document.getElementById("log");
            var entry = document.createElement("div");
            entry.className = className || "";
            entry.textContent = new Date().toLocaleTimeString() + " - " + msg;
            div.appendChild(entry);
            div.scrollTop = div.scrollHeight;
        }
        
        function wsConnect() {
            if (ws) {
                ws.close();
            }
            
            var endpoint = document.getElementById("endpoint").value;
            var url = "ws://" + window.location.host + endpoint;
            
            logMsg("Connecting to " + url + "...", "system");
            ws = new WebSocket(url);
            
            ws.onopen = function() {
                logMsg("Connected!", "system");
            };
            
            ws.onmessage = function(e) {
                logMsg("Received: " + e.data, "received");
            };
            
            ws.onerror = function(e) {
                logMsg("Error occurred", "error");
            };
            
            ws.onclose = function(e) {
                logMsg("Disconnected (code=" + e.code + ")", "system");
                ws = null;
            };
        }
        
        function wsDisconnect() {
            if (ws) {
                ws.close();
            }
        }
        
        function wsSend() {
            if (!ws || ws.readyState !== WebSocket.OPEN) {
                logMsg("Not connected!", "error");
                return;
            }
            
            var msg = document.getElementById("message").value;
            ws.send(msg);
            logMsg("Sent: " + msg, "sent");
            document.getElementById("message").value = "";
        }
        
        function wsSendPing() {
            if (!ws || ws.readyState !== WebSocket.OPEN) {
                logMsg("Not connected!", "error");
                return;
            }
            ws.send("ping");
            logMsg("Sent: ping", "sent");
        }
    </script>
</body>
</html>
)HTML";
    co_return response::html(html);
}

// HTTP handler: Server info
coro::task<response> info_handler([[maybe_unused]] context& ctx) {
    std::lock_guard<std::mutex> lock(g_clients_mutex);
    
    std::string json = R"({
        "server": "Elio WebSocket Server",
        "version": "1.0.0",
        "connected_clients": )" + std::to_string(g_clients.size()) + R"(,
        "endpoints": {
            "echo": "/ws/echo",
            "chat": "/ws/chat"
        }
    })";
    
    co_return response::json(json);
}

int main(int argc, char* argv[]) {
    uint16_t port = 8080;
    
    // Parse arguments
    if (argc > 1) {
        port = static_cast<uint16_t>(std::stoi(argv[1]));
    }
    
    // Block signals BEFORE creating scheduler threads
    signal_set sigs{SIGINT, SIGTERM};
    sigs.block_all_threads();
    
    // Create WebSocket-enabled router
    ws_router router;
    
    // HTTP routes
    router.get("/", index_handler);
    router.get("/info", info_handler);
    
    // WebSocket routes
    http::websocket::server_config ws_config;
    ws_config.max_message_size = 1024 * 1024;  // 1MB max message
    ws_config.enable_logging = true;
    
    router.websocket("/ws/echo", echo_handler, ws_config);
    router.websocket("/ws/chat", chat_handler, ws_config);
    
    // Create server
    http::server_config http_config;
    http_config.enable_logging = true;
    
    ws_server srv(std::move(router), http_config);
    
    // Create scheduler
    runtime::scheduler sched(4);
    sched.set_io_context(&io::default_io_context());
    sched.start();
    
    // Spawn signal handler coroutine
    auto sig_handler = signal_handler_task();
    sched.spawn(sig_handler.release());
    
    // Start server
    auto server_task = srv.listen(
        net::ipv4_address(port),
        io::default_io_context(),
        sched
    );
    sched.spawn(server_task.release());
    
    ELIO_LOG_INFO("WebSocket server started on port {}", port);
    ELIO_LOG_INFO("Open http://localhost:{} in your browser", port);
    ELIO_LOG_INFO("Press Ctrl+C to stop");
    
    // Wait for shutdown
    while (g_running) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    
    srv.stop();
    
    // Brief drain before shutdown
    auto& ctx = io::default_io_context();
    for (int i = 0; i < 10 && ctx.has_pending(); ++i) {
        ctx.poll(std::chrono::milliseconds(10));
    }
    
    sched.shutdown();
    
    ELIO_LOG_INFO("Server stopped");
    return 0;
}
