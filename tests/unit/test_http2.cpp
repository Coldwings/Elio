#include <catch2/catch_test_macros.hpp>
#include <elio/http/http2_session.hpp>

using namespace elio::http;

TEST_CASE("HTTP/2 error codes", "[http2]") {
    SECTION("Error code values match nghttp2") {
        REQUIRE(static_cast<int>(h2_error::none) == 0);
        REQUIRE(static_cast<int>(h2_error::protocol_error) == NGHTTP2_PROTOCOL_ERROR);
        REQUIRE(static_cast<int>(h2_error::internal_error) == NGHTTP2_INTERNAL_ERROR);
        REQUIRE(static_cast<int>(h2_error::flow_control_error) == NGHTTP2_FLOW_CONTROL_ERROR);
        REQUIRE(static_cast<int>(h2_error::stream_closed) == NGHTTP2_STREAM_CLOSED);
        REQUIRE(static_cast<int>(h2_error::frame_size_error) == NGHTTP2_FRAME_SIZE_ERROR);
        REQUIRE(static_cast<int>(h2_error::refused_stream) == NGHTTP2_REFUSED_STREAM);
        REQUIRE(static_cast<int>(h2_error::cancel) == NGHTTP2_CANCEL);
        REQUIRE(static_cast<int>(h2_error::compression_error) == NGHTTP2_COMPRESSION_ERROR);
    }
}

TEST_CASE("HTTP/2 stream state", "[http2]") {
    h2_stream stream;

    SECTION("Default state") {
        REQUIRE(stream.stream_id == -1);
        REQUIRE_FALSE(stream.headers_complete);
        REQUIRE_FALSE(stream.body_complete);
        REQUIRE_FALSE(stream.closed);
        REQUIRE(stream.error == h2_error::none);
        REQUIRE_FALSE(stream.is_complete());
    }

    SECTION("Headers complete") {
        stream.headers_complete = true;
        REQUIRE_FALSE(stream.is_complete());
    }

    SECTION("Body complete") {
        stream.body_complete = true;
        REQUIRE_FALSE(stream.is_complete());
    }

    SECTION("Fully complete") {
        stream.headers_complete = true;
        stream.body_complete = true;
        REQUIRE(stream.is_complete());
    }

    SECTION("Stream with data") {
        stream.stream_id = 1;
        stream.response_status = status::ok;
        stream.response_body = "Hello, World!";
        stream.response_headers.set("Content-Type", "text/plain");
        stream.headers_complete = true;
        stream.body_complete = true;

        REQUIRE(stream.stream_id == 1);
        REQUIRE(stream.response_status == status::ok);
        REQUIRE(stream.response_body == "Hello, World!");
        REQUIRE(stream.response_headers.get("Content-Type") == "text/plain");
        REQUIRE(stream.is_complete());
    }
}

TEST_CASE("nghttp2 library version", "[http2]") {
    // Basic check that nghttp2 is linked and functional
    nghttp2_info* info = nghttp2_version(0);
    REQUIRE(info != nullptr);
    REQUIRE(info->version_num > 0);

    INFO("nghttp2 version: " << info->version_str);
}
