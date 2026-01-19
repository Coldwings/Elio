/// @file http2_client.cpp
/// @brief HTTP/2 Client Example
/// 
/// This example demonstrates how to make HTTP/2 requests using Elio's
/// HTTP/2 client with multiplexed streams over TLS.
///
/// Usage: ./http2_client [url]
/// Default: Fetches https://nghttp2.org/
///
/// Note: HTTP/2 requires HTTPS (TLS with ALPN h2 negotiation).
/// The target server must support HTTP/2.

#include <elio/elio.hpp>
#include <elio/http/http2.hpp>

#include <atomic>
#include <condition_variable>
#include <mutex>

using namespace elio;
using namespace elio::http;
using namespace elio::runtime;

// Completion signaling
std::atomic<bool> g_done{false};
std::mutex g_mutex;
std::condition_variable g_cv;

/// Perform HTTP/2 requests demonstrating various features
coro::task<void> run_client(io::io_context& io_ctx, const std::string& base_url) {
    // Create HTTP/2 client with custom config
    h2_client_config config;
    config.user_agent = "elio-http2-client-example/1.0";
    config.max_concurrent_streams = 100;
    
    h2_client client(io_ctx, config);
    
    ELIO_LOG_INFO("=== HTTP/2 Client Example ===");
    ELIO_LOG_INFO("Base URL: {}", base_url);
    
    // 1. Simple GET request
    ELIO_LOG_INFO("\n--- HTTP/2 GET Request ---");
    {
        auto result = co_await client.get(base_url);
        if (result) {
            auto& resp = *result;
            ELIO_LOG_INFO("Status: {}", static_cast<int>(resp.get_status()));
            ELIO_LOG_INFO("Content-Type: {}", resp.content_type());
            ELIO_LOG_INFO("Body length: {} bytes", resp.body().size());
            
            // Print first 200 chars of body
            auto body = resp.body();
            if (body.size() > 200) {
                ELIO_LOG_INFO("Body (truncated): {}", body.substr(0, 200));
            } else {
                ELIO_LOG_INFO("Body: {}", body);
            }
        } else {
            ELIO_LOG_ERROR("HTTP/2 GET request failed: {}", strerror(errno));
        }
    }
    
    // 2. Multiple requests using same connection (HTTP/2 multiplexing)
    ELIO_LOG_INFO("\n--- HTTP/2 Connection Multiplexing ---");
    {
        ELIO_LOG_INFO("Making multiple requests over single HTTP/2 connection...");
        for (int i = 1; i <= 3; ++i) {
            ELIO_LOG_INFO("Request {}/3...", i);
            auto result = co_await client.get(base_url);
            if (result) {
                ELIO_LOG_INFO("  Status: {} (body: {} bytes)", 
                             static_cast<int>(result->get_status()),
                             result->body().size());
            } else {
                ELIO_LOG_ERROR("  Request {} failed", i);
            }
        }
        ELIO_LOG_INFO("All requests used HTTP/2 multiplexed streams on single connection");
    }
    
    ELIO_LOG_INFO("\n=== HTTP/2 Client Example Complete ===");
    
    // Signal completion
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_done = true;
    }
    g_cv.notify_all();
}

/// Simple one-off HTTP/2 request demonstration
coro::task<void> simple_request(io::io_context& io_ctx, const std::string& url) {
    ELIO_LOG_INFO("Fetching via HTTP/2: {}", url);
    
    // Use convenience function for one-off requests
    auto result = co_await h2_get(io_ctx, url);
    
    if (result) {
        auto& resp = *result;
        ELIO_LOG_INFO("Status: {}", static_cast<int>(resp.get_status()));
        ELIO_LOG_INFO("Content-Type: {}", resp.content_type());
        ELIO_LOG_INFO("Content-Length: {}", resp.body().size());
        
        // Print response headers
        ELIO_LOG_INFO("Response Headers:");
        for (const auto& [name, value] : resp.get_headers()) {
            ELIO_LOG_INFO("  {}: {}", name, value);
        }
        
        // Print response body (truncated if too long)
        auto body = resp.body();
        if (body.size() > 500) {
            ELIO_LOG_INFO("Body (first 500 chars):\n{}", body.substr(0, 500));
        } else {
            ELIO_LOG_INFO("Body:\n{}", body);
        }
    } else {
        ELIO_LOG_ERROR("HTTP/2 request failed: {}", strerror(errno));
        ELIO_LOG_INFO("Note: HTTP/2 requires HTTPS and server support for h2 ALPN.");
    }
    
    // Signal completion
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_done = true;
    }
    g_cv.notify_all();
}

int main(int argc, char* argv[]) {
    std::string url;
    bool full_demo = false;
    
    // Parse arguments
    if (argc > 1) {
        std::string arg = argv[1];
        if (arg == "--demo" || arg == "-d") {
            full_demo = true;
            url = "https://nghttp2.org/";  // nghttp2.org always supports HTTP/2
        } else {
            url = arg;
        }
    } else {
        // Default: run simple demo
        full_demo = false;
        url = "https://nghttp2.org/";
    }
    
    ELIO_LOG_INFO("HTTP/2 Client Example");
    ELIO_LOG_INFO("Using nghttp2 library for HTTP/2 protocol support");
    
    // Create scheduler
    scheduler sched(2);
    sched.set_io_context(&io::default_io_context());
    sched.start();
    
    // Run appropriate mode
    if (full_demo) {
        auto task = run_client(io::default_io_context(), url);
        sched.spawn(task.release());
    } else {
        auto task = simple_request(io::default_io_context(), url);
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
