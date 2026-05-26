#include <catch2/catch_test_macros.hpp>
#include <elio/http/sse.hpp>
#include <elio/net/tcp.hpp>
#include <elio/runtime/scheduler.hpp>
#include <elio/runtime/spawn.hpp>
#include <elio/coro/cancel_token.hpp>
#include <elio/time/timer.hpp>

#include <atomic>
#include <chrono>
#include <string>
#include <thread>
#include <vector>

using namespace elio::http::sse;

// ============================================================================
// SSE Event Tests
// ============================================================================

TEST_CASE("SSE event creation", "[sse][event]") {
    SECTION("message factory") {
        auto evt = event::message("Hello, World!");
        
        REQUIRE(evt.data == "Hello, World!");
        REQUIRE(evt.type.empty());
        REQUIRE(evt.id.empty());
        REQUIRE(evt.retry == -1);
    }
    
    SECTION("typed factory") {
        auto evt = event::typed("notification", "New message");
        
        REQUIRE(evt.data == "New message");
        REQUIRE(evt.type == "notification");
        REQUIRE(evt.id.empty());
    }
    
    SECTION("with_id factory") {
        auto evt = event::with_id("12345", "Test data");
        
        REQUIRE(evt.data == "Test data");
        REQUIRE(evt.id == "12345");
        REQUIRE(evt.type.empty());
    }
    
    SECTION("full factory") {
        auto evt = event::full("999", "custom", "payload", 5000);
        
        REQUIRE(evt.id == "999");
        REQUIRE(evt.type == "custom");
        REQUIRE(evt.data == "payload");
        REQUIRE(evt.retry == 5000);
    }
}

TEST_CASE("SSE event serialization", "[sse][serialize]") {
    SECTION("simple data event") {
        event evt;
        evt.data = "Hello";
        
        auto result = serialize_event(evt);
        
        REQUIRE(result == "data: Hello\n\n");
    }
    
    SECTION("event with type") {
        auto evt = event::typed("message", "Hello");
        
        auto result = serialize_event(evt);
        
        REQUIRE(result == "event: message\ndata: Hello\n\n");
    }
    
    SECTION("event with id") {
        event evt;
        evt.id = "123";
        evt.data = "Hello";
        
        auto result = serialize_event(evt);
        
        REQUIRE(result == "id: 123\ndata: Hello\n\n");
    }
    
    SECTION("event with retry") {
        event evt;
        evt.data = "Hello";
        evt.retry = 5000;
        
        auto result = serialize_event(evt);
        
        REQUIRE(result == "retry: 5000\ndata: Hello\n\n");
    }
    
    SECTION("full event") {
        event evt;
        evt.id = "42";
        evt.type = "update";
        evt.data = "content";
        evt.retry = 3000;
        
        auto result = serialize_event(evt);
        
        REQUIRE(result == "id: 42\nevent: update\nretry: 3000\ndata: content\n\n");
    }
    
    SECTION("multiline data") {
        event evt;
        evt.data = "line1\nline2\nline3";
        
        auto result = serialize_event(evt);
        
        REQUIRE(result == "data: line1\ndata: line2\ndata: line3\n\n");
    }
    
    SECTION("empty data") {
        event evt;
        
        auto result = serialize_event(evt);
        
        REQUIRE(result == "data: \n\n");
    }
}

// ============================================================================
// SSE Event Parser Tests
// ============================================================================

TEST_CASE("SSE event parsing", "[sse][parser]") {
    SECTION("parse simple event") {
        event_parser parser;
        
        parser.parse("data: Hello\n\n");
        
        REQUIRE(parser.has_event());
        auto evt = parser.get_event();
        REQUIRE(evt.has_value());
        REQUIRE(evt->data == "Hello");
        REQUIRE(evt->type.empty());
    }
    
    SECTION("parse typed event") {
        event_parser parser;
        
        parser.parse("event: notification\ndata: New message\n\n");
        
        REQUIRE(parser.has_event());
        auto evt = parser.get_event();
        REQUIRE(evt->type == "notification");
        REQUIRE(evt->data == "New message");
    }
    
    SECTION("parse event with id") {
        event_parser parser;
        
        parser.parse("id: 123\ndata: Hello\n\n");
        
        REQUIRE(parser.has_event());
        auto evt = parser.get_event();
        REQUIRE(evt->id == "123");
        REQUIRE(evt->data == "Hello");
        
        // Last event ID should be updated
        REQUIRE(parser.last_event_id() == "123");
    }
    
    SECTION("parse event with retry") {
        event_parser parser;
        
        parser.parse("retry: 5000\ndata: Hello\n\n");
        
        REQUIRE(parser.has_event());
        parser.get_event();
        
        // Retry should be updated
        REQUIRE(parser.retry_ms() == 5000);
    }
    
    SECTION("parse multiline data") {
        event_parser parser;
        
        parser.parse("data: line1\ndata: line2\ndata: line3\n\n");
        
        REQUIRE(parser.has_event());
        auto evt = parser.get_event();
        REQUIRE(evt->data == "line1\nline2\nline3");
    }
    
    SECTION("parse multiple events") {
        event_parser parser;
        
        parser.parse("data: first\n\ndata: second\n\n");
        
        REQUIRE(parser.has_event());
        auto evt1 = parser.get_event();
        REQUIRE(evt1->data == "first");
        
        REQUIRE(parser.has_event());
        auto evt2 = parser.get_event();
        REQUIRE(evt2->data == "second");
        
        REQUIRE_FALSE(parser.has_event());
    }
    
    SECTION("ignore comments") {
        event_parser parser;
        
        parser.parse(": this is a comment\ndata: Hello\n\n");
        
        REQUIRE(parser.has_event());
        auto evt = parser.get_event();
        REQUIRE(evt->data == "Hello");
    }
    
    SECTION("ignore unknown fields") {
        event_parser parser;
        
        parser.parse("foo: bar\ndata: Hello\n\n");
        
        REQUIRE(parser.has_event());
        auto evt = parser.get_event();
        REQUIRE(evt->data == "Hello");
    }
    
    SECTION("handle empty data field") {
        event_parser parser;
        
        parser.parse("data:\n\n");
        
        REQUIRE(parser.has_event());
        auto evt = parser.get_event();
        REQUIRE(evt->data.empty());
    }
    
    SECTION("handle field without colon") {
        event_parser parser;
        
        parser.parse("event\ndata: test\n\n");
        
        REQUIRE(parser.has_event());
        auto evt = parser.get_event();
        REQUIRE(evt->type.empty());  // "event" alone sets type to ""
        REQUIRE(evt->data == "test");
    }
    
    SECTION("handle trailing whitespace in data") {
        event_parser parser;
        
        // Leading space after colon is stripped
        parser.parse("data:  hello  \n\n");
        
        REQUIRE(parser.has_event());
        auto evt = parser.get_event();
        REQUIRE(evt->data == " hello  ");  // Only first space stripped
    }
    
    SECTION("incremental parsing") {
        event_parser parser;
        
        parser.parse("dat");
        REQUIRE_FALSE(parser.has_event());
        
        parser.parse("a: Hel");
        REQUIRE_FALSE(parser.has_event());
        
        parser.parse("lo\n\n");
        REQUIRE(parser.has_event());
        
        auto evt = parser.get_event();
        REQUIRE(evt->data == "Hello");
    }
    
    SECTION("CRLF line endings") {
        event_parser parser;
        
        parser.parse("data: Hello\r\n\r\n");
        
        REQUIRE(parser.has_event());
        auto evt = parser.get_event();
        REQUIRE(evt->data == "Hello");
    }
    
    SECTION("reject id with null character") {
        event_parser parser;
        
        // Construct string with embedded null properly
        std::string data = "id: test";
        data += '\0';
        data += "null\ndata: Hello\n\n";
        parser.parse(data);
        
        REQUIRE(parser.has_event());
        auto evt = parser.get_event();
        // ID with null should be ignored
        REQUIRE(evt->id.empty());
    }
    
    SECTION("invalid retry value ignored") {
        event_parser parser;
        
        parser.parse("retry: abc\ndata: Hello\n\n");
        
        REQUIRE(parser.has_event());
        // Default retry should remain
        REQUIRE(parser.retry_ms() == 3000);
    }
    
    SECTION("last event id persists across events") {
        event_parser parser;
        
        parser.parse("id: 1\ndata: first\n\n");
        parser.get_event();
        REQUIRE(parser.last_event_id() == "1");
        
        parser.parse("data: second\n\n");  // No id
        parser.get_event();
        REQUIRE(parser.last_event_id() == "1");  // Still 1
        
        parser.parse("id: 2\ndata: third\n\n");
        parser.get_event();
        REQUIRE(parser.last_event_id() == "2");
    }
    
    SECTION("reset clears buffer but keeps last_event_id") {
        event_parser parser;
        
        parser.parse("id: 123\ndata: test\n\n");
        parser.get_event();
        
        parser.parse("data: partial");  // Incomplete
        
        parser.reset();
        
        REQUIRE_FALSE(parser.has_event());
        REQUIRE(parser.last_event_id() == "123");  // Preserved
    }
}

// ============================================================================
// SSE Response Building Tests
// ============================================================================

TEST_CASE("SSE response building", "[sse][response]") {
    SECTION("build_sse_response has correct headers") {
        auto resp = build_sse_response();
        
        REQUIRE(resp.get_status() == elio::http::status::ok);
        REQUIRE(resp.header("Content-Type") == SSE_CONTENT_TYPE);
        REQUIRE(resp.header("Cache-Control") == "no-cache");
        REQUIRE(resp.header("Connection") == "keep-alive");
        REQUIRE(resp.header("Access-Control-Allow-Origin") == "*");
    }
}

// ============================================================================
// Integration-Style Tests
// ============================================================================

TEST_CASE("SSE round-trip serialization", "[sse][integration]") {
    SECTION("serialize and parse simple event") {
        event original;
        original.data = "Test message";
        
        auto serialized = serialize_event(original);
        
        event_parser parser;
        parser.parse(serialized);
        
        REQUIRE(parser.has_event());
        auto parsed = parser.get_event();
        REQUIRE(parsed->data == original.data);
    }
    
    SECTION("serialize and parse complex event") {
        event original;
        original.id = "event-42";
        original.type = "notification";
        original.data = "Line 1\nLine 2";
        
        auto serialized = serialize_event(original);
        
        event_parser parser;
        parser.parse(serialized);
        
        REQUIRE(parser.has_event());
        auto parsed = parser.get_event();
        REQUIRE(parsed->id == original.id);
        REQUIRE(parsed->type == original.type);
        REQUIRE(parsed->data == original.data);
    }
    
    SECTION("stream of events") {
        std::vector<event> events;
        events.push_back(event::message("First"));
        events.push_back(event::typed("update", "Second"));
        events.push_back(event::with_id("3", "Third"));
        
        // Serialize all
        std::string stream;
        for (const auto& evt : events) {
            stream += serialize_event(evt);
        }
        
        // Parse all
        event_parser parser;
        parser.parse(stream);
        
        for (size_t i = 0; i < events.size(); ++i) {
            REQUIRE(parser.has_event());
            auto parsed = parser.get_event();
            REQUIRE(parsed->data == events[i].data);
        }
        
        REQUIRE_FALSE(parser.has_event());
    }
}

TEST_CASE("SSE JSON data", "[sse][json]") {
    SECTION("JSON in event data") {
        event evt;
        evt.type = "data";
        evt.data = R"({"user":"john","action":"login","timestamp":1234567890})";
        
        auto serialized = serialize_event(evt);
        
        event_parser parser;
        parser.parse(serialized);
        
        REQUIRE(parser.has_event());
        auto parsed = parser.get_event();
        REQUIRE(parsed->data == evt.data);
        
        // Verify JSON is preserved
        REQUIRE(parsed->data.find("\"user\":\"john\"") != std::string::npos);
    }
    
    SECTION("multiline JSON in event data") {
        event evt;
        evt.data = R"({
    "user": "john",
    "items": [1, 2, 3]
})";
        
        auto serialized = serialize_event(evt);
        
        event_parser parser;
        parser.parse(serialized);
        
        REQUIRE(parser.has_event());
        auto parsed = parser.get_event();
        
        // Verify all lines are present
        REQUIRE(parsed->data.find("\"user\": \"john\"") != std::string::npos);
        REQUIRE(parsed->data.find("\"items\": [1, 2, 3]") != std::string::npos);
    }
}

// ============================================================================
// Regression tests for sse_connection / sse_client correctness
// ============================================================================
//
// These tests cover three issues fixed alongside this file:
//
//   1. Concurrent senders must not interleave bytes inside a single SSE frame.
//      sse_connection now holds a per-connection coroutine mutex that
//      serializes writes inside send_raw().
//
//   2. SSE responses MUST be parsed headers-only.  An upstream that wraps the
//      stream in `Content-Length: N` (or chunked) used to cause the generic
//      response_parser to consume real SSE event bytes as the HTTP body, so
//      the first events disappeared into parser.take_body().  do_connect()
//      now feeds only the bytes up to and including `\r\n\r\n` to the
//      response parser and forwards everything after the delimiter to the
//      SSE event parser.
//
//   3. receive(token) must observe BOTH the per-call token and the
//      connect-time token.  The previous selection logic was inverted and
//      silently dropped one of the two; the loop now checks both at every
//      iteration.

namespace {

/// Spin-wait briefly for a flag/predicate to flip to true.  Returns whether
/// it did within the timeout.  Used in lieu of join_handles below since the
/// integration tests fire-and-forget tasks via scheduler::go().
template <typename Pred>
bool wait_for(Pred pred, std::chrono::milliseconds timeout =
                              std::chrono::seconds(5)) {
    auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (pred()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return pred();
}

/// Send the entire buffer through a tcp_stream, retrying short writes.
elio::coro::task<bool> write_all(elio::net::tcp_stream& s,
                                  std::string_view data) {
    size_t sent = 0;
    while (sent < data.size()) {
        auto r = co_await s.write(data.data() + sent, data.size() - sent);
        if (r.result <= 0) co_return false;
        sent += static_cast<size_t>(r.result);
    }
    co_return true;
}

/// Read until the buffer either ends or `terminator` has been observed.
/// Drains the socket so the test can inspect what was written before
/// it closed.
elio::coro::task<std::string> read_until_close(elio::net::tcp_stream& s,
                                                 size_t cap = 65536) {
    std::string out;
    char buf[1024];
    while (out.size() < cap) {
        auto r = co_await s.read(buf, sizeof(buf));
        if (r.result <= 0) break;
        out.append(buf, static_cast<size_t>(r.result));
    }
    co_return out;
}

/// Drain bytes until we see a complete `\r\n\r\n` header terminator.
/// Returns the consumed prefix on success, empty on EOF.
elio::coro::task<std::string> read_request_headers(elio::net::tcp_stream& s) {
    std::string buf;
    char tmp[1024];
    while (buf.size() < 8192) {
        auto r = co_await s.read(tmp, sizeof(tmp));
        if (r.result <= 0) co_return std::string{};
        buf.append(tmp, static_cast<size_t>(r.result));
        if (buf.find("\r\n\r\n") != std::string::npos) co_return buf;
    }
    co_return std::string{};
}

}  // namespace

TEST_CASE("sse_connection serializes concurrent send_event calls",
          "[sse][concurrent][regression]") {
    using namespace elio;
    using namespace elio::net;
    using namespace elio::runtime;
    namespace coro = elio::coro;

    // Pair: server side wraps a tcp_stream in sse_connection; reader side is
    // a plain tcp_stream we drain to see exactly what hit the wire.
    auto listener_opt = tcp_listener::bind(ipv4_address("127.0.0.1", 0));
    REQUIRE(listener_opt.has_value());
    uint16_t port = listener_opt->local_address().port();

    scheduler sched(2);
    sched.start();

    constexpr int kRoundsPerSender = 25;
    std::atomic<int> writes_done{0};
    std::atomic<int> reader_done{0};
    std::string captured;

    sched.go([&]() -> coro::task<void> {
        auto server_stream = co_await listener_opt->accept();
        REQUIRE(server_stream.has_value());

        sse_connection conn(&*server_stream);

        // Two coroutines pump distinct events concurrently.  Without the send
        // mutex they would interleave bytes inside `event:`/`data:`/`\n\n`
        // framing and the wire output would not split cleanly into frames.
        auto sender = [&](std::string type) -> coro::task<void> {
            for (int i = 0; i < kRoundsPerSender; ++i) {
                std::string payload = type + ":" + std::to_string(i);
                bool ok = co_await conn.send_event(type, payload);
                REQUIRE(ok);
            }
            writes_done.fetch_add(1, std::memory_order_release);
        };
        auto t_a = elio::spawn([&] { return sender("alpha"); });
        auto t_b = elio::spawn([&] { return sender("beta"); });
        co_await std::move(t_a);
        co_await std::move(t_b);

        conn.close();
        // Half-close so the reader sees EOF promptly.
        co_await server_stream->close();
    });

    sched.go([&]() -> coro::task<void> {
        auto client_stream = co_await tcp_connect(ipv4_address("127.0.0.1", port));
        REQUIRE(client_stream.has_value());
        captured = co_await read_until_close(*client_stream, 1u << 20);
        reader_done.fetch_add(1, std::memory_order_release);
    });

    REQUIRE(wait_for([&] { return reader_done.load() == 1; }));
    sched.shutdown();

    // Re-parse what landed on the wire.  Every byte should belong to one of
    // the well-formed events; if frames had been spliced, the parser would
    // still happily produce events but field values would be corrupted —
    // we therefore validate each event's data matches "<type>:<index>".
    event_parser parser;
    parser.parse(captured);

    int alpha_seen = 0, beta_seen = 0;
    for (;;) {
        auto evt = parser.get_event();
        if (!evt) break;
        REQUIRE((evt->type == "alpha" || evt->type == "beta"));
        std::string expected = evt->type + ":" +
                               std::to_string(evt->type == "alpha" ? alpha_seen
                                                                   : beta_seen);
        REQUIRE(evt->data == expected);
        if (evt->type == "alpha") {
            ++alpha_seen;
        } else {
            ++beta_seen;
        }
    }
    REQUIRE(alpha_seen == kRoundsPerSender);
    REQUIRE(beta_seen == kRoundsPerSender);
}

TEST_CASE("sse_client does not eat events into HTTP body even when the "
          "server lies about Content-Length",
          "[sse][client][regression]") {
    using namespace elio;
    using namespace elio::net;
    using namespace elio::runtime;
    namespace coro = elio::coro;

    auto listener_opt = tcp_listener::bind(ipv4_address("127.0.0.1", 0));
    REQUIRE(listener_opt.has_value());
    uint16_t port = listener_opt->local_address().port();

    scheduler sched(2);
    sched.start();

    std::atomic<bool> server_done{false};
    std::atomic<int> events_received{0};
    std::vector<event> got;

    sched.go([&]() -> coro::task<void> {
        auto server_stream = co_await listener_opt->accept();
        REQUIRE(server_stream.has_value());

        // Drain the GET request.
        auto req = co_await read_request_headers(*server_stream);
        REQUIRE(!req.empty());

        // Misbehaving server: declares a bogus Content-Length and then sends
        // SSE events.  Pre-fix, response_parser would consume the first
        // `Content-Length` bytes of the body and the events would be lost.
        std::string body =
            "event: greet\ndata: hello\n\n"
            "event: tick\ndata: 1\n\n"
            "event: tick\ndata: 2\n\n";

        std::string headers =
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: text/event-stream\r\n"
            "Content-Length: " + std::to_string(body.size() * 4) + "\r\n"
            "Cache-Control: no-cache\r\n"
            "\r\n";

        REQUIRE(co_await write_all(*server_stream, headers));
        REQUIRE(co_await write_all(*server_stream, body));
        // Hold the socket open briefly so the client can drain events
        // before EOF triggers the auto-reconnect path.
        co_await elio::time::sleep_for(std::chrono::milliseconds(150));
        co_await server_stream->close();
        server_done = true;
    });

    sched.go([&]() -> coro::task<void> {
        client_config cfg;
        cfg.auto_reconnect = false;  // single-shot; we want a deterministic test
        sse_client client(cfg);
        std::string url =
            "http://127.0.0.1:" + std::to_string(port) + "/events";
        bool connected = co_await client.connect(url);
        REQUIRE(connected);

        // We sent 3 events.  If response_parser had consumed any of them as
        // body bytes, fewer than 3 would arrive (or they would be malformed).
        for (int i = 0; i < 3; ++i) {
            auto evt = co_await client.receive();
            if (!evt) break;
            got.push_back(*evt);
            events_received.fetch_add(1, std::memory_order_release);
        }
        co_await client.close();
    });

    REQUIRE(wait_for([&] { return events_received.load() == 3 && server_done.load(); }));
    sched.shutdown();

    REQUIRE(got.size() == 3);
    REQUIRE(got[0].type == "greet");
    REQUIRE(got[0].data == "hello");
    REQUIRE(got[1].type == "tick");
    REQUIRE(got[1].data == "1");
    REQUIRE(got[2].type == "tick");
    REQUIRE(got[2].data == "2");
}

TEST_CASE("sse_client receive(token) observes the per-call cancel token",
          "[sse][cancel][regression]") {
    using namespace elio;
    using namespace elio::net;
    using namespace elio::runtime;
    namespace coro = elio::coro;

    auto listener_opt = tcp_listener::bind(ipv4_address("127.0.0.1", 0));
    REQUIRE(listener_opt.has_value());
    uint16_t port = listener_opt->local_address().port();

    scheduler sched(2);
    sched.start();

    std::atomic<bool> server_handshake_done{false};
    std::atomic<bool> client_returned{false};
    std::atomic<bool> got_nullopt{false};

    sched.go([&]() -> coro::task<void> {
        auto server_stream = co_await listener_opt->accept();
        REQUIRE(server_stream.has_value());

        auto req = co_await read_request_headers(*server_stream);
        REQUIRE(!req.empty());

        std::string headers =
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: text/event-stream\r\n"
            "Cache-Control: no-cache\r\n"
            "\r\n";
        // Send one event so the first receive() returns immediately and we
        // are firmly in the "connected" state.  We then keep the socket open
        // (no further data) so the cancellation is the only thing that can
        // unstick the next receive() — except we cancel BEFORE that receive,
        // so the loop's top-of-iteration check is what fires.
        std::string body = "event: ping\ndata: 1\n\n";
        REQUIRE(co_await write_all(*server_stream, headers));
        REQUIRE(co_await write_all(*server_stream, body));
        server_handshake_done = true;

        // Hold the connection open until the test tears the scheduler down.
        co_await elio::time::sleep_for(std::chrono::seconds(2));
        co_await server_stream->close();
    });

    sched.go([&]() -> coro::task<void> {
        client_config cfg;
        cfg.auto_reconnect = false;
        sse_client client(cfg);
        // Connect with NO connect-time cancel token; only the per-call token
        // should be capable of breaking out — the previous (inverted) ternary
        // would have observed the connect-time token instead and ignored
        // the per-call cancellation.
        std::string url =
            "http://127.0.0.1:" + std::to_string(port) + "/events";
        bool ok = co_await client.connect(url);
        REQUIRE(ok);

        // First event arrives normally.
        auto evt = co_await client.receive();
        REQUIRE(evt.has_value());
        REQUIRE(evt->type == "ping");

        // Cancel the per-call token, then call receive(token).  The loop
        // checks `cancelled()` at the top of every iteration; with the fix
        // it must observe the per-call token and return std::nullopt.
        coro::cancel_source src;
        auto token = src.get_token();
        src.cancel();

        auto evt2 = co_await client.receive(token);
        if (!evt2.has_value()) {
            got_nullopt = true;
        }
        client_returned = true;
        co_await client.close();
    });

    REQUIRE(wait_for([&] { return client_returned.load(); }));
    sched.shutdown();

    REQUIRE(server_handshake_done.load());
    REQUIRE(got_nullopt.load());
}
