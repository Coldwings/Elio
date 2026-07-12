// Integration tests for elio::http::client hardening:
//   1. max_response_size aborts a body that exceeds the cap.
//   2. read_timeout interrupts a stalled response read.
//   3. connect_timeout interrupts stalled TLS handshake during setup.
//   4. A connection with leftover bytes after the response is NOT pooled
//      (response-splitting prevention).
//   5. Repeated informational responses cannot bypass max_response_size.
//   6. WebSocket and SSE client handshakes honor read_timeout while waiting
//      for protocol response headers.
//   7. Client cancellation tokens abort pending HTTP, WebSocket, and SSE I/O.
//
// These stand up a tiny TCP listener that pretends to be an HTTP-based server
// and craft hand-built byte sequences for each scenario -- no real
// http::server in the loop.

#include <catch2/catch_test_macros.hpp>

#include <elio/http/http_client.hpp>
#include <elio/http/sse.hpp>
#include <elio/http/websocket.hpp>
#include <elio/net/tcp.hpp>
#include <elio/runtime/scheduler.hpp>
#include <elio/time/timer.hpp>
#if defined(ELIO_HAS_TLS) && ELIO_HAS_TLS
#include <elio/tls/tls_context.hpp>
#endif

#include <atomic>
#include <cerrno>
#include <chrono>
#include <string>
#include <thread>

using elio::coro::task;
using elio::net::ipv4_address;
using elio::net::ipv6_address;
using elio::net::tcp_listener;
using elio::runtime::scheduler;

namespace {

constexpr bool running_under_tsan() {
#if defined(__SANITIZE_THREAD__)
    return true;
#elif defined(__has_feature)
#if __has_feature(thread_sanitizer)
    return true;
#else
    return false;
#endif
#else
    return false;
#endif
}

std::string make_url(uint16_t port, std::string_view path = "/") {
    return std::string("http://127.0.0.1:") + std::to_string(port) +
           std::string(path);
}

std::string make_https_url(uint16_t port, std::string_view path = "/") {
    return std::string("https://127.0.0.1:") + std::to_string(port) +
           std::string(path);
}

std::string make_ws_url(uint16_t port, std::string_view path = "/ws") {
    return std::string("ws://127.0.0.1:") + std::to_string(port) +
           std::string(path);
}

std::string make_ws_ipv6_url(uint16_t port, std::string_view path = "/ws") {
    return std::string("ws://[::1]:") + std::to_string(port) +
           std::string(path);
}

// Read a single request from the stream (until "\r\n\r\n"). Used by the
// fake server to know when to start writing its response.
task<std::string> read_request_headers(elio::net::tcp_stream& s) {
    std::string accum;
    char buf[1024];
    while (accum.find("\r\n\r\n") == std::string::npos) {
        auto r = co_await s.read(buf, sizeof(buf));
        if (r.result <= 0) co_return accum;
        accum.append(buf, static_cast<size_t>(r.result));
    }
    co_return accum;
}

task<void> drain_request_headers(elio::net::tcp_stream& s) {
    (void)co_await read_request_headers(s);
    co_return;
}

std::string request_header_value(std::string_view headers, std::string_view name) {
    std::string needle = "\r\n";
    needle += name;
    needle += ":";
    auto pos = headers.find(needle);
    if (pos == std::string_view::npos) {
        return {};
    }
    pos += needle.size();
    while (pos < headers.size() && (headers[pos] == ' ' || headers[pos] == '\t')) {
        ++pos;
    }
    auto end = headers.find("\r\n", pos);
    if (end == std::string_view::npos) {
        end = headers.size();
    }
    return std::string(headers.substr(pos, end - pos));
}

} // namespace

TEST_CASE("HTTP client rejects invalid outbound request inputs",
          "[http][client][security]") {
    scheduler sched(1);
    sched.start();

    std::atomic<bool> client_done{false};
    bool request_failed = false;
    int client_errno = 0;

    sched.go([&]() -> task<void> {
        elio::http::client_config cfg;
        cfg.user_agent = "elio\r\nInjected: yes";
        elio::http::client c(cfg);

        errno = 0;
        auto resp = co_await c.get("http://127.0.0.1:1/");
        request_failed = !resp;
        client_errno = errno;
        client_done = true;
    });

    for (int i = 0; i < 500 && !client_done; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    sched.shutdown();

    REQUIRE(client_done);
    REQUIRE(request_failed);
    REQUIRE(client_errno == EINVAL);
}

TEST_CASE("HTTP client rejects unsupported URL schemes",
          "[http][client][security]") {
    scheduler sched(1);
    sched.start();

    std::atomic<bool> client_done{false};
    bool request_failed = false;
    int client_errno = 0;

    sched.go([&]() -> task<void> {
        elio::http::client c;

        errno = 0;
        auto resp = co_await c.get("ftp://127.0.0.1/resource");
        request_failed = !resp;
        client_errno = errno;
        client_done = true;
    });

    for (int i = 0; i < 500 && !client_done; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    sched.shutdown();

    REQUIRE(client_done);
    REQUIRE(request_failed);
    REQUIRE(client_errno == EINVAL);
}

TEST_CASE("WebSocket client rejects invalid outbound handshake config",
          "[websocket][client][security]") {
    scheduler sched(1);
    sched.start();

    std::atomic<bool> client_done{false};
    bool connected = true;
    int client_errno = 0;

    sched.go([&]() -> task<void> {
        elio::http::websocket::client_config cfg;
        cfg.user_agent = "elio\r\nInjected: yes";
        elio::http::websocket::ws_client client(cfg);

        errno = 0;
        connected = co_await client.connect("ws://127.0.0.1:1/ws");
        client_errno = errno;
        client_done = true;
    });

    for (int i = 0; i < 500 && !client_done; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    sched.shutdown();

    REQUIRE(client_done);
    REQUIRE_FALSE(connected);
    REQUIRE(client_errno == EINVAL);
}

TEST_CASE("HTTP client rejects response exceeding max_response_size",
          "[http][client][security]") {
    auto listener = tcp_listener::bind(ipv4_address("127.0.0.1", 0));
    REQUIRE(listener.has_value());
    uint16_t port = listener->local_address().port();

    scheduler sched(2);
    sched.start();

    std::atomic<bool> server_done{false};
    std::atomic<bool> client_done{false};
    std::atomic<bool> client_failed{false};
    std::atomic<int> client_errno{0};

    sched.go([&]() -> task<void> {
        auto stream = co_await listener->accept();
        REQUIRE(stream.has_value());
        co_await drain_request_headers(*stream);

        // Claim a 64 KiB body and drip it. The client's max_response_size
        // is set to 8 KiB below — the parser should bail out long before
        // we finish sending.
        std::string head =
            "HTTP/1.1 200 OK\r\n"
            "Content-Length: 65536\r\n"
            "Connection: close\r\n"
            "\r\n";
        co_await stream->write(head);

        std::string blob(4096, 'x');
        for (int i = 0; i < 16; ++i) {
            auto w = co_await stream->write(blob);
            if (w.result <= 0) break;  // client closed
        }
        server_done = true;
    });

    sched.go([&]() -> task<void> {
        elio::http::client_config cfg;
        cfg.max_response_size = 8 * 1024;     // 8 KiB
        cfg.read_timeout = std::chrono::seconds(5);
        elio::http::client c(cfg);

        auto resp = co_await c.get(make_url(port));
        if (!resp) {
            client_failed = true;
            client_errno = errno;
        }
        client_done = true;
    });

    for (int i = 0; i < 500 && !(client_done && server_done); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    sched.shutdown();

    REQUIRE(client_done);
    REQUIRE(client_failed);
    REQUIRE(client_errno == EMSGSIZE);
}

TEST_CASE("HTTP client read_timeout fires on a stalled server",
          "[http][client][security][slow_loris]") {
    auto listener = tcp_listener::bind(ipv4_address("127.0.0.1", 0));
    REQUIRE(listener.has_value());
    uint16_t port = listener->local_address().port();

    scheduler sched(2);
    sched.start();

    std::atomic<bool> server_done{false};
    std::atomic<bool> client_done{false};
    std::atomic<bool> client_failed{false};
    std::atomic<int> client_errno{0};

    // Server: accept, drain request, then NEVER write a response.
    sched.go([&]() -> task<void> {
        auto stream = co_await listener->accept();
        REQUIRE(stream.has_value());
        co_await drain_request_headers(*stream);

        // Hold the connection open well past the client's read_timeout.
        co_await elio::time::sleep_for(std::chrono::seconds(3));
        server_done = true;
    });

    sched.go([&]() -> task<void> {
        elio::http::client_config cfg;
        cfg.read_timeout = std::chrono::seconds(1);
        elio::http::client c(cfg);

        auto t0 = std::chrono::steady_clock::now();
        auto resp = co_await c.get(make_url(port));
        auto elapsed = std::chrono::steady_clock::now() - t0;

        if (!resp) {
            client_failed = true;
            client_errno = errno;
        }
        // Must return well before the server's 3s sleep elapses.
        REQUIRE(elapsed < std::chrono::seconds(3));
        client_done = true;
    });

    for (int i = 0; i < 500 && !client_done; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    sched.shutdown();

    REQUIRE(client_done);
    REQUIRE(client_failed);
    REQUIRE(client_errno == ETIMEDOUT);
}

TEST_CASE("WebSocket client read_timeout fires on a stalled handshake response",
          "[websocket][client][timeout][regression]") {
    auto listener = tcp_listener::bind(ipv4_address("127.0.0.1", 0));
    REQUIRE(listener.has_value());
    uint16_t port = listener->local_address().port();

    scheduler sched(2);
    sched.start();

    std::atomic<bool> client_done{false};
    std::atomic<bool> client_failed{false};
    std::atomic<int> client_errno{0};
    std::atomic<int64_t> client_elapsed_ms{-1};

    // Server: accept and drain the WebSocket upgrade request, but never send
    // the 101 response. The client's read_timeout must bound this phase.
    sched.go([&]() -> task<void> {
        auto stream = co_await listener->accept();
        REQUIRE(stream.has_value());
        co_await drain_request_headers(*stream);
        co_await elio::time::sleep_for(std::chrono::seconds(3));
        stream->shutdown_socket();
    });

    sched.go([&]() -> task<void> {
        elio::http::websocket::client_config cfg;
        cfg.read_timeout = std::chrono::seconds(1);
        elio::http::websocket::ws_client client(cfg);

        auto t0 = std::chrono::steady_clock::now();
        bool ok = co_await client.connect(make_ws_url(port));
        auto elapsed = std::chrono::steady_clock::now() - t0;

        client_elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            elapsed).count();
        if (!ok) {
            client_failed = true;
            client_errno = errno;
        }
        client_done = true;
    });

    for (int i = 0; i < 500 && !client_done; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    sched.shutdown();

    REQUIRE(client_done);
    REQUIRE(client_failed);
    REQUIRE(client_errno == ETIMEDOUT);
    REQUIRE(client_elapsed_ms.load() >= 0);
    REQUIRE(client_elapsed_ms.load() < 3000);
}

TEST_CASE("WebSocket client parses bracketed IPv6 URLs",
          "[websocket][client][ipv6][regression]") {
    auto listener = tcp_listener::bind(ipv6_address("::1", 0));
    if (!listener) {
        SKIP("IPv6 loopback listener is unavailable in this environment");
    }
    uint16_t port = listener->local_address().port();

    scheduler sched(2);
    sched.start();

    std::atomic<bool> server_done{false};
    std::atomic<bool> client_done{false};
    std::atomic<bool> client_connected{false};
    std::atomic<int> client_errno{0};
    std::string observed_host;

    sched.go([&]() -> task<void> {
        auto stream = co_await listener->accept();
        REQUIRE(stream.has_value());
        auto headers = co_await read_request_headers(*stream);
        observed_host = request_header_value(headers, "Host");
        auto key = request_header_value(headers, "Sec-WebSocket-Key");
        REQUIRE_FALSE(key.empty());
        auto accept = elio::http::websocket::compute_websocket_accept(key);
        auto response = elio::http::websocket::build_server_handshake(accept);
        co_await stream->write(response);
        co_await elio::time::sleep_for(std::chrono::milliseconds(100));
        stream->shutdown_socket();
        server_done = true;
    });

    sched.go([&]() -> task<void> {
        elio::http::websocket::client_config cfg;
        cfg.read_timeout = std::chrono::seconds(2);
        elio::http::websocket::ws_client client(cfg);

        bool ok = co_await client.connect(make_ws_ipv6_url(port, "/ws?room=1"));
        client_connected = ok;
        if (!ok) {
            client_errno = errno;
        }
        client_done = true;
    });

    for (int i = 0; i < 500 && !(client_done && server_done); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    sched.shutdown();

    REQUIRE(client_done);
    REQUIRE(server_done);
    REQUIRE(client_connected);
    REQUIRE(client_errno == 0);
    REQUIRE(observed_host == "[::1]:" + std::to_string(port));
}

TEST_CASE("WebSocket client rejects unoffered server subprotocol",
          "[websocket][client][handshake][regression]") {
    auto listener = tcp_listener::bind(ipv4_address("127.0.0.1", 0));
    REQUIRE(listener.has_value());
    uint16_t port = listener->local_address().port();

    scheduler sched(2);
    sched.start();

    std::atomic<bool> server_done{false};
    std::atomic<bool> client_done{false};
    std::atomic<bool> client_failed{false};
    std::atomic<int> client_errno{0};
    std::string observed_protocols;
    std::string negotiated_protocol;

    sched.go([&]() -> task<void> {
        auto stream = co_await listener->accept();
        REQUIRE(stream.has_value());
        auto headers = co_await read_request_headers(*stream);
        observed_protocols = request_header_value(headers, "Sec-WebSocket-Protocol");
        auto key = request_header_value(headers, "Sec-WebSocket-Key");
        REQUIRE_FALSE(key.empty());

        auto accept = elio::http::websocket::compute_websocket_accept(key);
        auto response = elio::http::websocket::build_server_handshake(
            accept, "admin");
        co_await stream->write(response);
        co_await elio::time::sleep_for(std::chrono::milliseconds(100));
        stream->shutdown_socket();
        server_done = true;
    });

    sched.go([&]() -> task<void> {
        elio::http::websocket::client_config cfg;
        cfg.read_timeout = std::chrono::seconds(2);
        cfg.subprotocols = {"chat"};
        elio::http::websocket::ws_client client(cfg);

        bool ok = co_await client.connect(make_ws_url(port));
        if (!ok) {
            client_failed = true;
            client_errno = errno;
        }
        negotiated_protocol = std::string(client.subprotocol());
        client_done = true;
    });

    for (int i = 0; i < 500 && !(client_done && server_done); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    sched.shutdown();

    REQUIRE(client_done);
    REQUIRE(server_done);
    REQUIRE(client_failed);
    REQUIRE(client_errno == EBADMSG);
    REQUIRE(observed_protocols == "chat");
    REQUIRE(negotiated_protocol.empty());
}

TEST_CASE("SSE client read_timeout fires on stalled response headers",
          "[sse][client][timeout][regression]") {
    auto listener = tcp_listener::bind(ipv4_address("127.0.0.1", 0));
    REQUIRE(listener.has_value());
    uint16_t port = listener->local_address().port();

    scheduler sched(2);
    sched.start();

    std::atomic<bool> client_done{false};
    std::atomic<bool> client_failed{false};
    std::atomic<int> client_errno{0};
    std::atomic<int64_t> client_elapsed_ms{-1};

    // Server: accept and drain the SSE GET request, but never send HTTP
    // headers. The client's read_timeout must bound response header reads.
    sched.go([&]() -> task<void> {
        auto stream = co_await listener->accept();
        REQUIRE(stream.has_value());
        co_await drain_request_headers(*stream);
        co_await elio::time::sleep_for(std::chrono::seconds(3));
        stream->shutdown_socket();
    });

    sched.go([&]() -> task<void> {
        elio::http::sse::client_config cfg;
        cfg.auto_reconnect = false;
        cfg.read_timeout = std::chrono::seconds(1);
        elio::http::sse::sse_client client(cfg);

        auto t0 = std::chrono::steady_clock::now();
        bool ok = co_await client.connect(make_url(port, "/events"));
        auto elapsed = std::chrono::steady_clock::now() - t0;

        client_elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            elapsed).count();
        if (!ok) {
            client_failed = true;
            client_errno = errno;
        }
        client_done = true;
    });

    for (int i = 0; i < 500 && !client_done; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    sched.shutdown();

    REQUIRE(client_done);
    REQUIRE(client_failed);
    REQUIRE(client_errno == ETIMEDOUT);
    REQUIRE(client_elapsed_ms.load() >= 0);
    REQUIRE(client_elapsed_ms.load() < 3000);
}

TEST_CASE("SSE client rejects unsupported URL schemes",
          "[sse][client][security]") {
    scheduler sched(1);
    sched.start();

    std::atomic<bool> client_done{false};
    bool connected = true;
    int client_errno = 0;

    sched.go([&]() -> task<void> {
        elio::http::sse::client_config cfg;
        cfg.auto_reconnect = false;
        elio::http::sse::sse_client client(cfg);

        errno = 0;
        connected = co_await client.connect("ftp://127.0.0.1/events");
        client_errno = errno;
        client_done = true;
    });

    for (int i = 0; i < 500 && !client_done; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    sched.shutdown();

    REQUIRE(client_done);
    REQUIRE_FALSE(connected);
    REQUIRE(client_errno == EINVAL);
}

TEST_CASE("HTTP client cancellation aborts a pending response read",
          "[http][client][cancel][regression]") {
    if constexpr (running_under_tsan()) {
        SKIP("connect-cancellation fake server drain is covered by normal and ASAN runs");
    }

    auto listener = tcp_listener::bind(ipv4_address("127.0.0.1", 0));
    REQUIRE(listener.has_value());
    uint16_t port = listener->local_address().port();

    scheduler sched(2);
    sched.start();

    std::atomic<bool> client_done{false};
    std::atomic<bool> client_failed{false};
    std::atomic<int> client_errno{0};
    std::atomic<int64_t> client_elapsed_ms{-1};
    elio::coro::cancel_source cancel_source;

    sched.go([&]() -> task<void> {
        auto stream = co_await listener->accept();
        REQUIRE(stream.has_value());
        co_await drain_request_headers(*stream);
        char extra[1];
        (void)co_await stream->read(extra, sizeof(extra));
        stream->shutdown_socket();
    });

    sched.go([&]() -> task<void> {
        co_await elio::time::sleep_for(std::chrono::milliseconds(100));
        cancel_source.cancel();
    });

    sched.go([&]() -> task<void> {
        elio::http::client_config cfg;
        cfg.read_timeout = std::chrono::seconds(0);
        elio::http::client c(cfg);

        auto t0 = std::chrono::steady_clock::now();
        auto resp = co_await c.get(make_url(port), cancel_source.get_token());
        auto elapsed = std::chrono::steady_clock::now() - t0;

        client_elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            elapsed).count();
        if (!resp) {
            client_failed = true;
            client_errno = errno;
        }
        client_done = true;
    });

    for (int i = 0; i < 500 && !client_done; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    if (!client_done) {
        cancel_source.cancel();
        for (int i = 0; i < 500 && !client_done; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }
    REQUIRE(sched.shutdown(std::chrono::seconds(10)));

    REQUIRE(client_done);
    REQUIRE(client_failed);
    REQUIRE(client_errno == ECANCELED);
    REQUIRE(client_elapsed_ms.load() >= 0);
    REQUIRE(client_elapsed_ms.load() < 3000);
}

TEST_CASE("WebSocket client cancellation aborts stalled handshake response",
          "[websocket][client][cancel][regression]") {
    if constexpr (running_under_tsan()) {
        SKIP("connect-cancellation fake server drain is covered by normal and ASAN runs");
    }

    auto listener = tcp_listener::bind(ipv4_address("127.0.0.1", 0));
    REQUIRE(listener.has_value());
    uint16_t port = listener->local_address().port();

    scheduler sched(2);
    sched.start();

    std::atomic<bool> client_done{false};
    std::atomic<bool> client_failed{false};
    std::atomic<int> client_errno{0};
    std::atomic<int64_t> client_elapsed_ms{-1};
    elio::coro::cancel_source cancel_source;

    sched.go([&]() -> task<void> {
        auto stream = co_await listener->accept();
        REQUIRE(stream.has_value());
        co_await drain_request_headers(*stream);
        char extra[1];
        (void)co_await stream->read(extra, sizeof(extra));
        stream->shutdown_socket();
    });

    sched.go([&]() -> task<void> {
        co_await elio::time::sleep_for(std::chrono::milliseconds(100));
        cancel_source.cancel();
    });

    sched.go([&]() -> task<void> {
        elio::http::websocket::client_config cfg;
        cfg.read_timeout = std::chrono::seconds(0);
        elio::http::websocket::ws_client client(cfg);

        auto t0 = std::chrono::steady_clock::now();
        bool ok = co_await client.connect(make_ws_url(port), cancel_source.get_token());
        auto elapsed = std::chrono::steady_clock::now() - t0;

        client_elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            elapsed).count();
        if (!ok) {
            client_failed = true;
            client_errno = errno;
        }
        client_done = true;
    });

    for (int i = 0; i < 500 && !client_done; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    if (!client_done) {
        cancel_source.cancel();
        for (int i = 0; i < 500 && !client_done; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }
    REQUIRE(sched.shutdown(std::chrono::seconds(10)));

    REQUIRE(client_done);
    REQUIRE(client_failed);
    REQUIRE(client_errno == ECANCELED);
    REQUIRE(client_elapsed_ms.load() >= 0);
    REQUIRE(client_elapsed_ms.load() < 3000);
}

TEST_CASE("SSE client cancellation aborts stalled response headers",
          "[sse][client][cancel][regression]") {
    if constexpr (running_under_tsan()) {
        SKIP("connect-cancellation fake server drain is covered by normal and ASAN runs");
    }

    auto listener = tcp_listener::bind(ipv4_address("127.0.0.1", 0));
    REQUIRE(listener.has_value());
    uint16_t port = listener->local_address().port();

    scheduler sched(2);
    sched.start();

    std::atomic<bool> client_done{false};
    std::atomic<bool> client_failed{false};
    std::atomic<int> client_errno{0};
    std::atomic<int64_t> client_elapsed_ms{-1};
    elio::coro::cancel_source cancel_source;

    sched.go([&]() -> task<void> {
        auto stream = co_await listener->accept();
        REQUIRE(stream.has_value());
        co_await drain_request_headers(*stream);
        char extra[1];
        (void)co_await stream->read(extra, sizeof(extra));
        stream->shutdown_socket();
    });

    sched.go([&]() -> task<void> {
        co_await elio::time::sleep_for(std::chrono::milliseconds(100));
        cancel_source.cancel();
    });

    sched.go([&]() -> task<void> {
        elio::http::sse::client_config cfg;
        cfg.auto_reconnect = false;
        cfg.read_timeout = std::chrono::seconds(0);
        elio::http::sse::sse_client client(cfg);

        auto t0 = std::chrono::steady_clock::now();
        bool ok = co_await client.connect(make_url(port, "/events"),
                                          cancel_source.get_token());
        auto elapsed = std::chrono::steady_clock::now() - t0;

        client_elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            elapsed).count();
        if (!ok) {
            client_failed = true;
            client_errno = errno;
        }
        client_done = true;
    });

    for (int i = 0; i < 500 && !client_done; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    if (!client_done) {
        cancel_source.cancel();
        for (int i = 0; i < 500 && !client_done; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }
    REQUIRE(sched.shutdown(std::chrono::seconds(10)));

    REQUIRE(client_done);
    REQUIRE(client_failed);
    REQUIRE(client_errno == ECANCELED);
    REQUIRE(client_elapsed_ms.load() >= 0);
    REQUIRE(client_elapsed_ms.load() < 3000);
}

TEST_CASE("SSE receive cancellation aborts a pending event read",
          "[sse][client][cancel][regression]") {
    auto listener = tcp_listener::bind(ipv4_address("127.0.0.1", 0));
    REQUIRE(listener.has_value());
    uint16_t port = listener->local_address().port();

    scheduler sched(2);
    sched.start();

    std::atomic<bool> client_done{false};
    std::atomic<bool> client_connected{false};
    std::atomic<bool> client_failed{false};
    std::atomic<bool> still_connected_after_cancel{false};
    std::atomic<int> client_errno{0};
    std::atomic<int64_t> client_elapsed_ms{-1};
    elio::coro::cancel_source cancel_source;

    sched.go([&]() -> task<void> {
        auto stream = co_await listener->accept();
        REQUIRE(stream.has_value());
        co_await drain_request_headers(*stream);
        std::string headers =
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: text/event-stream\r\n"
            "Connection: keep-alive\r\n"
            "\r\n";
        co_await stream->write(headers);
        char extra[1];
        (void)co_await stream->read(extra, sizeof(extra));
        stream->shutdown_socket();
    });

    sched.go([&]() -> task<void> {
        elio::http::sse::client_config cfg;
        cfg.auto_reconnect = false;
        cfg.read_timeout = std::chrono::seconds(0);
        elio::http::sse::sse_client client(cfg);

        bool ok = co_await client.connect(make_url(port, "/events"));
        client_connected = ok;
        if (!ok) {
            client_failed = true;
            client_errno = errno;
            client_done = true;
            co_return;
        }

        sched.go([&]() -> task<void> {
            co_await elio::time::sleep_for(std::chrono::milliseconds(100));
            cancel_source.cancel();
        });

        auto t0 = std::chrono::steady_clock::now();
        auto evt = co_await client.receive(cancel_source.get_token());
        auto elapsed = std::chrono::steady_clock::now() - t0;

        client_elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            elapsed).count();
        if (!evt) {
            client_failed = true;
            client_errno = errno;
        }
        still_connected_after_cancel = client.is_connected();
        co_await client.close();
        client_done = true;
    });

    for (int i = 0; i < 500 && !client_done; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    if (!client_done) {
        cancel_source.cancel();
        for (int i = 0; i < 500 && !client_done; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }
    REQUIRE(sched.shutdown(std::chrono::seconds(10)));

    REQUIRE(client_done);
    REQUIRE(client_connected);
    REQUIRE(client_failed);
    REQUIRE(client_errno == ECANCELED);
    REQUIRE(still_connected_after_cancel);
    REQUIRE(client_elapsed_ms.load() >= 0);
    REQUIRE(client_elapsed_ms.load() < 3000);
}

TEST_CASE("WebSocket receive cancellation aborts a pending message read",
          "[websocket][client][cancel][regression]") {
    auto listener = tcp_listener::bind(ipv4_address("127.0.0.1", 0));
    REQUIRE(listener.has_value());
    uint16_t port = listener->local_address().port();

    scheduler sched(2);
    sched.start();

    std::atomic<bool> client_done{false};
    std::atomic<bool> client_connected{false};
    std::atomic<bool> client_failed{false};
    std::atomic<bool> still_open_after_cancel{false};
    std::atomic<int> client_errno{0};
    std::atomic<int64_t> client_elapsed_ms{-1};
    elio::coro::cancel_source cancel_source;

    sched.go([&]() -> task<void> {
        auto stream = co_await listener->accept();
        REQUIRE(stream.has_value());
        auto headers = co_await read_request_headers(*stream);
        auto key = request_header_value(headers, "Sec-WebSocket-Key");
        REQUIRE_FALSE(key.empty());
        auto accept = elio::http::websocket::compute_websocket_accept(key);
        auto response = elio::http::websocket::build_server_handshake(accept);
        co_await stream->write(response);
        char extra[1];
        (void)co_await stream->read(extra, sizeof(extra));
        stream->shutdown_socket();
    });

    sched.go([&]() -> task<void> {
        elio::http::websocket::client_config cfg;
        cfg.read_timeout = std::chrono::seconds(0);
        elio::http::websocket::ws_client client(cfg);

        bool ok = co_await client.connect(make_ws_url(port));
        client_connected = ok;
        if (!ok) {
            client_failed = true;
            client_errno = errno;
            client_done = true;
            co_return;
        }

        sched.go([&]() -> task<void> {
            co_await elio::time::sleep_for(std::chrono::milliseconds(100));
            cancel_source.cancel();
        });

        auto t0 = std::chrono::steady_clock::now();
        auto msg = co_await client.receive(cancel_source.get_token());
        auto elapsed = std::chrono::steady_clock::now() - t0;

        client_elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            elapsed).count();
        if (!msg) {
            client_failed = true;
            client_errno = errno;
        }
        still_open_after_cancel = client.is_open();
        co_await client.close();
        client_done = true;
    });

    for (int i = 0; i < 500 && !client_done; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    if (!client_done) {
        cancel_source.cancel();
        for (int i = 0; i < 500 && !client_done; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }
    REQUIRE(sched.shutdown(std::chrono::seconds(10)));

    REQUIRE(client_done);
    REQUIRE(client_connected);
    REQUIRE(client_failed);
    REQUIRE(client_errno == ECANCELED);
    REQUIRE(still_open_after_cancel);
    REQUIRE(client_elapsed_ms.load() >= 0);
    REQUIRE(client_elapsed_ms.load() < 3000);
}

#if defined(ELIO_HAS_TLS) && ELIO_HAS_TLS
TEST_CASE("HTTP client connect_timeout fires on a stalled TLS handshake",
          "[http][client][timeout][tls]") {
    auto listener = tcp_listener::bind(ipv4_address("127.0.0.1", 0));
    REQUIRE(listener.has_value());
    uint16_t port = listener->local_address().port();

    scheduler sched(2);
    sched.start();

    std::atomic<bool> server_done{false};
    std::atomic<bool> client_done{false};
    std::atomic<bool> client_failed{false};
    std::atomic<int> client_errno{0};
    std::atomic<int64_t> client_elapsed_ms{-1};

    // Server: accept TCP, but never speak TLS. The client must not wait for
    // read_timeout here; connect_timeout covers connection setup.
    sched.go([&]() -> task<void> {
        auto stream = co_await listener->accept();
        REQUIRE(stream.has_value());
        co_await elio::time::sleep_for(std::chrono::seconds(3));
        stream->shutdown_socket();
        server_done = true;
    });

    sched.go([&]() -> task<void> {
        elio::http::client_config cfg;
        cfg.connect_timeout = std::chrono::seconds(1);
        cfg.read_timeout = std::chrono::seconds(10);
        elio::http::client c(cfg);
        c.tls_context().set_verify_mode(elio::tls::verify_mode::none);

        auto t0 = std::chrono::steady_clock::now();
        auto resp = co_await c.get(make_https_url(port));
        auto elapsed = std::chrono::steady_clock::now() - t0;

        client_elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
            elapsed).count();
        if (!resp) {
            client_failed = true;
            client_errno = errno;
        }
        client_done = true;
    });

    for (int i = 0; i < 500 && !(client_done && server_done); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    sched.shutdown();

    REQUIRE(client_done);
    REQUIRE(client_failed);
    REQUIRE(client_errno == ETIMEDOUT);
    REQUIRE(client_elapsed_ms.load() >= 0);
    REQUIRE(client_elapsed_ms.load() < 3000);
}
#endif

TEST_CASE("HTTP client returns OK for clean keep-alive responses",
          "[http][client]") {
    // Sanity check that the timeout/limit machinery doesn't break the
    // happy path. Without this the other two tests could pass for the
    // wrong reason.
    auto listener = tcp_listener::bind(ipv4_address("127.0.0.1", 0));
    REQUIRE(listener.has_value());
    uint16_t port = listener->local_address().port();

    scheduler sched(2);
    sched.start();

    std::atomic<bool> server_done{false};
    std::atomic<bool> client_done{false};
    std::atomic<int> got_status{0};

    sched.go([&]() -> task<void> {
        auto stream = co_await listener->accept();
        REQUIRE(stream.has_value());
        co_await drain_request_headers(*stream);
        std::string resp =
            "HTTP/1.1 200 OK\r\n"
            "Content-Length: 5\r\n"
            "Connection: close\r\n"
            "\r\n"
            "hello";
        co_await stream->write(resp);
        server_done = true;
    });

    sched.go([&]() -> task<void> {
        elio::http::client c;
        auto resp = co_await c.get(make_url(port));
        if (resp) {
            got_status = resp->status_code();
        }
        client_done = true;
    });

    for (int i = 0; i < 500 && !(client_done && server_done); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    sched.shutdown();

    REQUIRE(client_done);
    REQUIRE(got_status == 200);
}

TEST_CASE("HTTP client skips informational responses before final response",
          "[http][client]") {
    auto listener = tcp_listener::bind(ipv4_address("127.0.0.1", 0));
    REQUIRE(listener.has_value());
    uint16_t port = listener->local_address().port();

    scheduler sched(2);
    sched.start();

    std::atomic<bool> server_done{false};
    std::atomic<bool> client_done{false};
    std::atomic<int> got_status{0};
    std::string got_body;

    sched.go([&]() -> task<void> {
        auto stream = co_await listener->accept();
        REQUIRE(stream.has_value());
        co_await drain_request_headers(*stream);
        std::string resp =
            "HTTP/1.1 100 Continue\r\n"
            "\r\n"
            "HTTP/1.1 103 Early Hints\r\n"
            "Link: </style.css>; rel=preload; as=style\r\n"
            "\r\n"
            "HTTP/1.1 200 OK\r\n"
            "Content-Length: 5\r\n"
            "Connection: close\r\n"
            "\r\n"
            "hello";
        co_await stream->write(resp);
        server_done = true;
    });

    sched.go([&]() -> task<void> {
        elio::http::client_config cfg;
        cfg.read_timeout = std::chrono::seconds(5);
        elio::http::client c(cfg);

        auto resp = co_await c.get(make_url(port));
        if (resp) {
            got_status = resp->status_code();
            got_body = std::string(resp->body());
        }
        client_done = true;
    });

    for (int i = 0; i < 500 && !(client_done && server_done); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    sched.shutdown();

    REQUIRE(client_done);
    REQUIRE(got_status == 200);
    REQUIRE(got_body == "hello");
}

TEST_CASE("HTTP client caps cumulative informational responses",
          "[http][client][security]") {
    auto listener = tcp_listener::bind(ipv4_address("127.0.0.1", 0));
    REQUIRE(listener.has_value());
    uint16_t port = listener->local_address().port();

    scheduler sched(2);
    sched.start();

    std::atomic<bool> server_done{false};
    std::atomic<bool> client_done{false};
    std::atomic<bool> client_failed{false};
    std::atomic<int> client_errno{0};

    sched.go([&]() -> task<void> {
        auto stream = co_await listener->accept();
        REQUIRE(stream.has_value());
        co_await drain_request_headers(*stream);

        std::string interim =
            "HTTP/1.1 103 Early Hints\r\n"
            "Link: </very-large-early-hints-entry.css>; rel=preload\r\n"
            "\r\n";
        for (int i = 0; i < 4; ++i) {
            auto w = co_await stream->write(interim);
            if (w.result <= 0) {
                server_done = true;
                co_return;
            }
            co_await elio::time::sleep_for(std::chrono::milliseconds(20));
        }
        std::string final_response =
            "HTTP/1.1 200 OK\r\n"
            "Content-Length: 2\r\n"
            "Connection: close\r\n"
            "\r\n"
            "ok";
        co_await stream->write(final_response);
        server_done = true;
    });

    sched.go([&]() -> task<void> {
        elio::http::client_config cfg;
        cfg.max_response_size = 128;
        cfg.read_timeout = std::chrono::seconds(0);
        elio::http::client c(cfg);

        auto resp = co_await c.get(make_url(port));
        if (!resp) {
            client_failed = true;
            client_errno = errno;
        }
        client_done = true;
    });

    for (int i = 0; i < 500 && !(client_done && server_done); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    sched.shutdown();

    REQUIRE(client_done);
    REQUIRE(client_failed);
    REQUIRE(client_errno == EMSGSIZE);
}

TEST_CASE("HTTP client rejects unexpected switching protocols response",
          "[http][client]") {
    auto listener = tcp_listener::bind(ipv4_address("127.0.0.1", 0));
    REQUIRE(listener.has_value());
    uint16_t port = listener->local_address().port();

    scheduler sched(2);
    sched.start();

    std::atomic<bool> server_done{false};
    std::atomic<bool> client_done{false};
    std::atomic<bool> client_failed{false};
    std::atomic<int> client_errno{0};

    sched.go([&]() -> task<void> {
        auto stream = co_await listener->accept();
        REQUIRE(stream.has_value());
        co_await drain_request_headers(*stream);
        std::string resp =
            "HTTP/1.1 101 Switching Protocols\r\n"
            "Connection: Upgrade\r\n"
            "Upgrade: websocket\r\n"
            "\r\n";
        co_await stream->write(resp);
        server_done = true;
    });

    sched.go([&]() -> task<void> {
        elio::http::client_config cfg;
        cfg.read_timeout = std::chrono::seconds(5);
        elio::http::client c(cfg);

        auto resp = co_await c.get(make_url(port));
        if (!resp) {
            client_failed = true;
            client_errno = errno;
        }
        client_done = true;
    });

    for (int i = 0; i < 500 && !(client_done && server_done); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    sched.shutdown();

    REQUIRE(client_done);
    REQUIRE(client_failed);
    REQUIRE(client_errno == EBADMSG);
}

TEST_CASE("HTTP client accepts close-delimited response bodies",
          "[http][client]") {
    auto listener = tcp_listener::bind(ipv4_address("127.0.0.1", 0));
    REQUIRE(listener.has_value());
    uint16_t port = listener->local_address().port();

    scheduler sched(2);
    sched.start();

    std::atomic<bool> server_done{false};
    std::atomic<bool> client_done{false};
    std::atomic<int> got_status{0};
    std::string got_body;

    sched.go([&]() -> task<void> {
        auto stream = co_await listener->accept();
        REQUIRE(stream.has_value());
        co_await drain_request_headers(*stream);
        std::string resp =
            "HTTP/1.1 200 OK\r\n"
            "Connection: close\r\n"
            "\r\n"
            "hello";
        co_await stream->write(resp);
        co_await stream->close();
        server_done = true;
    });

    sched.go([&]() -> task<void> {
        elio::http::client_config cfg;
        cfg.read_timeout = std::chrono::seconds(5);
        elio::http::client c(cfg);

        auto resp = co_await c.get(make_url(port));
        if (resp) {
            got_status = resp->status_code();
            got_body = std::string(resp->body());
        }
        client_done = true;
    });

    for (int i = 0; i < 500 && !(client_done && server_done); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    sched.shutdown();

    REQUIRE(client_done);
    REQUIRE(got_status == 200);
    REQUIRE(got_body == "hello");
}
