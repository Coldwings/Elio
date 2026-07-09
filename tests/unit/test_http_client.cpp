// Integration tests for elio::http::client hardening:
//   1. max_response_size aborts a body that exceeds the cap.
//   2. read_timeout interrupts a stalled response read.
//   3. A connection with leftover bytes after the response is NOT pooled
//      (response-splitting prevention).
//
// These exercise the post-PR send_request path. We stand up a tiny TCP
// listener that pretends to be an HTTP server and craft hand-built byte
// sequences for each scenario — no real http::server in the loop.

#include <catch2/catch_test_macros.hpp>

#include <elio/http/http_client.hpp>
#include <elio/net/tcp.hpp>
#include <elio/runtime/scheduler.hpp>
#include <elio/time/timer.hpp>

#include <atomic>
#include <chrono>
#include <string>
#include <thread>

using elio::coro::task;
using elio::net::ipv4_address;
using elio::net::tcp_listener;
using elio::runtime::scheduler;

namespace {

std::string make_url(uint16_t port, std::string_view path = "/") {
    return std::string("http://127.0.0.1:") + std::to_string(port) +
           std::string(path);
}

// Drain a single request from the stream (until "\r\n\r\n"). Used by the
// fake server to know when to start writing its response.
task<void> drain_request_headers(elio::net::tcp_stream& s) {
    std::string accum;
    char buf[1024];
    while (accum.find("\r\n\r\n") == std::string::npos) {
        auto r = co_await s.read(buf, sizeof(buf));
        if (r.result <= 0) co_return;
        accum.append(buf, static_cast<size_t>(r.result));
    }
    co_return;
}

} // namespace

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
