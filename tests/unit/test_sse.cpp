#include <catch2/catch_test_macros.hpp>
#include <elio/http/sse.hpp>

#include <string>
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
