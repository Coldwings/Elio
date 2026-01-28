/// @file sse_server.cpp
/// @brief Server-Sent Events (SSE) Server Example
///
/// This example demonstrates how to build an SSE server using Elio's
/// HTTP server with SSE event streaming.
///
/// Usage: ./sse_server [port]
/// Default: Port 8080
///
/// Features demonstrated:
/// - SSE event streaming
/// - Multiple event types
/// - Event IDs for reconnection
/// - Keep-alive comments
/// - Integration with HTTP server

#include <elio/elio.hpp>
#include <elio/http/http.hpp>
#include <elio/http/sse.hpp>

#include <atomic>
#include <chrono>

using namespace elio;
using namespace elio::http;
using namespace elio::http::sse;

// Global counter for events
std::atomic<uint64_t> g_event_counter{0};

// Running flag for SSE streams
std::atomic<bool> g_sse_active{true};

/// SSE event stream handler - sends periodic events
coro::task<void> event_stream(net::tcp_stream& stream) {
    // Create SSE connection
    sse_connection conn(&stream);

    ELIO_LOG_INFO("SSE client connected");

    // Send initial retry interval
    co_await conn.send_retry(3000);  // 3 seconds

    // Send events until client disconnects or server stops
    uint64_t local_counter = 0;
    while (conn.is_active() && g_sse_active) {
        ++local_counter;
        uint64_t event_id = ++g_event_counter;

        // Send different event types
        switch (local_counter % 4) {
            case 0: {
                // Simple data event
                event evt = event::with_id(
                    std::to_string(event_id),
                    "Counter: " + std::to_string(local_counter)
                );
                co_await conn.send(evt);
                break;
            }

            case 1: {
                // Typed event (e.g., for different data streams)
                event evt = event::full(
                    std::to_string(event_id),
                    "heartbeat",
                    R"({"alive":true,"timestamp":)" +
                        std::to_string(std::chrono::system_clock::now()
                            .time_since_epoch().count()) + "}",
                    -1
                );
                co_await conn.send(evt);
                break;
            }

            case 2: {
                // JSON data event
                std::string json = R"({"type":"update","count":)" +
                                   std::to_string(local_counter) +
                                   R"(,"id":")" + std::to_string(event_id) + R"("})";
                co_await conn.send_event("data", json);
                break;
            }

            case 3: {
                // Keep-alive comment (doesn't trigger client event)
                co_await conn.send_comment("keep-alive");
                break;
            }
        }

        // Wait before sending next event
        co_await time::sleep_for(std::chrono::seconds(1));
    }

    ELIO_LOG_INFO("SSE client disconnected");
}

/// Custom HTTP server that handles SSE endpoints
class sse_http_server {
public:
    explicit sse_http_server(router r, server_config config = {})
        : router_(std::move(r)), config_(config) {}

    coro::task<void> listen(const net::socket_address& addr) {
        auto* sched = runtime::scheduler::current();
        if (!sched) {
            ELIO_LOG_ERROR("SSE server must be started from within a scheduler context");
            co_return;
        }

        auto listener_result = net::tcp_listener::bind(addr);
        if (!listener_result) {
            ELIO_LOG_ERROR("Failed to bind SSE server: {}", strerror(errno));
            co_return;
        }

        ELIO_LOG_INFO("SSE server listening on {}", addr.to_string());

        auto& listener = *listener_result;
        running_ = true;

        while (running_) {
            auto stream_result = co_await listener.accept();
            if (!stream_result) {
                if (running_) {
                    ELIO_LOG_ERROR("Accept error: {}", strerror(errno));
                }
                continue;
            }

            auto handler = handle_connection(std::move(*stream_result));
            sched->spawn(handler.release());
        }
    }

    void stop() {
        running_ = false;
        g_sse_active = false;
    }

private:
    coro::task<void> handle_connection(net::tcp_stream stream) {
        std::vector<char> buffer(config_.read_buffer_size);
        request_parser parser;

        // Read HTTP request
        while (!parser.is_complete() && !parser.has_error()) {
            auto result = co_await stream.read(buffer.data(), buffer.size());
            if (result.result <= 0) co_return;

            auto [parse_result, consumed] = parser.parse(
                std::string_view(buffer.data(), result.result));
            if (parse_result == parse_result::error) co_return;
        }

        if (parser.has_error()) co_return;

        auto req = request::from_parser(parser);

        // Check if this is an SSE request
        if (req.path() == "/events" || req.path() == "/sse") {
            // Send SSE headers
            std::string headers =
                "HTTP/1.1 200 OK\r\n"
                "Content-Type: text/event-stream\r\n"
                "Cache-Control: no-cache\r\n"
                "Connection: keep-alive\r\n"
                "Access-Control-Allow-Origin: *\r\n"
                "\r\n";

            auto write_result = co_await stream.write(headers.data(), headers.size());
            if (write_result.result <= 0) co_return;

            // Get Last-Event-ID if present
            auto last_id = req.header("Last-Event-ID");
            if (!last_id.empty()) {
                ELIO_LOG_INFO("Client reconnecting with Last-Event-ID: {}", last_id);
            }

            // Handle SSE stream
            co_await event_stream(stream);
        } else {
            // Handle as regular HTTP
            auto peer = stream.peer_address();
            std::string client_addr = peer ? peer->to_string() : "unknown";
            context ctx(std::move(req), client_addr);

            std::unordered_map<std::string, std::string> params;
            auto* route = router_.find_route(ctx.req().get_method(),
                                              ctx.req().path(), params);

            response resp;
            if (route) {
                for (const auto& [name, value] : params) {
                    ctx.set_param(name, value);
                }
                try {
                    resp = co_await route->handler(ctx);
                } catch (const std::exception& e) {
                    resp = response::internal_error();
                }
            } else {
                resp = response::not_found();
            }

            auto data = resp.serialize();
            co_await stream.write(data.data(), data.size());
        }
    }

    router router_;
    server_config config_;
    std::atomic<bool> running_{false};
};

// HTTP handler: Serve test page
coro::task<response> index_handler([[maybe_unused]] context& ctx) {
    std::string html = R"HTML(
<!DOCTYPE html>
<html>
<head>
    <title>Elio SSE Test</title>
    <style>
        body { font-family: Arial, sans-serif; margin: 20px; }
        #log { border: 1px solid #ccc; padding: 10px; height: 400px; overflow-y: scroll; }
        .event { margin: 5px 0; padding: 5px; background: #f0f0f0; }
        .heartbeat { background: #e0ffe0; }
        .data { background: #e0e0ff; }
        .error { background: #ffe0e0; }
        button { margin: 5px; padding: 10px 20px; }
    </style>
</head>
<body>
    <h1>Elio Server-Sent Events Test</h1>

    <div>
        <button onclick="sseConnect()">Connect</button>
        <button onclick="sseDisconnect()">Disconnect</button>
        <button onclick="clearLog()">Clear Log</button>
        <span id="status" style="margin-left: 20px;">Disconnected</span>
    </div>

    <h3>Events:</h3>
    <div id="log"></div>

    <script>
        var es = null;

        function logMsg(msg, className) {
            var div = document.getElementById("log");
            var entry = document.createElement("div");
            entry.className = "event " + (className || "");
            entry.textContent = new Date().toLocaleTimeString() + " - " + msg;
            div.appendChild(entry);
            div.scrollTop = div.scrollHeight;
        }

        function setStatus(status) {
            document.getElementById("status").textContent = status;
        }

        function sseConnect() {
            if (es) {
                es.close();
            }

            logMsg("Connecting to /events...");
            setStatus("Connecting...");

            es = new EventSource("/events");

            es.onopen = function() {
                logMsg("Connected!");
                setStatus("Connected");
            };

            es.onmessage = function(e) {
                logMsg("Message: " + e.data);
            };

            es.addEventListener("heartbeat", function(e) {
                logMsg("Heartbeat: " + e.data, "heartbeat");
            });

            es.addEventListener("data", function(e) {
                logMsg("Data: " + e.data, "data");
            });

            es.onerror = function(e) {
                if (es.readyState === EventSource.CLOSED) {
                    logMsg("Connection closed", "error");
                    setStatus("Disconnected");
                } else {
                    logMsg("Error - reconnecting...", "error");
                    setStatus("Reconnecting...");
                }
            };
        }

        function sseDisconnect() {
            if (es) {
                es.close();
                es = null;
                logMsg("Disconnected");
                setStatus("Disconnected");
            }
        }

        function clearLog() {
            document.getElementById("log").innerHTML = "";
        }
    </script>
</body>
</html>
)HTML";
    co_return response::html(html);
}

// HTTP handler: Server info
coro::task<response> info_handler([[maybe_unused]] context& ctx) {
    std::string json = R"({
        "server": "Elio SSE Server",
        "version": "1.0.0",
        "total_events": )" + std::to_string(g_event_counter.load()) + R"(,
        "endpoints": {
            "events": "/events",
            "sse": "/sse"
        }
    })";

    co_return response::json(json);
}

/// Async main - uses ELIO_ASYNC_MAIN with elio::serve() for clean server lifecycle
coro::task<int> async_main(int argc, char* argv[]) {
    uint16_t port = 8080;

    // Parse arguments
    if (argc > 1) {
        std::string arg = argv[1];
        if (arg == "-h" || arg == "--help") {
            ELIO_LOG_INFO("Usage: {} [port]", argv[0]);
            ELIO_LOG_INFO("Default: Port 8080");
            co_return 0;
        }
        port = static_cast<uint16_t>(std::stoi(arg));
    }

    // Create router
    router r;
    r.get("/", index_handler);
    r.get("/info", info_handler);

    // Create SSE-enabled server
    server_config config;
    config.enable_logging = true;

    sse_http_server srv(std::move(r), config);

    auto bind_addr = net::socket_address(net::ipv4_address(port));

    ELIO_LOG_INFO("SSE server starting on port {}", port);
    ELIO_LOG_INFO("Open http://localhost:{} in your browser", port);
    ELIO_LOG_INFO("SSE endpoint: http://localhost:{}/events", port);
    ELIO_LOG_INFO("Press Ctrl+C to stop");

    // Start server and wait for shutdown signal
    // elio::serve() handles signal waiting and graceful shutdown automatically
    co_await elio::serve(srv, srv.listen(bind_addr));

    co_return 0;
}

// Use ELIO_ASYNC_MAIN - handles scheduler creation, execution, and shutdown automatically
ELIO_ASYNC_MAIN(async_main)
