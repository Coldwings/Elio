#include <catch2/catch_test_macros.hpp>
#include <elio/http/http_parser.hpp>

#include <string>
#include <vector>

using namespace elio::http;

namespace {
struct decoded_response {
    std::string body;
    std::vector<response_event> events;
    size_t consumed = 0;
};

void drain(response_decoder& decoder, std::string_view input,
           decoded_response& output) {
    for (;;) {
        const auto result = decoder.decode(input);
        REQUIRE(result.consumed <= input.size());
        if (result.event == response_event::body) {
            REQUIRE_FALSE(result.body.empty());
            // Body is the suffix of accepted current input, never decoder
            // storage, including when framing and payload arrive together.
            REQUIRE(result.body.data() ==
                    input.data() + result.consumed - result.body.size());
            output.body += result.body;
        }
        output.consumed += result.consumed;
        input.remove_prefix(result.consumed);
        output.events.push_back(result.event);
        if (result.event == response_event::need_more ||
            result.event == response_event::error ||
            result.event == response_event::message_complete ||
            result.event == response_event::protocol_handoff) return;
    }
}
} // namespace

TEST_CASE("response decoder separates headers and borrows partial large chunks",
          "[http][decoder]") {
    response_decoder decoder;
    const std::string head =
        "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n";
    auto result = decoder.decode(head);
    REQUIRE(result.event == response_event::headers_complete);
    REQUIRE(result.consumed == head.size());
    REQUIRE(decoder.headers_complete());
    REQUIRE_FALSE(decoder.is_complete());

    const std::string first = "10000000; foo=\"a\\\"b\"\r\nabc";
    result = decoder.decode(first);
    REQUIRE(result.event == response_event::body);
    REQUIRE(result.consumed == first.size());
    REQUIRE(result.body == "abc");
    REQUIRE(result.body.data() == first.data() + first.size() - 3);
    REQUIRE(decoder.bytes_buffered() == 0);
    const std::string next = "def";
    result = decoder.decode(next);
    REQUIRE(result.body.data() == next.data());
    REQUIRE(result.body == "def");
    REQUIRE(decoder.bytes_buffered() == 0);
    REQUIRE(decoder.finish_eof().event == response_event::error);
}

TEST_CASE("response decoder survives every split and one-byte delivery",
          "[http][decoder]") {
    const std::vector<std::string> wires{
        "HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nhello",
        "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n"
        "2;foo=\"bar\"\r\nhe\r\n3\r\nllo\r\n0\r\nX-End: yes\r\n\r\n"
    };
    for (const auto& wire : wires) {
        for (size_t split = 0; split <= wire.size(); ++split) {
            CAPTURE(split, wire);
            response_decoder decoder;
            decoded_response output;
            drain(decoder, std::string_view(wire).substr(0, split), output);
            drain(decoder, std::string_view(wire).substr(split), output);
            REQUIRE(decoder.is_complete());
            REQUIRE(output.body == "hello");
            REQUIRE(output.consumed == wire.size());
        }
        response_decoder decoder;
        decoded_response output;
        for (const char& byte : wire) drain(decoder, {&byte, 1}, output);
        REQUIRE(decoder.is_complete());
        REQUIRE(output.body == "hello");
        REQUIRE(output.consumed == wire.size());
        REQUIRE(decoder.finish_eof().event == response_event::message_complete);
    }
}

TEST_CASE("response decoder retains next-message and handoff boundaries",
          "[http][decoder]") {
    const std::string first = "HTTP/1.1 100 Continue\r\n\r\n";
    const std::string final = "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok";
    const std::string wire = first + final;
    response_decoder decoder;
    decoded_response output;
    drain(decoder, wire, output);
    REQUIRE(output.consumed == first.size());
    REQUIRE(output.body.empty());
    REQUIRE(decoder.status_code() == 100);
    decoder.reset();
    output = {};
    drain(decoder, std::string_view(wire).substr(first.size()), output);
    REQUIRE(output.body == "ok");
    REQUIRE(output.consumed == final.size());

    for (const auto verb : {method::GET, method::CONNECT}) {
        decoder.reset();
        decoder.set_request_method(verb);
        const std::string head = verb == method::GET
            ? "HTTP/1.1 101 Switching Protocols\r\n\r\n"
            : "HTTP/1.1 200 OK\r\n\r\n";
        output = {};
        drain(decoder, head + "protocol-data", output);
        REQUIRE(output.consumed == head.size());
        REQUIRE(output.events.back() == response_event::protocol_handoff);
        REQUIRE(output.body.empty());
    }
}

TEST_CASE("response decoder delivers close-delimited data and rejects truncation",
          "[http][decoder]") {
    response_decoder decoder;
    decoded_response output;
    drain(decoder, "HTTP/1.1 200 OK\r\n\r\nhello", output);
    REQUIRE(output.body == "hello");
    REQUIRE(decoder.is_close_delimited());
    REQUIRE_FALSE(decoder.is_complete());
    REQUIRE(decoder.finish_eof().event == response_event::message_complete);
    REQUIRE(decoder.finish_eof().event == response_event::message_complete);

    for (const auto tail : {
             "Content-Length: 6\r\n\r\nhello",
             "Transfer-Encoding: chunked\r\n\r\n5\r\nhello",
             "Transfer-Encoding: chunked\r\n\r\n5\r\nhello\r",
             "Transfer-Encoding: chunked\r\n\r\n5\r\nhello\r\n0\r\n"}) {
        decoder.reset();
        output = {};
        drain(decoder, std::string("HTTP/1.1 200 OK\r\n") + tail, output);
        REQUIRE_FALSE(decoder.is_complete());
        REQUIRE(decoder.finish_eof().event == response_event::error);
        REQUIRE(decoder.finish_eof().event == response_event::error);
    }
}

TEST_CASE("response decoder resets message context but preserves configured limits",
          "[http][decoder]") {
    response_decoder decoder;
    decoder.set_max_headers(1);
    decoder.set_request_method(method::HEAD);
    const std::string head =
        "HTTP/1.1 200 OK\r\nContent-Length: 4\r\n\r\n";
    decoded_response output;
    drain(decoder, head + "NEXT", output);
    REQUIRE(output.body.empty());
    REQUIRE(output.consumed == head.size());
    REQUIRE(output.events.front() == response_event::headers_complete);
    REQUIRE(output.events.back() == response_event::message_complete);
    decoder.reset();
    REQUIRE_FALSE(decoder.headers_complete());
    output = {};
    drain(decoder, head + "body", output);
    REQUIRE(output.body == "body");
    REQUIRE(decoder.is_complete());
    decoder.reset();
    output = {};
    drain(decoder, "HTTP/1.1 200 OK\r\nX: a\r\nY: b\r\n\r\n", output);
    REQUIRE(decoder.has_error());
}

TEST_CASE("response decoder enforces exact metadata limit with split CRLF",
          "[http][decoder]") {
    const std::string head = "HTTP/1.1 200 OK\r\n";
    const std::string header = "X: " + std::string(13, 'a');
    response_decoder decoder;
    decoder.set_max_header_size(header.size());
    decoded_response output;
    drain(decoder, head + header + "\r", output);
    REQUIRE_FALSE(decoder.has_error());
    REQUIRE(decoder.bytes_buffered() == header.size() + 1);
    drain(decoder, "\n\r\n", output);
    REQUIRE(decoder.headers_complete());
    REQUIRE(decoder.bytes_buffered() == 0);
    REQUIRE(decoder.finish_eof().event == response_event::message_complete);
    decoder.reset();
    output = {};
    drain(decoder, head + header + "b", output);
    REQUIRE(decoder.has_error());
}

TEST_CASE("response decoder bounds fragmented metadata and validates framing",
          "[http][decoder]") {
    SECTION("status line is bounded before a terminator arrives") {
        response_decoder decoder;
        decoder.set_max_header_size(16);
        auto result = decoder.decode("HTTP/1.1 200 " + std::string(32, 'a'));
        REQUIRE(result.event == response_event::error);
        REQUIRE(decoder.bytes_buffered() <= 17);
    }
    SECTION("duplicate headers count toward limits") {
        response_decoder decoder;
        decoder.set_max_headers(1);
        decoded_response output;
        drain(decoder, "HTTP/1.1 200 OK\r\nX: a\r\nX: b\r\n\r\n", output);
        REQUIRE(decoder.has_error());
    }
    for (const auto tail : {
             "Content-Length: 1\r\nTransfer-Encoding: chunked\r\n\r\n",
             "Transfer-Encoding: gzip, chunked\r\n\r\n",
             "Transfer-Encoding: chunked\r\n\r\n1\r\nxXX",
             "Transfer-Encoding: chunked\r\n\r\n1;bad=\"\r\n",
             "Transfer-Encoding: chunked\r\n\r\n0\r\nBad-Trailer\r\n\r\n"}) {
        response_decoder decoder;
        decoded_response output;
        drain(decoder, std::string("HTTP/1.1 200 OK\r\n") + tail, output);
        REQUIRE(decoder.has_error());
    }
}

TEST_CASE("response parser adapter preserves retired-byte and reset contracts",
          "[http][decoder]") {
    response_parser parser;
    const std::string start = "HTTP/1.1 200 OK\r\n";
    const std::string fragment = "Content-Length:";
    auto [first, used1] = parser.parse(start + fragment);
    REQUIRE(first == parse_result::need_more);
    REQUIRE(used1 == start.size());
    REQUIRE(parser.bytes_remaining() == fragment.size());
    auto [second, used2] = parser.parse(" 1\r\n\r\nxNEXT");
    REQUIRE(second == parse_result::complete);
    REQUIRE(used2 == fragment.size() + std::string_view(" 1\r\n\r\nx").size());
    REQUIRE(parser.body() == "x");
    REQUIRE(parser.take_remaining() == "NEXT");
    REQUIRE(parser.bytes_remaining() == 0);

    parser.reset();
    const std::string reply = "HTTP/1.1 200 OK\r\nContent-Length: 0\r\n\r\n";
    parser.parse(reply + reply);
    REQUIRE(parser.bytes_remaining() == reply.size());
    parser.reset();
    auto [third, used3] = parser.parse({});
    REQUIRE(third == parse_result::complete);
    REQUIRE(used3 == reply.size());
    REQUIRE(parser.bytes_remaining() == 0);
}
