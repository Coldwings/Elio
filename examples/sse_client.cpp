/// @file sse_client.cpp
/// @brief Server-Sent Events (SSE) Client Example
///
/// This example demonstrates how to connect to SSE endpoints using Elio's
/// SSE client with support for both http:// and https:// connections.
///
/// Usage: ./sse_client [url]
/// Default: http://localhost:8080/events
///
/// Features demonstrated:
/// - SSE connection
/// - Event parsing
/// - Event ID tracking
/// - Auto-reconnection
/// - Different event types

#include <elio/elio.hpp>
#include <elio/http/sse.hpp>

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <iostream>

using namespace elio;
using namespace elio::http::sse;

// Completion signaling
std::atomic<bool> g_done{false};
std::mutex g_mutex;
std::condition_variable g_cv;

/// Listen to SSE events
coro::task<void> listen_events(io::io_context& io_ctx, const std::string& url) {
    ELIO_LOG_INFO("=== SSE Client Demo ===");
    ELIO_LOG_INFO("Connecting to: {}", url);
    
    // Configure client
    client_config config;
    config.user_agent = "elio-sse-demo/1.0";
    config.auto_reconnect = true;
    config.default_retry_ms = 3000;
    config.verify_certificate = false;  // Allow self-signed certs for testing
    
    sse_client client(io_ctx, config);
    
    // Connect
    bool connected = co_await client.connect(url);
    if (!connected) {
        ELIO_LOG_ERROR("Failed to connect to {}", url);
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            g_done = true;
        }
        g_cv.notify_all();
        co_return;
    }
    
    ELIO_LOG_INFO("Connected! Listening for events...");
    ELIO_LOG_INFO("Press Ctrl+C to stop\n");
    
    // Receive events
    int event_count = 0;
    int max_events = 20;  // Limit for demo
    
    while (client.is_connected() && event_count < max_events) {
        auto evt = co_await client.receive();
        if (!evt) {
            if (client.state() == client_state::reconnecting) {
                ELIO_LOG_INFO("Reconnecting...");
                continue;
            }
            ELIO_LOG_INFO("Connection closed");
            break;
        }
        
        ++event_count;
        
        // Display event
        std::string type = evt->type.empty() ? "message" : evt->type;
        std::string id = evt->id.empty() ? "(none)" : evt->id;
        
        ELIO_LOG_INFO("[Event #{:3}] type={} id={}", event_count, type, id);
        ELIO_LOG_INFO("  data: {}", evt->data);
        
        // Track last event ID
        if (!evt->id.empty()) {
            ELIO_LOG_DEBUG("  Last-Event-ID is now: {}", client.last_event_id());
        }
    }
    
    // Close connection
    ELIO_LOG_INFO("\nClosing connection...");
    co_await client.close();
    
    ELIO_LOG_INFO("Received {} events total", event_count);
    ELIO_LOG_INFO("=== Demo Complete ===");
    
    // Signal completion
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_done = true;
    }
    g_cv.notify_all();
}

/// Simple connection test
coro::task<void> simple_test(io::io_context& io_ctx, const std::string& url) {
    ELIO_LOG_INFO("Simple SSE test: connecting to {}", url);
    
    auto client_opt = co_await sse_connect(io_ctx, url);
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
    ELIO_LOG_INFO("Connected! Waiting for events...");
    
    // Receive a few events
    for (int i = 0; i < 5; ++i) {
        auto evt = co_await client.receive();
        if (!evt) {
            ELIO_LOG_INFO("Connection closed");
            break;
        }
        
        ELIO_LOG_INFO("Event {}: {} ({})", i + 1, evt->data, 
                      evt->type.empty() ? "message" : evt->type);
    }
    
    co_await client.close();
    ELIO_LOG_INFO("Test complete");
    
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_done = true;
    }
    g_cv.notify_all();
}

/// Test reconnection behavior
coro::task<void> reconnect_test(io::io_context& io_ctx, const std::string& url) {
    ELIO_LOG_INFO("Reconnection test: connecting to {}", url);
    
    client_config config;
    config.auto_reconnect = true;
    config.default_retry_ms = 2000;
    config.max_reconnect_attempts = 3;
    
    sse_client client(io_ctx, config);
    
    if (!co_await client.connect(url)) {
        ELIO_LOG_ERROR("Initial connection failed");
        {
            std::lock_guard<std::mutex> lock(g_mutex);
            g_done = true;
        }
        g_cv.notify_all();
        co_return;
    }
    
    ELIO_LOG_INFO("Connected! Listening for events (reconnection enabled)...");
    
    int event_count = 0;
    while (event_count < 30) {  // Receive up to 30 events
        auto evt = co_await client.receive();
        
        if (!evt) {
            if (client.state() == client_state::reconnecting) {
                ELIO_LOG_INFO("Connection lost, reconnecting...");
                continue;
            }
            ELIO_LOG_INFO("Connection permanently closed");
            break;
        }
        
        ++event_count;
        ELIO_LOG_INFO("[{}] {}: {}", event_count, 
                      evt->type.empty() ? "message" : evt->type, 
                      evt->data);
    }
    
    co_await client.close();
    ELIO_LOG_INFO("Reconnection test complete ({} events received)", event_count);
    
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
              << "  --demo        Run feature demonstration (default)\n"
              << "  --simple      Run simple connection test\n"
              << "  --reconnect   Test reconnection behavior\n"
              << "  --help        Show this help\n"
              << "\n"
              << "Default URL: http://localhost:8080/events\n"
              << "\n"
              << "Examples:\n"
              << "  " << program << " http://localhost:8080/events\n"
              << "  " << program << " --simple http://localhost:3000/sse\n";
}

int main(int argc, char* argv[]) {
    std::string url = "http://localhost:8080/events";
    enum class Mode { demo, simple, reconnect } mode = Mode::demo;
    
    // Parse arguments
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--help" || arg == "-h") {
            print_usage(argv[0]);
            return 0;
        } else if (arg == "--demo" || arg == "-d") {
            mode = Mode::demo;
        } else if (arg == "--simple" || arg == "-s") {
            mode = Mode::simple;
        } else if (arg == "--reconnect" || arg == "-r") {
            mode = Mode::reconnect;
        } else if (arg[0] != '-') {
            url = arg;
        }
    }
    
    // Create scheduler
    runtime::scheduler sched(2);
    sched.set_io_context(&io::default_io_context());
    sched.start();
    
    // Run client based on mode
    switch (mode) {
        case Mode::demo: {
            auto task = listen_events(io::default_io_context(), url);
            sched.spawn(task.release());
            break;
        }
        case Mode::simple: {
            auto task = simple_test(io::default_io_context(), url);
            sched.spawn(task.release());
            break;
        }
        case Mode::reconnect: {
            auto task = reconnect_test(io::default_io_context(), url);
            sched.spawn(task.release());
            break;
        }
    }
    
    // Wait for completion with timeout
    {
        std::unique_lock<std::mutex> lock(g_mutex);
        g_cv.wait_for(lock, std::chrono::seconds(120), [] { return g_done.load(); });
    }
    
    // Brief drain before shutdown
    auto& ctx = io::default_io_context();
    for (int i = 0; i < 10 && ctx.has_pending(); ++i) {
        ctx.poll(std::chrono::milliseconds(10));
    }
    
    sched.shutdown();
    return 0;
}
