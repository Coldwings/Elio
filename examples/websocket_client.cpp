/// @file websocket_client.cpp
/// @brief WebSocket Client Example
///
/// This example demonstrates how to connect to WebSocket servers using Elio's
/// WebSocket client with support for both ws:// and wss:// connections.
///
/// Usage: ./websocket_client [url]
/// Default: ws://localhost:8080/ws/echo
///
/// Features demonstrated:
/// - WebSocket connection and handshake
/// - Sending text and binary messages
/// - Receiving messages
/// - Ping/pong handling
/// - Graceful close

#include <elio/elio.hpp>
#include <elio/http/websocket.hpp>

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <iostream>

using namespace elio;
using namespace elio::http::websocket;

// Completion signaling
std::atomic<bool> g_done{false};
std::mutex g_mutex;
std::condition_variable g_cv;

/// Interactive WebSocket client session
coro::task<void> interactive_session(ws_client& client) {
    ELIO_LOG_INFO("=== Interactive WebSocket Session ===");
    ELIO_LOG_INFO("Connection established!");
    ELIO_LOG_INFO("Subprotocol: {}", 
                  client.subprotocol().empty() ? "(none)" : client.subprotocol());
    
    // Send initial message
    co_await client.send_text("Hello from Elio WebSocket client!");
    ELIO_LOG_INFO("Sent: Hello from Elio WebSocket client!");
    
    // Receive messages
    int msg_count = 0;
    while (client.is_open() && msg_count < 5) {
        auto msg = co_await client.receive();
        if (!msg) {
            ELIO_LOG_INFO("Connection closed");
            break;
        }
        
        ++msg_count;
        
        if (msg->type == opcode::text) {
            ELIO_LOG_INFO("Received text: {}", msg->data);
        } else if (msg->type == opcode::binary) {
            ELIO_LOG_INFO("Received binary: {} bytes", msg->data.size());
        }
        
        // Send another message after receiving
        if (msg_count < 5) {
            std::string reply = "Message #" + std::to_string(msg_count);
            co_await client.send_text(reply);
            ELIO_LOG_INFO("Sent: {}", reply);
        }
    }
    
    // Close connection
    ELIO_LOG_INFO("Closing connection...");
    co_await client.close(close_code::normal, "Session complete");
    ELIO_LOG_INFO("Connection closed gracefully");
}

/// Demonstrate various WebSocket features
coro::task<void> demo_features(io::io_context& io_ctx, const std::string& url) {
    ELIO_LOG_INFO("=== WebSocket Client Demo ===");
    ELIO_LOG_INFO("Connecting to: {}", url);
    
    // Configure client
    client_config config;
    config.user_agent = "elio-websocket-demo/1.0";
    config.verify_certificate = false;  // Allow self-signed certs for testing
    
    ws_client client(io_ctx, config);
    
    // Connect
    bool connected = co_await client.connect(url);
    if (!connected) {
        ELIO_LOG_ERROR("Failed to connect to {}", url);
        // Signal completion
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            g_done = true;
        }
        g_cv.notify_all();
        co_return;
    }
    
    ELIO_LOG_INFO("Connected successfully!");
    
    // --- Feature 1: Text Messages ---
    ELIO_LOG_INFO("\n--- Text Messages ---");
    
    co_await client.send_text("Hello, WebSocket!");
    ELIO_LOG_INFO("Sent text message");
    
    auto msg = co_await client.receive();
    if (msg && msg->type == opcode::text) {
        ELIO_LOG_INFO("Received: {}", msg->data);
    }
    
    // --- Feature 2: JSON Messages ---
    ELIO_LOG_INFO("\n--- JSON Messages ---");
    
    std::string json = R"({"action":"test","value":42})";
    co_await client.send_text(json);
    ELIO_LOG_INFO("Sent JSON: {}", json);
    
    msg = co_await client.receive();
    if (msg && msg->type == opcode::text) {
        ELIO_LOG_INFO("Received: {}", msg->data);
    }
    
    // --- Feature 3: Binary Messages ---
    ELIO_LOG_INFO("\n--- Binary Messages ---");
    
    std::string binary_data;
    for (int i = 0; i < 256; ++i) {
        binary_data += static_cast<char>(i);
    }
    co_await client.send_binary(binary_data);
    ELIO_LOG_INFO("Sent {} bytes of binary data", binary_data.size());
    
    msg = co_await client.receive();
    if (msg && msg->type == opcode::binary) {
        ELIO_LOG_INFO("Received {} bytes of binary data", msg->data.size());
        bool match = (msg->data == binary_data);
        ELIO_LOG_INFO("Data matches: {}", match ? "yes" : "no");
    }
    
    // --- Feature 4: Ping/Pong ---
    ELIO_LOG_INFO("\n--- Ping/Pong ---");
    
    co_await client.send_ping("test-ping");
    ELIO_LOG_INFO("Sent ping");
    // Note: Pong is handled automatically by the receive loop
    
    // --- Feature 5: Multiple Messages ---
    ELIO_LOG_INFO("\n--- Multiple Messages ---");
    
    for (int i = 1; i <= 3; ++i) {
        std::string msg_text = "Message " + std::to_string(i);
        co_await client.send_text(msg_text);
        ELIO_LOG_INFO("Sent: {}", msg_text);
        
        auto response = co_await client.receive();
        if (response && response->type == opcode::text) {
            ELIO_LOG_INFO("Received: {}", response->data);
        }
    }
    
    // --- Close Connection ---
    ELIO_LOG_INFO("\n--- Closing Connection ---");
    co_await client.close(close_code::normal, "Demo complete");
    ELIO_LOG_INFO("Connection closed");
    
    ELIO_LOG_INFO("\n=== Demo Complete ===");
    
    // Signal completion
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_done = true;
    }
    g_cv.notify_all();
}

/// Simple echo test
coro::task<void> echo_test(io::io_context& io_ctx, const std::string& url) {
    ELIO_LOG_INFO("Echo test: connecting to {}", url);
    
    auto client_opt = co_await ws_connect(io_ctx, url);
    if (!client_opt) {
        ELIO_LOG_ERROR("Failed to connect");
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            g_done = true;
        }
        g_cv.notify_all();
        co_return;
    }
    
    auto& client = *client_opt;
    ELIO_LOG_INFO("Connected!");
    
    // Echo test
    const char* messages[] = {
        "Hello",
        "World",
        "WebSocket",
        "Test"
    };
    
    for (const char* msg : messages) {
        co_await client.send_text(msg);
        ELIO_LOG_INFO("Sent: {}", msg);
        
        auto response = co_await client.receive();
        if (response) {
            ELIO_LOG_INFO("Received: {}", response->data);
        }
    }
    
    co_await client.close();
    ELIO_LOG_INFO("Echo test complete");
    
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_done = true;
    }
    g_cv.notify_all();
}

void print_usage(const char* program) {
    std::cout << "Usage: " << program << " [options] [url]\n"
              << "\n"
              << "Options:\n"
              << "  --demo     Run feature demonstration\n"
              << "  --echo     Run simple echo test\n"
              << "  --help     Show this help\n"
              << "\n"
              << "Default URL: ws://localhost:8080/ws/echo\n"
              << "\n"
              << "Examples:\n"
              << "  " << program << " ws://localhost:8080/ws/echo\n"
              << "  " << program << " wss://echo.websocket.org\n"
              << "  " << program << " --demo\n";
}

int main(int argc, char* argv[]) {
    std::string url = "ws://localhost:8080/ws/echo";
    bool demo_mode = false;
    
    // Parse arguments
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--help" || arg == "-h") {
            print_usage(argv[0]);
            return 0;
        } else if (arg == "--demo" || arg == "-d") {
            demo_mode = true;
        } else if (arg == "--echo" || arg == "-e") {
            demo_mode = false;
        } else if (arg[0] != '-') {
            url = arg;
        }
    }
    
    // Create scheduler
    runtime::scheduler sched(2);
    sched.start();
    
    // Run client
    if (demo_mode) {
        auto task = demo_features(io::default_io_context(), url);
        sched.spawn(task.release());
    } else {
        auto task = echo_test(io::default_io_context(), url);
        sched.spawn(task.release());
    }
    
    // Wait for completion with timeout
    {
        std::unique_lock<std::mutex> lock(g_mutex);
        g_cv.wait_for(lock, std::chrono::seconds(60), [] { return g_done.load(); });
    }
    
    // Brief drain before shutdown
    auto& ctx = io::default_io_context();
    for (int i = 0; i < 10 && ctx.has_pending(); ++i) {
        ctx.poll(std::chrono::milliseconds(10));
    }
    
    sched.shutdown();
    return 0;
}
