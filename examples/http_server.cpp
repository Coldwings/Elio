/// @file http_server.cpp
/// @brief HTTP Server Example
///
/// This example demonstrates how to build an HTTP server using Elio's
/// HTTP module with router-based request handling.
///
/// Supports both IPv4 and IPv6:
///   - Default: Dual-stack (IPv6 socket accepting both IPv4 and IPv6)
///   - Use -4 for IPv4-only mode
///   - Use -6 for IPv6-only mode
///   - Use -b <addr> to bind to a specific address
///
/// Usage: ./http_server [options] [port]
///   -4               IPv4 only
///   -6               IPv6 only
///   -b <addr>        Bind to specific address
///   --https cert key Enable HTTPS with certificate and key files
/// Default: HTTP on port 8080 (dual-stack)

#include <elio/elio.hpp>
#include <elio/http/http.hpp>
#include <elio/tls/tls.hpp>

#include <iostream>

using namespace elio;
using namespace elio::http;

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
    std::string body(req.body());

    std::string json = R"({"echo": ")" + body + R"(", "length": )" +
                      std::to_string(body.size()) + "}";
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

    if (text.empty()) {
        co_return response::bad_request("Todo text is required");
    }

    size_t id;
    {
        std::lock_guard<std::mutex> lock(todos_mutex);
        id = todos.size();
        todos.push_back(text);
    }

    std::string json = R"({"id": )" + std::to_string(id) + R"(, "text": ")" + text + R"("})";
    co_return response(status::created, json, mime::application_json);
}

// Handler: GET /api/todos/:id
coro::task<response> get_todo_handler(context& ctx) {
    std::string id_str(ctx.param("id"));
    size_t id = std::stoul(id_str);

    std::lock_guard<std::mutex> lock(todos_mutex);
    if (id >= todos.size()) {
        co_return response::not_found();
    }

    std::string json = R"({"id": )" + std::to_string(id) +
                      R"(, "text": ")" + todos[id] + R"("})";
    co_return response::json(json);
}

// Handler: DELETE /api/todos/:id
coro::task<response> delete_todo_handler(context& ctx) {
    std::string id_str(ctx.param("id"));
    size_t id = std::stoul(id_str);

    std::lock_guard<std::mutex> lock(todos_mutex);
    if (id >= todos.size()) {
        co_return response::not_found();
    }

    todos.erase(todos.begin() + static_cast<long>(id));
    co_return response::json(R"({"deleted": true})");
}

// Handler: GET /api/info
coro::task<response> info_handler([[maybe_unused]] context& ctx) {
    std::string json = R"({
        "server": "Elio HTTP Server",
        "version": ")" + std::string(elio::version()) + R"(",
        "features": ["async", "coroutines", "http/1.1", "keep-alive"]
    })";
    co_return response::json(json);
}

// Handler with path parameter: GET /api/users/:id
coro::task<response> user_handler(context& ctx) {
    std::string_view id = ctx.param("id");
    std::string json = R"({"user_id": ")" + std::string(id) + R"(", "name": "User )" +
                      std::string(id) + R"("})";
    co_return response::json(json);
}

/// Async main - uses ELIO_ASYNC_MAIN with elio::serve() for clean server lifecycle
coro::task<int> async_main(int argc, char* argv[]) {
    uint16_t port = 8080;
    bool use_https = false;
    std::string cert_file, key_file;
    std::string bind_address;
    bool ipv4_only = false;
    bool ipv6_only = false;

    // Parse arguments
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--https" && i + 2 < argc) {
            use_https = true;
            cert_file = argv[++i];
            key_file = argv[++i];
        } else if (arg == "-4") {
            ipv4_only = true;
        } else if (arg == "-6") {
            ipv6_only = true;
        } else if (arg == "-b" && i + 1 < argc) {
            bind_address = argv[++i];
        } else if (arg == "-h" || arg == "--help") {
            std::cout << "Usage: " << argv[0] << " [options] [port]\n"
                      << "Options:\n"
                      << "  -4               IPv4 only\n"
                      << "  -6               IPv6 only\n"
                      << "  -b <addr>        Bind to specific address\n"
                      << "  --https cert key Enable HTTPS\n"
                      << "  -h, --help       Show this help\n"
                      << "Default: HTTP on port 8080 (dual-stack)\n";
            co_return 0;
        } else if (arg[0] != '-') {
            port = static_cast<uint16_t>(std::stoi(arg));
        }
    }

    // Determine bind address
    net::socket_address bind_addr;
    net::tcp_options opts;

    if (!bind_address.empty()) {
        bind_addr = net::socket_address(bind_address, port);
    } else if (ipv4_only) {
        bind_addr = net::socket_address(net::ipv4_address(port));
    } else {
        bind_addr = net::socket_address(net::ipv6_address(port));
        opts.ipv6_only = ipv6_only;
    }

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
        co_return response(status::not_found, json, mime::application_json);
    });

    ELIO_LOG_INFO("Press Ctrl+C to stop");

    // Start server and wait for shutdown signal
    // elio::serve() handles signal waiting and graceful shutdown automatically
    if (use_https) {
        try {
            auto tls_ctx = tls::tls_context::make_server(cert_file, key_file);
            ELIO_LOG_INFO("Starting HTTPS server on {}", bind_addr.to_string());
            co_await elio::serve(srv, srv.listen_tls(bind_addr, tls_ctx, opts));
        } catch (const std::exception& e) {
            ELIO_LOG_ERROR("Failed to start HTTPS server: {}", e.what());
            co_return 1;
        }
    } else {
        ELIO_LOG_INFO("Starting HTTP server on {}", bind_addr.to_string());
        co_await elio::serve(srv, srv.listen(bind_addr, opts));
    }

    co_return 0;
}

// Use ELIO_ASYNC_MAIN - handles scheduler creation, execution, and shutdown automatically
ELIO_ASYNC_MAIN(async_main)
