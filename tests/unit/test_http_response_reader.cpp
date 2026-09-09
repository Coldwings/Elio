#include <catch2/catch_test_macros.hpp>
#include <elio/elio.hpp>
#include <elio/http/http_response_reader.hpp>

#include <algorithm>
#include <cstring>
#include <string>

namespace {

struct response_test_stream {
    std::string wire;
    size_t offset = 0;
    size_t fragment_size = 8192;
    size_t reads = 0;
    bool interrupt_once = false;
    const char* last_buffer = nullptr;
    size_t last_size = 0;

    elio::coro::task<elio::io::io_result> read(
        void* data, size_t size, elio::coro::cancel_token token) {
        ++reads;
        if (token.is_cancelled()) co_return elio::io::io_result{-ECANCELED, 0};
        if (interrupt_once) {
            interrupt_once = false;
            co_return elio::io::io_result{-EINTR, 0};
        }
        size = std::min({size, fragment_size, wire.size() - offset});
        std::memcpy(data, wire.data() + offset, size);
        offset += size;
        last_buffer = static_cast<const char*>(data);
        last_size = size;
        co_return elio::io::io_result{static_cast<int32_t>(size), 0};
    }
};

template<typename T>
T complete_inline(elio::coro::task<T> task) {
    auto handle = elio::coro::detail::task_access::handle(task);
    handle.resume();
    REQUIRE(handle.done());
    return task.await_resume();
}

} // namespace

TEST_CASE("HTTP response reader borrows body and retains the next message",
          "[http][reader][issue-1192]") {
    using namespace elio::http;
    response_test_stream stream{
        "HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nhello"
        "HTTP/1.1 204 No Content\r\n\r\n"};
    response_reader reader;
    REQUIRE(complete_inline(reader.read(stream)).event == response_event::headers_complete);
    auto body = complete_inline(reader.read(stream));
    REQUIRE(body.event == response_event::body);
    REQUIRE(body.body == "hello");
    REQUIRE(body.body.data() >= stream.last_buffer);
    REQUIRE(body.body.data() + body.body.size() <= stream.last_buffer + stream.last_size);
    REQUIRE(stream.reads == 1);
    REQUIRE_FALSE(reader.next_response());
    REQUIRE(complete_inline(reader.read(stream)).event == response_event::message_complete);
    REQUIRE(reader.next_response());
    REQUIRE(complete_inline(reader.read(stream)).event == response_event::headers_complete);
    REQUIRE(reader.decoder().status_code() == 204);
    REQUIRE(complete_inline(reader.read(stream)).event == response_event::message_complete);
    REQUIRE(reader.bytes_remaining() == 0);
    REQUIRE(stream.reads == 1);
}

TEST_CASE("HTTP response reader decodes one-byte fragments and EINTR",
          "[http][reader][issue-1192]") {
    using namespace elio::http;
    response_test_stream stream{
        "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n"
        "2\r\nhe\r\n3\r\nllo\r\n0\r\n\r\n"};
    stream.fragment_size = 1;
    stream.interrupt_once = true;
    response_reader reader(4);
    REQUIRE(complete_inline(reader.read(stream)).event == response_event::headers_complete);
    std::string body;
    for (;;) {
        auto part = complete_inline(reader.read(stream));
        REQUIRE(part.success());
        if (part.event == response_event::message_complete) break;
        REQUIRE(part.event == response_event::body);
        body += part.body;
    }
    REQUIRE(body == "hello");
    REQUIRE_FALSE(reader.reached_eof());
}

TEST_CASE("HTTP response reader distinguishes closing body from truncation",
          "[http][reader][issue-1192]") {
    using namespace elio::http;
    for (bool framed : {false, true}) {
        response_test_stream stream{
            std::string("HTTP/1.1 417 Rejected\r\n") +
            (framed ? "Content-Length: 99\r\n" : "Connection: close\r\n") +
            "\r\nno"};
        response_reader reader;
        REQUIRE(complete_inline(reader.read(stream)).event == response_event::headers_complete);
        REQUIRE(complete_inline(reader.read(stream)).body == "no");
        auto end = complete_inline(reader.read(stream));
        REQUIRE(reader.reached_eof());
        REQUIRE(end.event == (framed ? response_event::error : response_event::message_complete));
        REQUIRE(end.error == (framed ? EBADMSG : 0));
    }
}

TEST_CASE("HTTP response reader leaves upgrade bytes unparsed",
          "[http][reader][issue-1192]") {
    using namespace elio::http;
    response_test_stream stream{"HTTP/1.1 101 Switching Protocols\r\n\r\nframe"};
    response_reader reader;
    REQUIRE(complete_inline(reader.read(stream)).event == response_event::headers_complete);
    REQUIRE(complete_inline(reader.read(stream)).event == response_event::protocol_handoff);
    REQUIRE(reader.remaining() == "frame");
    REQUIRE_FALSE(reader.next_response());
}

TEST_CASE("HTTP response reader retains partial framing across an interrupted read",
          "[http][reader][issue-1192]") {
    using namespace elio::http;
    response_reader reader;
    int step = 0;
    auto receive = [&](void* data, size_t) -> elio::coro::task<elio::io::io_result> {
        ++step;
        if (step == 2) co_return elio::io::io_result{-ECANCELED, 0};
        std::string_view wire = step == 1
            ? "HTTP/1.1 200 OK\r\nContent-Len"
            : "gth: 2\r\n\r\nokNEXT";
        std::memcpy(data, wire.data(), wire.size());
        co_return elio::io::io_result{static_cast<int32_t>(wire.size()), 0};
    };
    auto interrupted = complete_inline(reader.read_with(receive));
    REQUIRE(interrupted.error == ECANCELED);
    REQUIRE_FALSE(reader.decoder().has_error());
    REQUIRE(complete_inline(reader.read_with(receive)).event == response_event::headers_complete);
    REQUIRE(complete_inline(reader.read_with(receive)).body == "ok");
    REQUIRE(complete_inline(reader.read_with(receive)).event == response_event::message_complete);
    REQUIRE(reader.remaining() == "NEXT");
    REQUIRE(step == 3);

    reader.reset();
    REQUIRE_FALSE(reader.reached_eof());
    REQUIRE(reader.message_bytes() == 0);
    REQUIRE(reader.bytes_remaining() == 0);
    REQUIRE_FALSE(reader.next_response());
    response_test_stream second{"HTTP/1.1 204 No Content\r\n\r\n"};
    REQUIRE(complete_inline(reader.read(second)).event == response_event::headers_complete);
    REQUIRE(complete_inline(reader.read(second)).event == response_event::message_complete);
}
