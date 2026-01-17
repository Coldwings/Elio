/// @file http_server.cpp
/// @brief HTTP Server Example
/// 
/// This example demonstrates how to build an HTTP server using Elio's
/// HTTP module with router-based request handling.
///
/// Usage: ./http_server [port] [--https cert.pem key.pem]
/// Default: HTTP on port 8080

#include <elio/elio.hpp>
#include <elio/http/http.hpp>
#include <elio/tls/tls.hpp>

#include <csignal>
#include <atomic>

using namespace elio;
using namespace elio::http;
using namespace elio::runtime;

// Global flag for graceful shutdown
std::atomic<bool> g_running{true};

void signal_handler(int) {
    g_running = false;
}

// Simple in-memory data store for demo
std::vector<std::string> todos;
std::mutex todos_mutex;

// Handler: GET /
coro::task<response> index_handler([[maybe_unused]] context& ctx) {
    std::string html = R"(
<!DOCTYPE html>
<html>
<head><title>Elio HTTP Server</title></head>
<body>
    <h1>Welcome to Elio HTTP Server</h1>
    <p>This is a simple HTTP server built with the Elio async I/O library.</p>
    <h2>API Endpoints:</h2>
    <ul>
        <li><a href="/api/hello">GET /api/hello</a> - Hello World</li>
        <li><a href="/api/echo">POST /api/echo</a> - Echo request body</li>
        <li><a href="/api/todos">GET /api/todos</a> - List todos</li>
        <li>POST /api/todos - Add a todo</li>
        <li><a href="/api/info">GET /api/info</a> - Server info</li>
    </ul>
</body>
</html>
)";
    co_return response::html(html);
}

// Handler: GET /api/hello
coro::task<response> hello_handler([[maybe_unused]] context& ctx) {
    co_return response::json(R"({"message": "Hello, World!"})");
}

// Handler: POST /api/echo
coro::task<response> echo_handler(context& ctx) {
    auto& req = ctx.req();
    std::string body_str(req.body());
    
    std::string json = R"({"echo": ")" + body_str + R"(", "content_type": ")" + 
                       std::string(req.content_type()) + R"("})";
    co_return response::json(json);
}

// Handler: GET /api/todos
coro::task<response> list_todos_handler([[maybe_unused]] context& ctx) {
    std::lock_guard<std::mutex> lock(todos_mutex);
    
    std::string json = "[";
    for (size_t i = 0; i < todos.size(); ++i) {
        if (i > 0) json += ",";
        json += R"({"id": )" + std::to_string(i) + R"(, "text": ")" + todos[i] + R"("})";
    }
    json += "]";
    
    co_return response::json(json);
}

// Handler: POST /api/todos
coro::task<response> add_todo_handler(context& ctx) {
    auto& req = ctx.req();
    std::string text(req.body());
    
    size_t id;
    {
        std::lock_guard<std::mutex> lock(todos_mutex);
        id = todos.size();
        todos.push_back(text);
    }
    
    std::string json = R"({"id": )" + std::to_string(id) + R"(, "text": ")" + text + R"("})";
    
    response resp(status::created, json, mime::application_json);
    co_return resp;
}

// Handler: GET /api/todos/:id
coro::task<response> get_todo_handler(context& ctx) {
    auto id_str = ctx.param("id");
    size_t id = 0;
    auto [ptr, ec] = std::from_chars(id_str.data(), id_str.data() + id_str.size(), id);
    
    if (ec != std::errc{}) {
        co_return response::bad_request(R"({"error": "Invalid ID"})");
    }
    
    std::lock_guard<std::mutex> lock(todos_mutex);
    if (id >= todos.size()) {
        co_return response::not_found(R"({"error": "Todo not found"})");
    }
    
    std::string json = R"({"id": )" + std::to_string(id) + R"(, "text": ")" + todos[id] + R"("})";
    co_return response::json(json);
}

// Handler: DELETE /api/todos/:id
coro::task<response> delete_todo_handler(context& ctx) {
    auto id_str = ctx.param("id");
    size_t id = 0;
    auto [ptr, ec] = std::from_chars(id_str.data(), id_str.data() + id_str.size(), id);
    
    if (ec != std::errc{}) {
        co_return response::bad_request(R"({"error": "Invalid ID"})");
    }
    
    std::lock_guard<std::mutex> lock(todos_mutex);
    if (id >= todos.size()) {
        co_return response::not_found(R"({"error": "Todo not found"})");
    }
    
    todos.erase(todos.begin() + static_cast<long>(id));
    
    co_return response(status::no_content);
}

// Handler: GET /api/info
coro::task<response> info_handler(context& ctx) {
    std::string json = R"({
        "server": "Elio HTTP Server",
        "version": "1.0.0",
        "client": ")" + std::string(ctx.client_addr()) + R"("
    })";
    co_return response::json(json);
}

// Handler: GET /api/users/:id
coro::task<response> user_handler(context& ctx) {
    auto id = ctx.param("id");
    std::string json = R"({"user_id": ")" + std::string(id) + R"("})";
    co_return response::json(json);
}

int main(int argc, char* argv[]) {
    uint16_t port = 8080;
    bool use_https = false;
    std::string cert_file, key_file;
    
    // Parse arguments
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--https" && i + 2 < argc) {
            use_https = true;
            cert_file = argv[++i];
            key_file = argv[++i];
        } else if (arg[0] != '-') {
            port = static_cast<uint16_t>(std::stoi(arg));
        }
    }
    
    // Setup signal handler
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);
    
    // Create router
    router r;
    
    // Register routes
    r.get("/", index_handler);
    r.get("/api/hello", hello_handler);
    r.post("/api/echo", echo_handler);
    r.get("/api/todos", list_todos_handler);
    r.post("/api/todos", add_todo_handler);
    r.get("/api/todos/:id", get_todo_handler);
    r.del("/api/todos/:id", delete_todo_handler);
    r.get("/api/info", info_handler);
    r.get("/api/users/:id", user_handler);
    
    // Create server
    server srv(std::move(r));
    
    // Set custom 404 handler
    srv.set_not_found_handler([](context& ctx) -> coro::task<response> {
        std::string json = R"({"error": "Not Found", "path": ")" + 
                          std::string(ctx.req().path()) + R"("})";
        auto resp = response(status::not_found, json, mime::application_json);
        co_return resp;
    });
    
    // Create scheduler
    scheduler sched(4);
    sched.set_io_context(&io::default_io_context());
    sched.start();
    
    // Start server
    if (use_https) {
        try {
            auto tls_ctx = tls::tls_context::make_server(cert_file, key_file);
            
            auto server_task = srv.listen_tls(
                net::ipv4_address(port), 
                io::default_io_context(), 
                sched,
                tls_ctx
            );
            sched.spawn(server_task.release());
            
            ELIO_LOG_INFO("HTTPS server started on port {}", port);
        } catch (const std::exception& e) {
            ELIO_LOG_ERROR("Failed to start HTTPS server: {}", e.what());
            return 1;
        }
    } else {
        auto server_task = srv.listen(
            net::ipv4_address(port), 
            io::default_io_context(), 
            sched
        );
        sched.spawn(server_task.release());
        
        ELIO_LOG_INFO("HTTP server started on port {}", port);
    }
    
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
