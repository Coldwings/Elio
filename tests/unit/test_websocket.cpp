#include <catch2/catch_test_macros.hpp>
#include <elio/http/websocket.hpp>

#include <cstdlib>
#include <new>
#include <string>
#include <type_traits>
#include <vector>

using namespace elio::http::websocket;

// ----------------------------------------------------------------------------
// Opt-in allocation tracking used by the WebSocket DoS regression test below.
//
// These replaceable global operators simply forward to malloc/free (so ASAN
// still intercepts them) and, only while ws_alloc_tracking is enabled, count
// how many allocations of at least ws_alloc_threshold bytes occur.  The flag is
// only toggled inside a single-threaded test section, so the plain counters are
// safe.
// ----------------------------------------------------------------------------
namespace {
bool   ws_alloc_tracking = false;
size_t ws_alloc_threshold = 0;
size_t ws_big_alloc_count = 0;

inline void* ws_tracked_alloc(std::size_t n) {
    if (ws_alloc_tracking && n >= ws_alloc_threshold) {
        ++ws_big_alloc_count;
    }
    void* p = std::malloc(n ? n : 1);
    if (!p) throw std::bad_alloc();
    return p;
}
} // namespace

void* operator new(std::size_t n) { return ws_tracked_alloc(n); }
void* operator new[](std::size_t n) { return ws_tracked_alloc(n); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p, std::size_t) noexcept { std::free(p); }
void operator delete[](void* p) noexcept { std::free(p); }
void operator delete[](void* p, std::size_t) noexcept { std::free(p); }


// ============================================================================
// WebSocket Frame Tests
// ============================================================================

TEST_CASE("WebSocket opcode helpers", "[websocket][frame]") {
    SECTION("is_control_frame") {
        REQUIRE_FALSE(is_control_frame(opcode::continuation));
        REQUIRE_FALSE(is_control_frame(opcode::text));
        REQUIRE_FALSE(is_control_frame(opcode::binary));
        REQUIRE(is_control_frame(opcode::close));
        REQUIRE(is_control_frame(opcode::ping));
        REQUIRE(is_control_frame(opcode::pong));
    }
    
    SECTION("is_valid_opcode") {
        REQUIRE(is_valid_opcode(0x0));  // continuation
        REQUIRE(is_valid_opcode(0x1));  // text
        REQUIRE(is_valid_opcode(0x2));  // binary
        REQUIRE_FALSE(is_valid_opcode(0x3));  // reserved
        REQUIRE_FALSE(is_valid_opcode(0x7));  // reserved
        REQUIRE(is_valid_opcode(0x8));  // close
        REQUIRE(is_valid_opcode(0x9));  // ping
        REQUIRE(is_valid_opcode(0xA));  // pong
        REQUIRE_FALSE(is_valid_opcode(0xB));  // reserved
        REQUIRE_FALSE(is_valid_opcode(0xF));  // reserved
    }
}

TEST_CASE("WebSocket close codes", "[websocket][frame]") {
    REQUIRE(close_reason(close_code::normal) == "Normal closure");
    REQUIRE(close_reason(close_code::going_away) == "Going away");
    REQUIRE(close_reason(close_code::protocol_error) == "Protocol error");
    REQUIRE(close_reason(close_code::too_large) == "Message too big");
}

TEST_CASE("WebSocket reserved close codes remapped on encode", "[websocket][frame]") {
    // RFC 6455 §7.4.1: 1005/1006/1015 MUST NOT appear on the wire; they must
    // be replaced with protocol_error (1002).
    auto wire_code = [](close_code c) -> uint16_t {
        auto frame = encode_close_frame(c, "", false);
        return (static_cast<uint16_t>(static_cast<uint8_t>(frame[2])) << 8) |
               static_cast<uint8_t>(frame[3]);
    };

    SECTION("no_status (1005) maps to protocol_error (1002)") {
        REQUIRE(wire_code(close_code::no_status) == 1002);
    }

    SECTION("abnormal (1006) maps to protocol_error (1002)") {
        REQUIRE(wire_code(close_code::abnormal) == 1002);
    }

    SECTION("raw 1015 maps to protocol_error (1002)") {
        REQUIRE(wire_code(static_cast<close_code>(1015)) == 1002);
    }

    SECTION("normal (1000) is unchanged") {
        REQUIRE(wire_code(close_code::normal) == 1000);
    }
}

TEST_CASE("WebSocket is_valid_close_code", "[websocket][frame]") {
    // Defined codes that must be accepted
    REQUIRE(is_valid_close_code(1000));  // normal
    REQUIRE(is_valid_close_code(1001));  // going_away
    REQUIRE(is_valid_close_code(1002));  // protocol_error
    REQUIRE(is_valid_close_code(1003));  // unsupported
    REQUIRE(is_valid_close_code(1007));  // invalid_data
    REQUIRE(is_valid_close_code(1008));  // policy_violation
    REQUIRE(is_valid_close_code(1009));  // too_large
    REQUIRE(is_valid_close_code(1010));  // extension_required
    REQUIRE(is_valid_close_code(1011));  // unexpected
    // RFC 6455 §7.4.1 registered codes 1012-1014
    REQUIRE(is_valid_close_code(1012));  // Service Restart
    REQUIRE(is_valid_close_code(1013));  // Try Again Later
    REQUIRE(is_valid_close_code(1014));  // Bad Gateway
    // Library/application range
    REQUIRE(is_valid_close_code(3000));
    REQUIRE(is_valid_close_code(4999));

    // Codes that must be rejected
    REQUIRE_FALSE(is_valid_close_code(0));
    REQUIRE_FALSE(is_valid_close_code(999));
    REQUIRE_FALSE(is_valid_close_code(1004));  // reserved
    REQUIRE_FALSE(is_valid_close_code(1005));  // no_status (internal)
    REQUIRE_FALSE(is_valid_close_code(1006));  // abnormal (internal)
    REQUIRE_FALSE(is_valid_close_code(1015));  // TLS (internal)
    REQUIRE_FALSE(is_valid_close_code(2999));
    REQUIRE_FALSE(is_valid_close_code(5000));
}

TEST_CASE("WebSocket frame encoding", "[websocket][frame]") {
    SECTION("encode simple text frame (unmasked)") {
        auto frame = encode_text_frame("Hello", false);
        
        REQUIRE(frame.size() == 7);  // 2 header + 5 payload
        REQUIRE((frame[0] & 0x0F) == 0x01);  // opcode = text
        REQUIRE((frame[0] & 0x80) != 0);     // FIN = 1
        REQUIRE((frame[1] & 0x80) == 0);     // MASK = 0
        REQUIRE((frame[1] & 0x7F) == 5);     // length = 5
        
        std::string payload(frame.begin() + 2, frame.end());
        REQUIRE(payload == "Hello");
    }
    
    SECTION("encode text frame (masked)") {
        auto frame = encode_text_frame("Hi", true);
        
        REQUIRE(frame.size() == 8);  // 2 header + 4 mask + 2 payload
        REQUIRE((frame[0] & 0x0F) == 0x01);  // opcode = text
        REQUIRE((frame[1] & 0x80) != 0);     // MASK = 1
        REQUIRE((frame[1] & 0x7F) == 2);     // length = 2
    }
    
    SECTION("encode medium-length frame (126 bytes)") {
        std::string data(200, 'x');
        auto frame = encode_text_frame(data, false);
        
        REQUIRE(frame.size() == 4 + 200);  // 2 header + 2 extended length + 200 payload
        REQUIRE((frame[1] & 0x7F) == 126);  // extended length marker
        
        uint16_t len = (static_cast<uint16_t>(frame[2]) << 8) | frame[3];
        REQUIRE(len == 200);
    }
    
    SECTION("encode binary frame") {
        std::string data = "\x00\x01\x02\x03";
        auto frame = encode_binary_frame(data, false);
        
        REQUIRE((frame[0] & 0x0F) == 0x02);  // opcode = binary
    }
    
    SECTION("encode close frame") {
        auto frame = encode_close_frame(close_code::normal, "bye", false);
        
        REQUIRE((frame[0] & 0x0F) == 0x08);  // opcode = close
        REQUIRE((frame[1] & 0x7F) == 5);     // 2 bytes code + 3 bytes reason
        
        // Check close code
        uint16_t code = (static_cast<uint16_t>(frame[2]) << 8) | frame[3];
        REQUIRE(code == 1000);
        
        // Check reason
        std::string reason(frame.begin() + 4, frame.end());
        REQUIRE(reason == "bye");
    }
    
    SECTION("encode ping frame") {
        auto frame = encode_ping_frame("test", false);
        
        REQUIRE((frame[0] & 0x0F) == 0x09);  // opcode = ping
        REQUIRE((frame[1] & 0x7F) == 4);     // length = 4
    }
    
    SECTION("encode pong frame") {
        auto frame = encode_pong_frame("test", false);
        
        REQUIRE((frame[0] & 0x0F) == 0x0A);  // opcode = pong
    }
}

TEST_CASE("WebSocket frame parsing", "[websocket][frame]") {
    SECTION("parse simple text frame") {
        auto encoded = encode_text_frame("Hello", false);
        
        frame_header header;
        auto result = parse_frame_header(encoded.data(), encoded.size(), header);
        
        REQUIRE(result.complete);
        REQUIRE_FALSE(result.error);
        REQUIRE(header.fin);
        REQUIRE(header.op == opcode::text);
        REQUIRE_FALSE(header.masked);
        REQUIRE(header.payload_len == 5);
        REQUIRE(result.header_size == 2);
        REQUIRE(result.frame_size == 7);
    }
    
    SECTION("parse masked frame") {
        auto encoded = encode_text_frame("Hi", true);
        
        frame_header header;
        auto result = parse_frame_header(encoded.data(), encoded.size(), header);
        
        REQUIRE(result.complete);
        REQUIRE(header.masked);
        REQUIRE(header.payload_len == 2);
        REQUIRE(result.header_size == 6);  // 2 base + 4 mask
    }
    
    SECTION("parse extended length frame (16-bit)") {
        std::string data(200, 'x');
        auto encoded = encode_text_frame(data, false);
        
        frame_header header;
        auto result = parse_frame_header(encoded.data(), encoded.size(), header);
        
        REQUIRE(result.complete);
        REQUIRE(header.payload_len == 200);
        REQUIRE(result.header_size == 4);  // 2 base + 2 extended
    }
    
    SECTION("parse incomplete header") {
        uint8_t partial[] = {0x81};  // Only first byte
        
        frame_header header;
        auto result = parse_frame_header(partial, 1, header);
        
        REQUIRE_FALSE(result.complete);
        REQUIRE_FALSE(result.error);
    }
    
    SECTION("reject invalid opcode") {
        uint8_t invalid[] = {0x83, 0x00};  // opcode 3 is reserved
        
        frame_header header;
        auto result = parse_frame_header(invalid, 2, header);
        
        REQUIRE(result.error);
    }
    
    SECTION("reject control frame with payload > 125") {
        // Manually construct invalid close frame
        uint8_t invalid[] = {0x88, 0x7E, 0x00, 0x80};  // Close with 128 byte payload
        
        frame_header header;
        auto result = parse_frame_header(invalid, 4, header);
        
        REQUIRE(result.error);
        REQUIRE(result.error_msg.find("too large") != std::string::npos);
    }
}

TEST_CASE("WebSocket masking", "[websocket][frame]") {
    SECTION("apply_mask is reversible") {
        std::string original = "Hello, World!";
        std::vector<uint8_t> data(original.begin(), original.end());
        uint8_t mask[4] = {0xAB, 0xCD, 0xEF, 0x12};
        
        // Mask
        apply_mask(data.data(), data.size(), mask);
        
        // Verify data changed
        std::string masked(data.begin(), data.end());
        REQUIRE(masked != original);
        
        // Unmask
        apply_mask(data.data(), data.size(), mask);
        
        // Verify data restored
        std::string unmasked(data.begin(), data.end());
        REQUIRE(unmasked == original);
    }
    
    SECTION("mask key generation is random") {
        auto key1 = generate_mask_key();
        auto key2 = generate_mask_key();
        
        // Very unlikely to be the same
        REQUIRE(key1 != key2);
    }
}

TEST_CASE("WebSocket frame parser", "[websocket][parser]") {
    SECTION("parse single text message") {
        frame_parser parser;
        auto frame = encode_text_frame("Hello", false);
        
        int consumed = parser.parse(frame.data(), frame.size());
        
        REQUIRE(consumed > 0);
        REQUIRE(parser.has_message());
        REQUIRE_FALSE(parser.has_error());
        
        auto msg = parser.get_message();
        REQUIRE(msg.has_value());
        REQUIRE(msg->type == opcode::text);
        REQUIRE(msg->data == "Hello");
        REQUIRE(msg->complete);
    }
    
    SECTION("parse binary message") {
        frame_parser parser;
        std::string data = "\x00\x01\x02\x03";
        auto frame = encode_binary_frame(data, false);
        
        parser.parse(frame.data(), frame.size());
        
        REQUIRE(parser.has_message());
        auto msg = parser.get_message();
        REQUIRE(msg->type == opcode::binary);
        REQUIRE(msg->data == data);
    }
    
    SECTION("parse control frame (ping)") {
        frame_parser parser;
        auto frame = encode_ping_frame("test", false);
        
        parser.parse(frame.data(), frame.size());
        
        REQUIRE_FALSE(parser.has_message());  // Not a data message
        REQUIRE(parser.has_control_frame());
        
        auto ctrl = parser.get_control_frame();
        REQUIRE(ctrl.has_value());
        REQUIRE(ctrl->first == opcode::ping);
        REQUIRE(ctrl->second == "test");
    }
    
    SECTION("parse multiple messages") {
        frame_parser parser;
        auto frame1 = encode_text_frame("First", false);
        auto frame2 = encode_text_frame("Second", false);
        
        std::vector<uint8_t> combined;
        combined.insert(combined.end(), frame1.begin(), frame1.end());
        combined.insert(combined.end(), frame2.begin(), frame2.end());
        
        parser.parse(combined.data(), combined.size());
        
        REQUIRE(parser.has_message());
        auto msg1 = parser.get_message();
        REQUIRE(msg1->data == "First");
        
        REQUIRE(parser.has_message());
        auto msg2 = parser.get_message();
        REQUIRE(msg2->data == "Second");
        
        REQUIRE_FALSE(parser.has_message());
    }
    
    SECTION("handle incremental parsing") {
        frame_parser parser;
        auto frame = encode_text_frame("Hello", false);
        
        // Feed one byte at a time
        for (size_t i = 0; i < frame.size(); ++i) {
            parser.parse(&frame[i], 1);
        }
        
        REQUIRE(parser.has_message());
        auto msg = parser.get_message();
        REQUIRE(msg->data == "Hello");
    }

    SECTION("reset clears partial message state") {
        frame_parser parser;

        frame_header partial_hdr;
        partial_hdr.op = opcode::binary;
        partial_hdr.fin = false;
        partial_hdr.masked = false;
        auto partial = encode_frame(partial_hdr, "partial");
        parser.parse(partial.data(), partial.size());

        parser.reset();

        auto complete = encode_binary_frame("fresh", false);
        auto consumed = parser.parse(complete.data(), complete.size());

        REQUIRE_FALSE(parser.has_error());
        REQUIRE(consumed == static_cast<ssize_t>(complete.size()));
        REQUIRE(parser.has_message());
        auto msg = parser.get_message();
        REQUIRE(msg.has_value());
        REQUIRE(msg->data == "fresh");
    }
    
    SECTION("max message size enforcement") {
        frame_parser parser;
        parser.set_max_message_size(10);

        auto frame = encode_text_frame("This message is too long!", false);
        parser.parse(frame.data(), frame.size());

        REQUIRE(parser.has_error());
        REQUIRE(parser.error().find("maximum size") != std::string::npos);
    }

    SECTION("oversized frame reports too_large close code") {
        frame_parser parser;
        parser.set_max_message_size(10);

        auto frame = encode_binary_frame(std::string(64, 'x'), false);
        auto consumed = parser.parse(frame.data(), frame.size());

        REQUIRE(parser.has_error());
        REQUIRE(parser.error_close_code() == close_code::too_large);
        REQUIRE(consumed < 0);
    }

    SECTION("oversized frame rejected after header before payload arrives") {
        frame_parser parser;
        parser.set_max_message_size(10);

        auto frame = encode_binary_frame(std::string(64, 'x'), false);
        const auto header_size = frame.size() - 64;
        auto consumed = parser.parse(frame.data(), header_size);

        REQUIRE(parser.has_error());
        REQUIRE(parser.error_close_code() == close_code::too_large);
        REQUIRE(consumed < 0);
    }

    SECTION("oversized frame rejected before allocating payload") {
        // Regression test for the DoS bug where the parser copied the full frame
        // payload into a std::string *before* enforcing max_message_size.
        //
        // We feed one complete, oversized data frame.  parse() first copies the
        // frame into its internal buffer_ (one large allocation, unavoidable).
        // The buggy code then built a second payload-sized std::string before the
        // size check; the fixed code rejects first, so only ONE allocation at or
        // above the payload size must occur.
        const size_t limit = 1024;
        const size_t payload_bytes = 4u * 1024 * 1024;  // 4 MiB, well above limit

        auto frame = encode_binary_frame(std::string(payload_bytes, 'x'), false);

        frame_parser parser;
        parser.set_max_message_size(limit);

        ws_big_alloc_count = 0;
        ws_alloc_threshold = payload_bytes;  // only count payload-sized allocations
        ws_alloc_tracking = true;
        auto consumed = parser.parse(frame.data(), frame.size());
        ws_alloc_tracking = false;

        REQUIRE(parser.has_error());
        REQUIRE(parser.error_close_code() == close_code::too_large);
        REQUIRE(consumed < 0);
        // Only buffer_ should allocate at/above the payload size.  A second such
        // allocation means the payload was materialised before the size check.
        REQUIRE(ws_big_alloc_count == 1);
    }

    SECTION("fragmented message rejected before exceeding limit") {
        // First fragment fits under the limit; the continuation fragment would
        // push the accumulated message over it and must be rejected using the
        // frame header length before its payload is appended.
        frame_parser parser;
        parser.set_max_message_size(10);

        frame_header hdr1;
        hdr1.op = opcode::binary;
        hdr1.fin = false;
        hdr1.masked = false;
        auto frag1 = encode_frame(hdr1, std::string(6, 'a'));  // 6 bytes, under 10

        frame_header hdr2;
        hdr2.op = opcode::continuation;
        hdr2.fin = true;
        hdr2.masked = false;
        auto frag2 = encode_frame(hdr2, std::string(8, 'b'));  // 6 + 8 = 14 > 10

        parser.parse(frag1.data(), frag1.size());
        REQUIRE_FALSE(parser.has_error());
        REQUIRE_FALSE(parser.has_message());

        auto consumed = parser.parse(frag2.data(), frag2.size());
        REQUIRE(parser.has_error());
        REQUIRE(parser.error_close_code() == close_code::too_large);
        REQUIRE(consumed < 0);
    }

    SECTION("fragmented continuation rejected after header before payload arrives") {
        frame_parser parser;
        parser.set_max_message_size(10);

        frame_header hdr1;
        hdr1.op = opcode::binary;
        hdr1.fin = false;
        hdr1.masked = false;
        auto frag1 = encode_frame(hdr1, std::string(6, 'a'));  // 6 bytes, under 10

        frame_header hdr2;
        hdr2.op = opcode::continuation;
        hdr2.fin = true;
        hdr2.masked = false;
        auto frag2 = encode_frame(hdr2, std::string(8, 'b'));  // 6 + 8 = 14 > 10
        const auto frag2_header_size = frag2.size() - 8;

        parser.parse(frag1.data(), frag1.size());
        REQUIRE_FALSE(parser.has_error());
        REQUIRE_FALSE(parser.has_message());

        auto consumed = parser.parse(frag2.data(), frag2_header_size);
        REQUIRE(parser.has_error());
        REQUIRE(parser.error_close_code() == close_code::too_large);
        REQUIRE(consumed < 0);
    }

    SECTION("payload exactly at the limit is accepted") {
        frame_parser parser;
        parser.set_max_message_size(5);

        auto frame = encode_binary_frame(std::string(5, 'z'), false);
        auto consumed = parser.parse(frame.data(), frame.size());

        REQUIRE_FALSE(parser.has_error());
        REQUIRE(consumed == static_cast<ssize_t>(frame.size()));
        auto msg = parser.get_message();
        REQUIRE(msg.has_value());
        REQUIRE(msg->data.size() == 5);
    }

    SECTION("parse returns a wide signed byte count") {
        // parse() must return a signed type wide enough that a large consumed
        // byte count cannot overflow into the negative error sentinel.
        frame_parser parser;
        auto frame = encode_text_frame("Hello", false);
        auto consumed = parser.parse(frame.data(), frame.size());

        static_assert(std::is_signed_v<decltype(consumed)>,
                      "frame_parser::parse() must return a signed type");
        static_assert(sizeof(decltype(consumed)) >= sizeof(size_t),
                      "frame_parser::parse() return type must be as wide as size_t");
        REQUIRE(consumed == static_cast<ssize_t>(frame.size()));
    }

    SECTION("invalid UTF-8 text frame rejected") {
        frame_parser parser;
        // 0xFF is never a valid UTF-8 byte
        std::string bad_utf8 = "hello\xFF world";
        auto frame = encode_text_frame(bad_utf8, false);
        parser.parse(frame.data(), frame.size());

        REQUIRE(parser.has_error());
        REQUIRE(parser.error_close_code() == close_code::invalid_data);
    }

    SECTION("UTF-16 surrogate in text frame rejected") {
        frame_parser parser;
        // ED A0 80 encodes U+D800 (a high surrogate), invalid in UTF-8 per RFC 3629
        std::string surrogate = "\xED\xA0\x80";
        auto frame = encode_text_frame(surrogate, false);
        parser.parse(frame.data(), frame.size());

        REQUIRE(parser.has_error());
        REQUIRE(parser.error_close_code() == close_code::invalid_data);
    }

    SECTION("invalid UTF-8 in fragmented text message rejected") {
        frame_parser parser;
        // First fragment: text frame with fin=false
        frame_header hdr1;
        hdr1.op = opcode::text;
        hdr1.fin = false;
        hdr1.masked = false;
        auto frag1 = encode_frame(hdr1, "valid ");
        // Second fragment: continuation frame with fin=true, containing invalid UTF-8
        frame_header hdr2;
        hdr2.op = opcode::continuation;
        hdr2.fin = true;
        hdr2.masked = false;
        std::string bad = "bad\xFF";
        auto frag2 = encode_frame(hdr2, bad);

        parser.parse(frag1.data(), frag1.size());
        REQUIRE_FALSE(parser.has_error());
        REQUIRE_FALSE(parser.has_message());

        parser.parse(frag2.data(), frag2.size());
        REQUIRE(parser.has_error());
        REQUIRE(parser.error_close_code() == close_code::invalid_data);
    }
}

TEST_CASE("WebSocket close payload parsing", "[websocket][frame]") {
    SECTION("parse close with code and reason") {
        std::string payload;
        payload += static_cast<char>(0x03);  // 1000 >> 8
        payload += static_cast<char>(0xE8);  // 1000 & 0xFF
        payload += "goodbye";
        
        auto [code, reason] = parse_close_payload(payload);
        
        REQUIRE(code == close_code::normal);
        REQUIRE(reason == "goodbye");
    }
    
    SECTION("parse close with only code") {
        std::string payload;
        payload += static_cast<char>(0x03);
        payload += static_cast<char>(0xEA);  // 1002
        
        auto [code, reason] = parse_close_payload(payload);
        
        REQUIRE(code == close_code::protocol_error);
        REQUIRE(reason.empty());
    }
    
    SECTION("parse empty close payload") {
        auto [code, reason] = parse_close_payload("");
        
        REQUIRE(code == close_code::no_status);
        REQUIRE(reason.empty());
    }
}

// ============================================================================
// WebSocket Handshake Tests
// ============================================================================

TEST_CASE("WebSocket base64 encoding", "[websocket][handshake]") {
    SECTION("encode empty") {
        REQUIRE(base64_encode(nullptr, 0) == "");
    }
    
    SECTION("encode 'f'") {
        const uint8_t data[] = {'f'};
        REQUIRE(base64_encode(data, 1) == "Zg==");
    }
    
    SECTION("encode 'fo'") {
        const uint8_t data[] = {'f', 'o'};
        REQUIRE(base64_encode(data, 2) == "Zm8=");
    }
    
    SECTION("encode 'foo'") {
        const uint8_t data[] = {'f', 'o', 'o'};
        REQUIRE(base64_encode(data, 3) == "Zm9v");
    }
    
    SECTION("encode 'Hello'") {
        const uint8_t data[] = {'H', 'e', 'l', 'l', 'o'};
        REQUIRE(base64_encode(data, 5) == "SGVsbG8=");
    }
}

TEST_CASE("WebSocket base64 decoding", "[websocket][handshake]") {
    SECTION("decode empty") {
        REQUIRE(base64_decode("") == "");
    }
    
    SECTION("decode 'Zg=='") {
        REQUIRE(base64_decode("Zg==") == "f");
    }
    
    SECTION("decode 'Zm8='") {
        REQUIRE(base64_decode("Zm8=") == "fo");
    }
    
    SECTION("decode 'Zm9v'") {
        REQUIRE(base64_decode("Zm9v") == "foo");
    }
    
    SECTION("decode 'SGVsbG8='") {
        REQUIRE(base64_decode("SGVsbG8=") == "Hello");
    }
}

TEST_CASE("WebSocket key validation", "[websocket][handshake]") {
    SECTION("valid key decodes to 16-byte nonce") {
        REQUIRE(is_valid_websocket_key("dGhlIHNhbXBsZSBub25jZQ=="));
    }

    SECTION("reject wrong encoded length") {
        REQUIRE_FALSE(is_valid_websocket_key("dGhlIHNhbXBsZSBub25jZQ="));
    }

    SECTION("reject invalid base64 characters") {
        REQUIRE_FALSE(is_valid_websocket_key("dGhlIHNhbXBsZSBub25jZQ!="));
    }

    SECTION("reject misplaced padding") {
        REQUIRE_FALSE(is_valid_websocket_key("dGhlIHNhbXBsZSBub25jZQ=A"));
    }

    SECTION("reject decoded length other than 16 bytes") {
        REQUIRE_FALSE(is_valid_websocket_key("AAAAAAAAAAAAAAAAAAAAAAAA"));
    }
}

TEST_CASE("WebSocket key generation", "[websocket][handshake]") {
    SECTION("generated key is base64 encoded 16 bytes") {
        auto key = generate_websocket_key();
        
        // 16 bytes = 24 base64 characters (with padding)
        REQUIRE(key.size() == 24);
        
        // Verify it decodes to 16 bytes
        auto decoded = base64_decode(key);
        REQUIRE(decoded.size() == 16);
    }
    
    SECTION("generated keys are unique") {
        auto key1 = generate_websocket_key();
        auto key2 = generate_websocket_key();
        
        REQUIRE(key1 != key2);
    }
}

TEST_CASE("WebSocket accept computation", "[websocket][handshake]") {
    SECTION("RFC 6455 example") {
        // Example from RFC 6455 Section 1.3
        std::string key = "dGhlIHNhbXBsZSBub25jZQ==";
        std::string expected = "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=";
        
        REQUIRE(compute_websocket_accept(key) == expected);
    }
    
    SECTION("verify_websocket_accept") {
        std::string key = "dGhlIHNhbXBsZSBub25jZQ==";
        std::string accept = "s3pPLMBiTxaQ9kYGzzhZRbK+xOo=";
        
        REQUIRE(verify_websocket_accept(accept, key));
        REQUIRE_FALSE(verify_websocket_accept("invalid", key));
    }
}

TEST_CASE("WebSocket upgrade validation", "[websocket][handshake]") {
    SECTION("valid upgrade request") {
        REQUIRE(is_websocket_upgrade("GET", "websocket", "Upgrade", "13"));
        REQUIRE(is_websocket_upgrade("GET", "WebSocket", "upgrade", "13"));
        REQUIRE(is_websocket_upgrade("GET", "websocket", "keep-alive, Upgrade", "13"));
        REQUIRE(is_websocket_upgrade("GET", "h2c, WebSocket", "keep-alive, upgrade", "13"));
    }
    
    SECTION("invalid method") {
        REQUIRE_FALSE(is_websocket_upgrade("POST", "websocket", "Upgrade", "13"));
    }
    
    SECTION("missing upgrade header") {
        REQUIRE_FALSE(is_websocket_upgrade("GET", "http", "Upgrade", "13"));
    }

    SECTION("upgrade header requires exact websocket token") {
        REQUIRE_FALSE(is_websocket_upgrade("GET", "xwebsocket", "Upgrade", "13"));
        REQUIRE_FALSE(is_websocket_upgrade("GET", "websocket2", "Upgrade", "13"));
        REQUIRE_FALSE(is_websocket_upgrade("GET", "h2c, xwebsocket", "Upgrade", "13"));
    }
    
    SECTION("missing connection upgrade") {
        REQUIRE_FALSE(is_websocket_upgrade("GET", "websocket", "close", "13"));
    }

    SECTION("connection header requires exact upgrade token") {
        REQUIRE_FALSE(is_websocket_upgrade("GET", "websocket", "xupgrade", "13"));
        REQUIRE_FALSE(is_websocket_upgrade("GET", "websocket", "upgraded", "13"));
        REQUIRE_FALSE(is_websocket_upgrade("GET", "websocket", "keep-alive, xupgrade", "13"));
    }
    
    SECTION("wrong version") {
        REQUIRE_FALSE(is_websocket_upgrade("GET", "websocket", "Upgrade", "12"));
    }
}

TEST_CASE("WebSocket server upgrade request validation", "[websocket][handshake]") {
    auto make_request = [](std::string_view upgrade,
                           std::string_view connection,
                           std::string_view key) {
        elio::http::request req(elio::http::method::GET, "/ws");
        req.set_header("Upgrade", upgrade);
        req.set_header("Connection", connection);
        req.set_header("Sec-WebSocket-Key", key);
        req.set_header("Sec-WebSocket-Version", "13");
        return req;
    };

    server_config config;

    SECTION("accept valid request") {
        auto req = make_request("websocket", "keep-alive, Upgrade",
                                "dGhlIHNhbXBsZSBub25jZQ==");
        auto result = validate_upgrade_request(req, config);
        REQUIRE(result.success);
    }

    SECTION("reject malformed upgrade and connection tokens") {
        auto bad_upgrade = make_request("xwebsocket", "Upgrade",
                                        "dGhlIHNhbXBsZSBub25jZQ==");
        REQUIRE_FALSE(validate_upgrade_request(bad_upgrade, config).success);

        auto bad_connection = make_request("websocket", "keep-alive, xupgrade",
                                           "dGhlIHNhbXBsZSBub25jZQ==");
        REQUIRE_FALSE(validate_upgrade_request(bad_connection, config).success);
    }

    SECTION("reject invalid Sec-WebSocket-Key values") {
        auto invalid_chars = make_request("websocket", "Upgrade",
                                          "dGhlIHNhbXBsZSBub25jZQ!=");
        REQUIRE_FALSE(validate_upgrade_request(invalid_chars, config).success);

        auto invalid_padding = make_request("websocket", "Upgrade",
                                            "dGhlIHNhbXBsZSBub25jZQ=A");
        REQUIRE_FALSE(validate_upgrade_request(invalid_padding, config).success);

        auto wrong_decoded_length = make_request("websocket", "Upgrade",
                                                 "AAAAAAAAAAAAAAAAAAAAAAAA");
        REQUIRE_FALSE(validate_upgrade_request(wrong_decoded_length, config).success);
    }
}

TEST_CASE("WebSocket protocol negotiation", "[websocket][handshake]") {
    SECTION("parse protocols") {
        auto protos = parse_protocols("chat, superchat");
        
        REQUIRE(protos.size() == 2);
        REQUIRE(protos[0] == "chat");
        REQUIRE(protos[1] == "superchat");
    }
    
    SECTION("parse protocols with extra whitespace") {
        auto protos = parse_protocols("  chat  ,  superchat  ");
        
        REQUIRE(protos.size() == 2);
        REQUIRE(protos[0] == "chat");
        REQUIRE(protos[1] == "superchat");
    }
    
    SECTION("negotiate protocol - match found") {
        std::vector<std::string> client = {"chat", "superchat"};
        std::vector<std::string> server = {"superchat", "v1"};
        
        auto result = negotiate_protocol(client, server);
        REQUIRE(result == "superchat");
    }
    
    SECTION("negotiate protocol - no match") {
        std::vector<std::string> client = {"chat"};
        std::vector<std::string> server = {"v1", "v2"};
        
        auto result = negotiate_protocol(client, server);
        REQUIRE(result.empty());
    }
}

TEST_CASE("WebSocket handshake building", "[websocket][handshake]") {
    SECTION("build client handshake") {
        auto request = build_client_handshake("example.com", "/ws", 
                                               "dGhlIHNhbXBsZSBub25jZQ==",
                                               {"chat"});
        
        REQUIRE(request.find("GET /ws HTTP/1.1\r\n") != std::string::npos);
        REQUIRE(request.find("Host: example.com\r\n") != std::string::npos);
        REQUIRE(request.find("Upgrade: websocket\r\n") != std::string::npos);
        REQUIRE(request.find("Connection: Upgrade\r\n") != std::string::npos);
        REQUIRE(request.find("Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n") != std::string::npos);
        REQUIRE(request.find("Sec-WebSocket-Version: 13\r\n") != std::string::npos);
        REQUIRE(request.find("Sec-WebSocket-Protocol: chat\r\n") != std::string::npos);
    }
    
    SECTION("build server handshake") {
        auto response = build_server_handshake("s3pPLMBiTxaQ9kYGzzhZRbK+xOo=", "chat");
        
        REQUIRE(response.find("HTTP/1.1 101") != std::string::npos);
        REQUIRE(response.find("Upgrade: websocket\r\n") != std::string::npos);
        REQUIRE(response.find("Connection: Upgrade\r\n") != std::string::npos);
        REQUIRE(response.find("Sec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=\r\n") != std::string::npos);
        REQUIRE(response.find("Sec-WebSocket-Protocol: chat\r\n") != std::string::npos);
    }
    
    SECTION("build rejection response") {
        auto response = build_rejection_response(400, "Bad Request");
        
        REQUIRE(response.find("HTTP/1.1 400 Bad Request\r\n") != std::string::npos);
        REQUIRE(response.find("Connection: close\r\n") != std::string::npos);
    }
}
