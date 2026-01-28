/// @file http_client.cpp
/// @brief HTTP Client Example
/// 
/// This example demonstrates how to make HTTP requests using Elio's
/// HTTP client with connection pooling and TLS support.
///
/// Usage: ./http_client [url]
/// Default: Fetches https://httpbin.org/get

#include <elio/elio.hpp>
#include <elio/http/http.hpp>
#include <elio/tls/tls.hpp>

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

/// Perform multiple HTTP requests demonstrating various features
coro::task<void> run_client(io::io_context& io_ctx, const std::string& base_url) {
    // Create client with custom config
    client_config config;
    config.user_agent = "elio-http-client-example/1.0";
    config.follow_redirects = true;
    config.max_redirects = 5;
    
    client c(io_ctx, config);
    
    ELIO_LOG_INFO("=== HTTP Client Example ===");
    ELIO_LOG_INFO("Base URL: {}", base_url);
    
    // 1. Simple GET request
    ELIO_LOG_INFO("\n--- GET Request ---");
    {
        auto result = co_await c.get(base_url + "/get");
        if (result) {
            auto& resp = *result;
            ELIO_LOG_INFO("Status: {} {}", resp.status_code(), status_reason(resp.get_status()));
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
            ELIO_LOG_ERROR("GET request failed: {}", strerror(errno));
        }
    }
    
    // 2. POST request with JSON
    ELIO_LOG_INFO("\n--- POST Request (JSON) ---");
    {
        std::string json_body = R"({"name": "Elio", "version": "1.0", "async": true})";
        auto result = co_await c.post(base_url + "/post", json_body, mime::application_json);
        if (result) {
            auto& resp = *result;
            ELIO_LOG_INFO("Status: {} {}", resp.status_code(), status_reason(resp.get_status()));
            
            auto body = resp.body();
            if (body.size() > 300) {
                ELIO_LOG_INFO("Body (truncated): {}", body.substr(0, 300));
            } else {
                ELIO_LOG_INFO("Body: {}", body);
            }
        } else {
            ELIO_LOG_ERROR("POST request failed: {}", strerror(errno));
        }
    }
    
    // 3. POST with form data
    ELIO_LOG_INFO("\n--- POST Request (Form) ---");
    {
        std::string form_body = "username=elio&password=secret&remember=true";
        auto result = co_await c.post(base_url + "/post", form_body, mime::application_form_urlencoded);
        if (result) {
            ELIO_LOG_INFO("Status: {} {}", result->status_code(), status_reason(result->get_status()));
        } else {
            ELIO_LOG_ERROR("Form POST failed: {}", strerror(errno));
        }
    }
    
    // 4. Custom headers
    ELIO_LOG_INFO("\n--- Custom Headers ---");
    {
        request req(method::GET, "/headers");
        req.set_host("httpbin.org");
        req.set_header("X-Custom-Header", "elio-test-value");
        req.set_header("Accept", "application/json");
        
        url target;
        target.scheme = "https";
        target.host = "httpbin.org";
        target.path = "/headers";
        
        auto result = co_await c.send(req, target);
        if (result) {
            ELIO_LOG_INFO("Status: {} {}", result->status_code(), status_reason(result->get_status()));
            auto body = result->body();
            if (body.size() > 300) {
                ELIO_LOG_INFO("Body (truncated): {}", body.substr(0, 300));
            } else {
                ELIO_LOG_INFO("Body: {}", body);
            }
        } else {
            ELIO_LOG_ERROR("Custom headers request failed: {}", strerror(errno));
        }
    }
    
    // 5. Connection reuse (multiple requests to same host)
    ELIO_LOG_INFO("\n--- Connection Reuse (Keep-Alive) ---");
    {
        for (int i = 1; i <= 3; ++i) {
            ELIO_LOG_INFO("Request {}/3...", i);
            auto result = co_await c.get(base_url + "/get?seq=" + std::to_string(i));
            if (result) {
                ELIO_LOG_INFO("  Status: {}", result->status_code());
            } else {
                ELIO_LOG_ERROR("  Request {} failed", i);
            }
        }
        ELIO_LOG_INFO("All requests used keep-alive connection pooling");
    }
    
    // 6. Redirect following
    ELIO_LOG_INFO("\n--- Redirect Following ---");
    {
        // httpbin.org/redirect/n redirects n times
        auto result = co_await c.get(base_url + "/redirect/2");
        if (result) {
            ELIO_LOG_INFO("Final status: {} (followed redirects automatically)", result->status_code());
        } else {
            ELIO_LOG_ERROR("Redirect request failed: {}", strerror(errno));
        }
    }
    
    // 7. Status codes
    ELIO_LOG_INFO("\n--- Various Status Codes ---");
    {
        std::vector<int> codes = {200, 201, 404, 500};
        for (int code : codes) {
            auto result = co_await c.get(base_url + "/status/" + std::to_string(code));
            if (result) {
                ELIO_LOG_INFO("Requested {}: got {} {}", 
                             code, result->status_code(), status_reason(result->get_status()));
            } else {
                ELIO_LOG_ERROR("Request for status {} failed", code);
            }
        }
    }
    
    // 8. HEAD request
    ELIO_LOG_INFO("\n--- HEAD Request ---");
    {
        auto result = co_await c.head(base_url + "/get");
        if (result) {
            ELIO_LOG_INFO("Status: {}", result->status_code());
            ELIO_LOG_INFO("Content-Type: {}", result->header("Content-Type"));
            ELIO_LOG_INFO("Body size: {} (should be 0 for HEAD)", result->body().size());
        } else {
            ELIO_LOG_ERROR("HEAD request failed: {}", strerror(errno));
        }
    }
    
    ELIO_LOG_INFO("\n=== HTTP Client Example Complete ===");
    
    // Signal completion
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_done = true;
    }
    g_cv.notify_all();
}

/// Simple one-off request demonstration
coro::task<void> simple_request(io::io_context& io_ctx, const std::string& url) {
    ELIO_LOG_INFO("Fetching: {}", url);
    
    // Use convenience function for one-off requests
    auto result = co_await http::get(io_ctx, url);
    
    if (result) {
        auto& resp = *result;
        ELIO_LOG_INFO("Status: {} {}", resp.status_code(), status_reason(resp.get_status()));
        ELIO_LOG_INFO("Content-Type: {}", resp.content_type());
        ELIO_LOG_INFO("Content-Length: {}", resp.body().size());
        
        // Print response body (truncated if too long)
        auto body = resp.body();
        if (body.size() > 500) {
            ELIO_LOG_INFO("Body (first 500 chars):\n{}", body.substr(0, 500));
        } else {
            ELIO_LOG_INFO("Body:\n{}", body);
        }
    } else {
        ELIO_LOG_ERROR("Request failed: {}", strerror(errno));
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
            url = "https://httpbin.org";
        } else {
            url = arg;
        }
    } else {
        // Default: run full demo
        full_demo = true;
        url = "https://httpbin.org";
    }
    
    // Create scheduler
    scheduler sched(2);
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
