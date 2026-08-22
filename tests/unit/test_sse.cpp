#include <catch2/catch_test_macros.hpp>
#include <elio/http/sse.hpp>
#include <elio/net/tcp.hpp>
#include <elio/runtime/scheduler.hpp>
#include <elio/runtime/spawn.hpp>
#include <elio/coro/cancel_token.hpp>
#include <elio/coro/this_coro.hpp>
#include <elio/time/timer.hpp>

#include "../test_main.cpp"

#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <exception>
#include <memory>
#include <mutex>
#include <optional>
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

    SECTION("data is split on every SSE line ending") {
        event evt;
        evt.data = "line1\rline2\r\nline3\nline4";

        auto result = serialize_event(evt);

        REQUIRE(result ==
                "data: line1\ndata: line2\ndata: line3\ndata: line4\n\n");
    }

    SECTION("data preserves terminal SSE line endings") {
        struct test_case {
            std::string input;
            std::string expected_serialized;
        };

        const std::vector<test_case> cases = {
            {"\n", "data: \ndata: \n\n"},
            {"tail\n", "data: tail\ndata: \n\n"},
            {"tail\r", "data: tail\ndata: \n\n"},
            {"tail\r\n", "data: tail\ndata: \n\n"},
            {"tail\n\n", "data: tail\ndata: \ndata: \n\n"},
        };

        for (const auto& test : cases) {
            CAPTURE(test.input);

            event evt;
            evt.data = test.input;

            REQUIRE(serialize_event(evt) == test.expected_serialized);
        }
    }

    SECTION("control fields with line breaks are not serialized") {
        event evt;
        evt.id = "safe\rid: injected";
        evt.type = "update\ndata: injected";
        evt.data = "payload";

        auto result = serialize_event(evt);

        REQUIRE(result == "data: payload\n\n");
    }

    SECTION("comments with line breaks remain comments") {
        auto result = detail::serialize_comment("first\rdata: injected\nid: bad");

        REQUIRE(result == ": first\n: data: injected\n: id: bad\n\n");
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

    SECTION("id-only block updates last_event_id without dispatch") {
        event_parser parser;

        REQUIRE(parser.parse("id: 42\n\n") == 0);

        REQUIRE_FALSE(parser.has_event());
        REQUIRE(parser.last_event_id() == "42");
    }

    SECTION("empty id clears last_event_id without dispatch") {
        event_parser parser;

        REQUIRE(parser.parse("id: 123\ndata: first\n\n") == 1);
        REQUIRE(parser.has_event());
        parser.get_event();
        REQUIRE(parser.last_event_id() == "123");

        REQUIRE(parser.parse("id:\n\n") == 0);

        REQUIRE_FALSE(parser.has_event());
        REQUIRE(parser.last_event_id().empty());
    }

    SECTION("empty id on dispatched event clears last_event_id") {
        event_parser parser;

        REQUIRE(parser.parse("id: 123\ndata: first\n\n") == 1);
        REQUIRE(parser.has_event());
        parser.get_event();
        REQUIRE(parser.last_event_id() == "123");

        REQUIRE(parser.parse("id:\ndata: second\n\n") == 1);

        REQUIRE(parser.has_event());
        auto evt = parser.get_event();
        REQUIRE(evt.has_value());
        REQUIRE(evt->id.empty());
        REQUIRE(evt->data == "second");
        REQUIRE(parser.last_event_id().empty());
    }

    SECTION("initial last_event_id persists until an id field changes it") {
        event_parser parser(event_parser::default_max_buffer_size, "seed");

        REQUIRE(parser.parse("data: no id\n\n") == 1);
        REQUIRE(parser.has_event());
        parser.get_event();
        REQUIRE(parser.last_event_id() == "seed");

        REQUIRE(parser.parse("id:\n\n") == 0);

        REQUIRE_FALSE(parser.has_event());
        REQUIRE(parser.last_event_id().empty());
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

    SECTION("unterminated line over limit fails closed") {
        event_parser parser(8);

        parser.parse("data: 12");
        REQUIRE_FALSE(parser.failed());
        REQUIRE_FALSE(parser.has_event());

        parser.parse("3");
        REQUIRE(parser.failed());
        REQUIRE_FALSE(parser.has_event());
        REQUIRE_FALSE(parser.error_message().empty());
    }

    SECTION("line at limit can be completed by later terminator") {
        event_parser parser(8);

        parser.parse("data: ok");
        REQUIRE_FALSE(parser.failed());

        parser.parse("\n\n");
        REQUIRE_FALSE(parser.failed());
        REQUIRE(parser.has_event());
        auto evt = parser.get_event();
        REQUIRE(evt->data == "ok");
    }

    SECTION("large chunk with short lines is processed incrementally") {
        event_parser parser(8);

        parser.parse("data: a\n\ndata: b\n\n");
        REQUIRE_FALSE(parser.failed());

        REQUIRE(parser.has_event());
        auto evt1 = parser.get_event();
        REQUIRE(evt1->data == "a");

        REQUIRE(parser.has_event());
        auto evt2 = parser.get_event();
        REQUIRE(evt2->data == "b");
    }

    SECTION("terminated overlong line fails closed") {
        event_parser parser(8);

        parser.parse("data: 123\n\n");
        REQUIRE(parser.failed());
        REQUIRE_FALSE(parser.has_event());
        REQUIRE_FALSE(parser.error_message().empty());
    }

    SECTION("event data over limit fails closed") {
        event_parser parser(10);

        parser.parse("data: abc\n");
        REQUIRE_FALSE(parser.failed());
        parser.parse("data: def\n");
        REQUIRE_FALSE(parser.failed());

        parser.parse("data: ghi\n");
        REQUIRE(parser.failed());
        REQUIRE_FALSE(parser.has_event());
        REQUIRE_FALSE(parser.error_message().empty());
    }

    SECTION("reset clears parser limit failure") {
        event_parser parser(8);

        parser.parse("data: 123");
        REQUIRE(parser.failed());

        parser.reset();
        REQUIRE_FALSE(parser.failed());
        REQUIRE(parser.error_message().empty());

        parser.parse("data: ok\n\n");
        REQUIRE(parser.has_event());
        auto evt = parser.get_event();
        REQUIRE(evt->data == "ok");
    }
    
    SECTION("CRLF line endings") {
        event_parser parser;
        
        parser.parse("data: Hello\r\n\r\n");
        
        REQUIRE(parser.has_event());
        auto evt = parser.get_event();
        REQUIRE(evt->data == "Hello");
    }

    SECTION("CR-only line endings") {
        event_parser parser;

        parser.parse("data: Hello\r\r");

        REQUIRE(parser.has_event());
        auto evt = parser.get_event();
        REQUIRE(evt->data == "Hello");
    }

    SECTION("mixed CR and CRLF line endings") {
        event_parser parser;

        parser.parse("event: update\rdata: World\r\n\r\n");

        REQUIRE(parser.has_event());
        auto evt = parser.get_event();
        REQUIRE(evt->type == "update");
        REQUIRE(evt->data == "World");
    }

    SECTION("CR at buffer boundary followed by LF") {
        event_parser parser;

        // Feed in two chunks: first ends with \r, second starts with \n
        parser.parse("data: Hello\r");
        // \r at end of buffer is processed immediately as standalone CR.
        // This terminates the "data: Hello" line but no empty line yet,
        // so no event dispatched.
        REQUIRE_FALSE(parser.has_event());

        parser.parse("\n\r\n");
        // The leading \n belongs to the previous chunk's terminal \r, so it
        // must not create an empty line. The following \r\n is the blank line
        // that dispatches the event.
        REQUIRE(parser.has_event());
        auto evt = parser.get_event();
        REQUIRE(evt->data == "Hello");
    }

    SECTION("CRLF split across chunks preserves event metadata") {
        event_parser parser;

        REQUIRE(parser.parse("id: 42\r") == 0);
        REQUIRE_FALSE(parser.has_event());
        REQUIRE(parser.parse("") == 0);
        REQUIRE_FALSE(parser.has_event());

        REQUIRE(parser.parse("\nevent: update\r") == 0);
        REQUIRE_FALSE(parser.has_event());

        REQUIRE(parser.parse("\ndata: World\r\n\r\n") == 1);
        REQUIRE(parser.has_event());
        auto evt = parser.get_event();
        REQUIRE(evt->id == "42");
        REQUIRE(evt->type == "update");
        REQUIRE(evt->data == "World");
        REQUIRE(parser.last_event_id() == "42");
    }

    SECTION("same-chunk standalone CR does not suppress next chunk LF") {
        event_parser parser;

        REQUIRE(parser.parse("id: 42\rdata: World\n") == 0);
        REQUIRE_FALSE(parser.has_event());

        REQUIRE(parser.parse("\n") == 1);
        REQUIRE(parser.has_event());
        auto evt = parser.get_event();
        REQUIRE(evt->id == "42");
        REQUIRE(evt->data == "World");
        REQUIRE(parser.last_event_id() == "42");
    }

    SECTION("CR at buffer boundary followed by non-LF") {
        event_parser parser;

        // Feed in two chunks: first ends with \r (processed immediately as
        // standalone CR), second starts with a regular char.
        // Per SSE spec, the \r at end of buffer is treated as a line
        // terminator.  If the next chunk starts with a non-\n char, no
        // spurious empty line is produced.
        parser.parse("data: Hello\r");
        // \r terminates the "data: Hello" line but no empty line yet,
        // so no event dispatched.
        REQUIRE_FALSE(parser.has_event());

        // The next chunk starts with 'd', not '\n'.  The previous \r
        // already terminated the line, so this is a new line.
        // \r\r produces: "data: World" line + empty line (dispatches event).
        // The event accumulates both data lines: "Hello\nWorld".
        parser.parse("data: World\r\r");
        REQUIRE(parser.has_event());
        auto evt = parser.get_event();
        REQUIRE(evt->data == "Hello\nWorld");
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

    SECTION("serialize and parse CR-delimited data without field injection") {
        event original;
        original.data = "Line 1\rid: injected\r\nevent: injected\ndata: real";

        auto serialized = serialize_event(original);

        event_parser parser;
        parser.parse(serialized);

        REQUIRE(parser.has_event());
        auto parsed = parser.get_event();
        REQUIRE(parsed->id.empty());
        REQUIRE(parsed->type.empty());
        REQUIRE(parsed->data == "Line 1\nid: injected\nevent: injected\ndata: real");
    }

    SECTION("serialize and parse data with terminal line endings") {
        struct test_case {
            std::string input;
            std::string expected_data;
        };

        const std::vector<test_case> cases = {
            {"\n", "\n"},
            {"tail\n", "tail\n"},
            {"tail\r", "tail\n"},
            {"tail\r\n", "tail\n"},
            {"tail\n\n", "tail\n\n"},
        };

        for (const auto& test : cases) {
            CAPTURE(test.input);

            event original;
            original.data = test.input;

            auto serialized = serialize_event(original);

            event_parser parser;
            parser.parse(serialized);

            REQUIRE(parser.has_event());
            auto parsed = parser.get_event();
            REQUIRE(parsed->data == test.expected_data);
        }
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
//
//   4. SSE response header buffering must enforce the configured parser
//      limits before the final header delimiter arrives, and must not retain
//      the old fixed 8192-byte aggregate cap when callers configure larger
//      valid header lines.

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

TEST_CASE("sse_client rejects invalid outbound header config",
          "[sse][client][security]") {
    using namespace elio::runtime;

    scheduler sched(1);
    sched.start();

    std::atomic<bool> client_done{false};
    bool connected = true;
    int connect_errno = 0;

    sched.go([&]() -> elio::coro::task<void> {
        client_config cfg;
        cfg.auto_reconnect = false;
        cfg.last_event_id = "last\r\nInjected: yes";
        sse_client client(cfg);

        errno = 0;
        connected = co_await client.connect("http://127.0.0.1:1/events");
        connect_errno = errno;
        client_done = true;
        co_await client.close();
    });

    REQUIRE(wait_for([&] { return client_done.load(); }));
    sched.shutdown();

    REQUIRE_FALSE(connected);
    REQUIRE(connect_errno == EINVAL);
}

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

    constexpr int kRoundsPerSender = 25;

    struct sender_observation {
        int completed_rounds = 0;
        int failed_round = -1;
        std::exception_ptr exception;
    };

    struct server_observation {
        bool accepted = false;
        bool alpha_started = false;
        bool beta_started = false;
        bool stream_closed = false;
        sender_observation alpha;
        sender_observation beta;
        std::exception_ptr exception;
    };

    struct reader_observation {
        bool connected = false;
        bool read_completed = false;
        bool stream_closed = false;
        int terminal_read_result = 0;
        std::string captured;
        std::exception_ptr exception;
    };

    struct shared_session {
        explicit shared_session(tcp_stream&& accepted)
            : stream(std::move(accepted)), connection(&stream) {}

        tcp_stream stream;
        sse_connection connection;
    };

    struct root_state {
        std::mutex mutex;
        std::condition_variable changed;
        std::shared_ptr<shared_session> active_session;
        bool interruption_requested = false;
        bool server_done = false;
        bool reader_done = false;

        void publish_session(std::shared_ptr<shared_session> session) {
            std::lock_guard<std::mutex> lock(mutex);
            active_session = std::move(session);
            if (interruption_requested) {
                active_session->connection.close();
                active_session->stream.shutdown_socket();
            }
        }

        void retire_session(const std::shared_ptr<shared_session>& session) {
            std::lock_guard<std::mutex> lock(mutex);
            if (active_session == session) {
                active_session.reset();
            }
        }

        void interrupt_session() {
            std::lock_guard<std::mutex> lock(mutex);
            interruption_requested = true;
            if (active_session) {
                active_session->connection.close();
                active_session->stream.shutdown_socket();
            }
        }

        void mark_server_done() {
            {
                std::lock_guard<std::mutex> lock(mutex);
                server_done = true;
            }
            changed.notify_all();
        }

        void mark_reader_done() {
            {
                std::lock_guard<std::mutex> lock(mutex);
                reader_done = true;
            }
            changed.notify_all();
        }

        bool wait_for_roots(std::chrono::milliseconds timeout) {
            std::unique_lock<std::mutex> lock(mutex);
            return changed.wait_for(
                lock, timeout, [&] { return server_done && reader_done; });
        }
    };

    auto listener = std::make_shared<tcp_listener>(std::move(*listener_opt));
    auto roots = std::make_shared<root_state>();

    scheduler sched(2);
    sched.start();

    auto server_root = sched.go_joinable(
        [listener, roots]() -> coro::task<server_observation> {
            server_observation observed;
            std::shared_ptr<shared_session> session;
            try {
                auto accepted = co_await listener->accept(
                    coro::this_coro::cancel_token());
                observed.accepted = accepted.has_value();
                if (accepted) {
                    session = std::make_shared<shared_session>(
                        std::move(*accepted));
                    roots->publish_session(session);

                    // Two coroutines pump distinct events concurrently.
                    // Each child owns the session and returns an observation,
                    // so one failure cannot throw past the sibling join or
                    // invalidate the connection while the sibling is active.
                    auto sender =
                        [session, rounds = kRoundsPerSender](std::string type)
                            -> coro::task<sender_observation> {
                            sender_observation sender_observed;
                            try {
                                for (int i = 0; i < rounds; ++i) {
                                    std::string payload =
                                        type + ":" + std::to_string(i);
                                    bool ok = co_await session->connection.send_event(
                                        type, payload);
                                    if (!ok) {
                                        sender_observed.failed_round = i;
                                        co_return sender_observed;
                                    }
                                    ++sender_observed.completed_rounds;
                                }
                            } catch (...) {
                                sender_observed.exception =
                                    std::current_exception();
                            }
                            co_return sender_observed;
                        };

                    std::optional<coro::join_handle<sender_observation>> alpha;
                    std::optional<coro::join_handle<sender_observation>> beta;

                    try {
                        alpha.emplace(
                            elio::spawn(sender, std::string("alpha")));
                        observed.alpha_started = true;
                    } catch (...) {
                        observed.alpha.exception = std::current_exception();
                    }
                    try {
                        beta.emplace(elio::spawn(sender, std::string("beta")));
                        observed.beta_started = true;
                    } catch (...) {
                        observed.beta.exception = std::current_exception();
                    }

                    if (alpha) {
                        try {
                            observed.alpha = co_await std::move(*alpha);
                        } catch (...) {
                            observed.alpha.exception =
                                std::current_exception();
                        }
                    }
                    if (beta) {
                        try {
                            observed.beta = co_await std::move(*beta);
                        } catch (...) {
                            observed.beta.exception =
                                std::current_exception();
                        }
                    }
                }
            } catch (...) {
                observed.exception = std::current_exception();
            }

            if (session) {
                session->connection.close();
                roots->retire_session(session);
                try {
                    co_await session->stream.close();
                    observed.stream_closed = true;
                } catch (...) {
                    if (!observed.exception) {
                        observed.exception = std::current_exception();
                    }
                }
            }
            roots->mark_server_done();
            co_return observed;
        });

    auto reader_root = sched.go_joinable(
        [port, roots]() -> coro::task<reader_observation> {
            reader_observation observed;
            std::optional<tcp_stream> stream;
            try {
                auto token = coro::this_coro::cancel_token();
                auto connected = co_await tcp_connect(
                    ipv4_address("127.0.0.1", port), token);
                observed.connected = connected.has_value();
                if (connected) {
                    stream.emplace(std::move(*connected));
                    char buffer[1024];
                    constexpr size_t kCaptureLimit = 1u << 20;
                    while (observed.captured.size() < kCaptureLimit) {
                        auto result = co_await stream->read(
                            buffer, sizeof(buffer), token);
                        if (result.result <= 0) {
                            observed.terminal_read_result = result.result;
                            break;
                        }
                        observed.captured.append(
                            buffer, static_cast<size_t>(result.result));
                    }
                    observed.read_completed = true;
                }
            } catch (...) {
                observed.exception = std::current_exception();
            }

            if (stream) {
                try {
                    co_await stream->close();
                    observed.stream_closed = true;
                } catch (...) {
                    if (!observed.exception) {
                        observed.exception = std::current_exception();
                    }
                }
            }
            roots->mark_reader_done();
            co_return observed;
        });

    const bool roots_completed_before_cleanup = roots->wait_for_roots(
        elio::test::scaled_sec(10));

    server_root.request_cancel();
    reader_root.request_cancel();
    roots->interrupt_session();

    const bool roots_unwound = roots->wait_for_roots(
        elio::test::scaled_sec(10));
    const bool scheduler_stopped = sched.shutdown(elio::test::scaled_sec(10));
    const bool server_ready = server_root.is_ready();
    const bool server_destroyed = server_root.is_destroyed();
    const bool reader_ready = reader_root.is_ready();
    const bool reader_destroyed = reader_root.is_destroyed();

    std::optional<server_observation> server_observed;
    std::optional<reader_observation> reader_observed;
    std::exception_ptr server_join_exception;
    std::exception_ptr reader_join_exception;
    if (server_ready && server_destroyed) {
        try {
            server_observed.emplace(server_root.await_resume());
        } catch (...) {
            server_join_exception = std::current_exception();
        }
    }
    if (reader_ready && reader_destroyed) {
        try {
            reader_observed.emplace(reader_root.await_resume());
        } catch (...) {
            reader_join_exception = std::current_exception();
        }
    }
    if (server_destroyed) {
        listener->close();
    }

    auto exception_message = [](const std::exception_ptr& exception) {
        if (!exception) {
            return std::string{};
        }
        try {
            std::rethrow_exception(exception);
        } catch (const std::exception& error) {
            return std::string(error.what());
        } catch (...) {
            return std::string("non-standard exception");
        }
    };

    std::string server_exception;
    std::string alpha_exception;
    std::string beta_exception;
    std::string reader_exception;
    if (server_observed) {
        server_exception = exception_message(server_observed->exception);
        alpha_exception = exception_message(server_observed->alpha.exception);
        beta_exception = exception_message(server_observed->beta.exception);
    }
    if (reader_observed) {
        reader_exception = exception_message(reader_observed->exception);
    }
    const std::string server_join_error =
        exception_message(server_join_exception);
    const std::string reader_join_error =
        exception_message(reader_join_exception);

    CAPTURE(roots_completed_before_cleanup,
            roots_unwound,
            scheduler_stopped,
            server_ready,
            server_destroyed,
            reader_ready,
            reader_destroyed,
            server_exception,
            alpha_exception,
            beta_exception,
            reader_exception,
            server_join_error,
            reader_join_error);
    REQUIRE(roots_completed_before_cleanup);
    REQUIRE(roots_unwound);
    REQUIRE(scheduler_stopped);
    REQUIRE(server_ready);
    REQUIRE(server_destroyed);
    REQUIRE(reader_ready);
    REQUIRE(reader_destroyed);
    REQUIRE(server_join_exception == nullptr);
    REQUIRE(reader_join_exception == nullptr);
    REQUIRE(server_observed.has_value());
    REQUIRE(reader_observed.has_value());
    REQUIRE(server_observed->exception == nullptr);
    REQUIRE(reader_observed->exception == nullptr);
    REQUIRE(server_observed->accepted);
    REQUIRE(server_observed->alpha_started);
    REQUIRE(server_observed->beta_started);
    REQUIRE(server_observed->stream_closed);
    REQUIRE(reader_observed->connected);
    REQUIRE(reader_observed->read_completed);
    REQUIRE(reader_observed->stream_closed);
    REQUIRE(server_observed->alpha.exception == nullptr);
    REQUIRE(server_observed->alpha.failed_round == -1);
    REQUIRE(server_observed->alpha.completed_rounds == kRoundsPerSender);
    REQUIRE(server_observed->beta.exception == nullptr);
    REQUIRE(server_observed->beta.failed_round == -1);
    REQUIRE(server_observed->beta.completed_rounds == kRoundsPerSender);

    // Re-parse what landed on the wire.  Every byte should belong to one of
    // the well-formed events; if frames had been spliced, the parser would
    // still happily produce events but field values would be corrupted —
    // we therefore validate each event's data matches "<type>:<index>".
    event_parser parser;
    parser.parse(reader_observed->captured);

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

TEST_CASE("sse_client enforces configured response header limits before "
          "delimiter",
          "[sse][client][security][regression]") {
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
    std::atomic<bool> client_done{false};
    bool connected = true;
    int connect_errno = 0;

    sched.go([&]() -> coro::task<void> {
        auto server_stream = co_await listener_opt->accept();
        REQUIRE(server_stream.has_value());

        auto req = co_await read_request_headers(*server_stream);
        REQUIRE(!req.empty());

        std::string response =
            "HTTP/1.1 200 OK\r\n"
            "X-Too-Long: " + std::string(64, 'a');
        REQUIRE(co_await write_all(*server_stream, response));
        co_await elio::time::sleep_for(std::chrono::milliseconds(100));
        co_await server_stream->close();
        server_done = true;
    });

    sched.go([&]() -> coro::task<void> {
        client_config cfg;
        cfg.auto_reconnect = false;
        cfg.max_header_size = 16;
        sse_client client(cfg);
        std::string url =
            "http://127.0.0.1:" + std::to_string(port) + "/events";

        errno = 0;
        connected = co_await client.connect(url);
        connect_errno = errno;
        client_done = true;
        co_await client.close();
    });

    REQUIRE(wait_for([&] { return client_done.load() && server_done.load(); }));
    sched.shutdown();

    REQUIRE_FALSE(connected);
    REQUIRE(connect_errno == EMSGSIZE);
}

TEST_CASE("sse_client honors configured response header limits above 8192",
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
    std::atomic<bool> client_done{false};
    bool connected = false;
    bool got_event = false;
    std::string event_data;

    sched.go([&]() -> coro::task<void> {
        auto server_stream = co_await listener_opt->accept();
        REQUIRE(server_stream.has_value());

        auto req = co_await read_request_headers(*server_stream);
        REQUIRE(!req.empty());

        std::string response_prefix =
            "HTTP/1.1 200 OK\r\n"
            "X-Pad: " + std::string(8300, 'a');
        REQUIRE(co_await write_all(*server_stream, response_prefix));
        co_await elio::time::sleep_for(std::chrono::milliseconds(100));

        std::string response_suffix =
            "\r\n"
            "Content-Type: text/event-stream\r\n"
            "Cache-Control: no-cache\r\n"
            "\r\n"
            "event: ready\n"
            "data: ok\n"
            "\n";
        REQUIRE(co_await write_all(*server_stream, response_suffix));
        co_await elio::time::sleep_for(std::chrono::milliseconds(100));
        co_await server_stream->close();
        server_done = true;
    });

    sched.go([&]() -> coro::task<void> {
        client_config cfg;
        cfg.auto_reconnect = false;
        cfg.max_header_size = 9000;
        sse_client client(cfg);
        std::string url =
            "http://127.0.0.1:" + std::to_string(port) + "/events";

        connected = co_await client.connect(url);
        if (connected) {
            auto evt = co_await client.receive();
            got_event = evt.has_value();
            if (evt) {
                event_data = evt->data;
            }
        }
        client_done = true;
        co_await client.close();
    });

    REQUIRE(wait_for([&] { return client_done.load() && server_done.load(); }));
    sched.shutdown();

    REQUIRE(connected);
    REQUIRE(got_event);
    REQUIRE(event_data == "ok");
}

TEST_CASE("sse_client syncs id-only and empty Last-Event-ID updates",
          "[sse][client][regression]") {
    using namespace elio;
    using namespace elio::net;
    using namespace elio::runtime;
    namespace coro = elio::coro;

    struct session_result {
        bool server_done = false;
        bool client_done = false;
        bool server_failed = false;
        bool client_failed = false;
        bool connected = false;
        bool received_event = false;
        std::string request;
        std::string client_last_id = "unset";
    };

    auto run_session = [](std::string initial_last_id,
                          std::string body) -> session_result {
        auto listener_opt = tcp_listener::bind(ipv4_address("127.0.0.1", 0));
        REQUIRE(listener_opt.has_value());
        uint16_t port = listener_opt->local_address().port();

        scheduler sched(2);
        sched.start();

        std::atomic<bool> server_done{false};
        std::atomic<bool> client_done{false};
        std::atomic<bool> server_failed{false};
        std::atomic<bool> client_failed{false};
        session_result result;

        sched.go([&]() -> coro::task<void> {
            auto stream = co_await listener_opt->accept();
            if (!stream) {
                server_failed = true;
                co_return;
            }

            result.request = co_await read_request_headers(*stream);
            if (result.request.empty()) {
                server_failed = true;
                co_return;
            }

            std::string response =
                "HTTP/1.1 200 OK\r\n"
                "Content-Type: text/event-stream\r\n"
                "Cache-Control: no-cache\r\n"
                "\r\n";
            response += body;
            if (!co_await write_all(*stream, response)) {
                server_failed = true;
                co_return;
            }

            co_await stream->close();
            server_done = true;
        });

        sched.go([&]() -> coro::task<void> {
            client_config cfg;
            cfg.auto_reconnect = false;
            cfg.last_event_id = std::move(initial_last_id);
            sse_client client(cfg);
            std::string url =
                "http://127.0.0.1:" + std::to_string(port) + "/events";

            result.connected = co_await client.connect(url);
            if (!result.connected) {
                client_failed = true;
                client_done = true;
                co_return;
            }

            auto evt = co_await client.receive();
            result.received_event = evt.has_value();
            result.client_last_id = std::string(client.last_event_id());
            client_done = true;
            co_await client.close();
        });

        REQUIRE(wait_for([&] {
            return (client_done.load() && server_done.load()) ||
                   server_failed.load() || client_failed.load();
        }));
        sched.shutdown();

        result.server_done = server_done.load();
        result.client_done = client_done.load();
        result.server_failed = server_failed.load();
        result.client_failed = client_failed.load();
        return result;
    };

    auto id_only = run_session("", "id: 42\n\n");
    REQUIRE_FALSE(id_only.server_failed);
    REQUIRE_FALSE(id_only.client_failed);
    REQUIRE(id_only.server_done);
    REQUIRE(id_only.client_done);
    REQUIRE(id_only.connected);
    REQUIRE_FALSE(id_only.received_event);
    REQUIRE(id_only.request.find("Last-Event-ID:") == std::string::npos);
    REQUIRE(id_only.client_last_id == "42");

    auto empty_id = run_session("42", "id:\n\n");
    REQUIRE_FALSE(empty_id.server_failed);
    REQUIRE_FALSE(empty_id.client_failed);
    REQUIRE(empty_id.server_done);
    REQUIRE(empty_id.client_done);
    REQUIRE(empty_id.connected);
    REQUIRE_FALSE(empty_id.received_event);
    REQUIRE(empty_id.request.find("Last-Event-ID: 42\r\n") !=
            std::string::npos);
    REQUIRE(empty_id.client_last_id.empty());
}

TEST_CASE("sse_client rejects non-event-stream responses",
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
    std::atomic<bool> client_done{false};
    bool connected = true;
    int connect_errno = 0;

    sched.go([&]() -> coro::task<void> {
        auto server_stream = co_await listener_opt->accept();
        REQUIRE(server_stream.has_value());

        auto req = co_await read_request_headers(*server_stream);
        REQUIRE(!req.empty());

        std::string headers =
            "HTTP/1.1 200 OK\r\n"
            "Content-Type: application/json\r\n"
            "Cache-Control: no-cache\r\n"
            "\r\n";
        REQUIRE(co_await write_all(*server_stream, headers));
        co_await server_stream->close();
        server_done = true;
    });

    sched.go([&]() -> coro::task<void> {
        client_config cfg;
        cfg.auto_reconnect = false;
        sse_client client(cfg);
        std::string url =
            "http://127.0.0.1:" + std::to_string(port) + "/events";

        errno = 0;
        connected = co_await client.connect(url);
        connect_errno = errno;
        REQUIRE_FALSE(connected);
        REQUIRE(connect_errno == EBADMSG);
        REQUIRE_FALSE(client.is_connected());
        client_done = true;
        co_await client.close();
    });

    REQUIRE(wait_for([&] { return client_done.load() && server_done.load(); }));
    sched.shutdown();

    REQUIRE_FALSE(connected);
    REQUIRE(connect_errno == EBADMSG);
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
