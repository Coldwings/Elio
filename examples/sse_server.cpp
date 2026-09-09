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
#include <cerrno>
#include <string>

using namespace elio;
using namespace elio::http;
using namespace elio::http::sse;

// Global counter for this demo; Last-Event-ID is logged, not replayed.
std::atomic<uint64_t> g_event_counter{0};

/// The server owns this producer and its event_writer until completion.
coro::task<send_result> event_stream(event_writer& out, coro::cancel_token token) {
    ELIO_LOG_INFO("SSE client connected");
    uint64_t local_counter = 0;
    while (!token.is_cancelled()) {
        ++local_counter;
        const auto event_id = ++g_event_counter;
        const auto id = std::to_string(event_id);
        std::string data;
        send_result sent;
        switch (local_counter % 4) {
            case 0:
                data = "Counter: " + std::to_string(local_counter);
                sent = co_await out.send_event({id, {}, data}, token);
                break;
            case 1:
                data = R"({"alive":true,"timestamp":)" +
                    std::to_string(std::chrono::system_clock::now()
                        .time_since_epoch().count()) + "}";
                // Advertise the initial reconnect interval on the first event.
                sent = co_await out.send_event(
                    {id, "heartbeat", data, local_counter == 1 ? 3000 : -1}, token);
                break;
            case 2:
                data = R"({"type":"update","count":)" +
                    std::to_string(local_counter) + R"(,"id":")" + id + R"("})";
                sent = co_await out.send_event({{}, "data", data}, token);
                break;
            case 3:
                sent = co_await out.send_comment("keep-alive", token);
                break;
        }
        // id/data remain alive through the await; no detached writer or raw
        // transport access is needed, and a failed send is never replayed.
        if (!sent.success()) co_return sent;
        if (co_await time::sleep_for(std::chrono::seconds(1), token) ==
            coro::cancel_result::cancelled) {
            co_return send_result{send_errc::cancelled, ECANCELED};
        }
    }
    co_return send_result{send_errc::cancelled, ECANCELED};
}

streaming_response events_handler(context& ctx) {
    const auto last_id = ctx.req().header("Last-Event-ID");
    if (!last_id.empty()) {
        ELIO_LOG_INFO("Client reconnecting with Last-Event-ID: {}", last_id);
    }
    return make_streaming_response(event_stream);
}

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

/// Async main - run from main() after shutdown signals are masked for signalfd
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
    r.get("/events", events_handler);
    r.get("/sse", events_handler);

    // Create SSE-enabled server
    server_config config;
    config.enable_logging = true;

    http::server srv(std::move(r), config);

    auto bind_addr = net::socket_address(net::ipv4_address(port));

    ELIO_LOG_INFO("SSE server starting on port {}", port);
    ELIO_LOG_INFO("Open http://localhost:{} in your browser", port);
    ELIO_LOG_INFO("SSE endpoint: http://localhost:{}/events", port);
    ELIO_LOG_INFO("Press Ctrl+C to stop");

    // Start server and wait for shutdown signal
    // serve() requests stop, joins the listener, and drains active sessions.
    // srv and its route-owned producers stay alive through cancellation cleanup.
    co_await elio::serve(srv, [&]() { return srv.listen(bind_addr); });

    co_return 0;
}

int main(int argc, char* argv[]) {
    elio::signal::signal_set shutdown_signals(elio::default_shutdown_signals);
    shutdown_signals.block_all_threads();
    return elio::run(async_main, argc, argv);
}
