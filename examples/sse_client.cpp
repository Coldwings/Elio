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

#include <iostream>

using namespace elio;
using namespace elio::http::sse;

/// Listen to SSE events
coro::task<void> listen_events(const std::string& url) {
    ELIO_LOG_INFO("=== SSE Client Demo ===");
    ELIO_LOG_INFO("Connecting to: {}", url);

    // Configure client
    client_config config;
    config.user_agent = "elio-sse-demo/1.0";
    config.auto_reconnect = true;
    config.default_retry_ms = 3000;
    config.verify_certificate = false;  // Allow self-signed certs for testing

    sse_client client(config);

    // Connect
    bool connected = co_await client.connect(url);
    if (!connected) {
        ELIO_LOG_ERROR("Failed to connect to {}", url);
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
    co_return;
}

/// Simple connection test
coro::task<void> simple_test(const std::string& url) {
    ELIO_LOG_INFO("Simple SSE test: connecting to {}", url);

    auto client_opt = co_await sse_connect(url);
    if (!client_opt) {
        ELIO_LOG_ERROR("Failed to connect");
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
    co_return;
}

/// Test reconnection behavior
coro::task<void> reconnect_test(const std::string& url) {
    ELIO_LOG_INFO("Reconnection test: connecting to {}", url);

    client_config config;
    config.auto_reconnect = true;
    config.default_retry_ms = 2000;
    config.max_reconnect_attempts = 3;

    sse_client client(config);

    if (!co_await client.connect(url)) {
        ELIO_LOG_ERROR("Initial connection failed");
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
    co_return;
}

/// Async main - uses ELIO_ASYNC_MAIN for automatic scheduler management
coro::task<int> async_main(int argc, char* argv[]) {
    std::string url = "http://localhost:8080/events";
    enum class Mode { demo, simple, reconnect } mode = Mode::demo;

    // Parse arguments
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--help" || arg == "-h") {
            std::cout << "Usage: " << argv[0] << " [options] [url]\n"
                      << "\n"
                      << "Options:\n"
                      << "  --demo, -d       Run feature demonstration (default)\n"
                      << "  --simple, -s     Run simple connection test\n"
                      << "  --reconnect, -r  Test reconnection behavior\n"
                      << "  --help, -h       Show this help\n"
                      << "\n"
                      << "Default URL: http://localhost:8080/events\n"
                      << "\n"
                      << "Examples:\n"
                      << "  " << argv[0] << " http://localhost:8080/events\n"
                      << "  " << argv[0] << " --simple http://localhost:3000/sse\n";
            co_return 0;
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

    // Run client based on mode
    switch (mode) {
        case Mode::demo:
            co_await listen_events(url);
            break;
        case Mode::simple:
            co_await simple_test(url);
            break;
        case Mode::reconnect:
            co_await reconnect_test(url);
            break;
    }

    co_return 0;
}

// Use ELIO_ASYNC_MAIN - handles scheduler creation, execution, and shutdown automatically
ELIO_ASYNC_MAIN(async_main)
