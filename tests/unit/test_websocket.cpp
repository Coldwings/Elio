#include <catch2/catch_test_macros.hpp>
#include <elio/http/websocket.hpp>

#include <string>
#include <vector>
#include <cstring>

using namespace elio::http::websocket;

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
    
    SECTION("max message size enforcement") {
        frame_parser parser;
        parser.set_max_message_size(10);
        
        auto frame = encode_text_frame("This message is too long!", false);
        parser.parse(frame.data(), frame.size());
        
        REQUIRE(parser.has_error());
        REQUIRE(parser.error().find("maximum size") != std::string::npos);
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
    }
    
    SECTION("invalid method") {
        REQUIRE_FALSE(is_websocket_upgrade("POST", "websocket", "Upgrade", "13"));
    }
    
    SECTION("missing upgrade header") {
        REQUIRE_FALSE(is_websocket_upgrade("GET", "http", "Upgrade", "13"));
    }
    
    SECTION("missing connection upgrade") {
        REQUIRE_FALSE(is_websocket_upgrade("GET", "websocket", "close", "13"));
    }
    
    SECTION("wrong version") {
        REQUIRE_FALSE(is_websocket_upgrade("GET", "websocket", "Upgrade", "12"));
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
