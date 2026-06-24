#include <catch2/catch_test_macros.hpp>
#include <elio/http/http2_client.hpp>
#include <elio/http/http2_session.hpp>

#include <string>
#include <string_view>
#include <type_traits>

using namespace elio::http;

// h2_session is intentionally non-movable because nghttp2 callbacks
// capture `this` and coroutines may hold references to internal maps.
static_assert(!std::is_move_constructible_v<h2_session>);
static_assert(!std::is_move_assignable_v<h2_session>);
static_assert(std::is_move_constructible_v<h2_client>);
static_assert(std::is_move_assignable_v<h2_client>);

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

// Regression for the PR #67 / on_header_callback noexcept bug:
//
// PR #67 made elio::http::headers::set() throw std::invalid_argument when the
// name is not an RFC 7230 token or the value contains CR/LF/NUL. That is the
// right behavior for caller code (it blocks header injection / response
// smuggling) but the same setter is also invoked from h2_session::
// on_header_callback, which is registered with nghttp2 (a C library) and runs
// inside nghttp2_session_mem_recv(). Throwing across a C frame is undefined
// behavior — nghttp2 does not unwind, internal HPACK / stream state would be
// left corrupt.
//
// The fix marks on_header_callback noexcept and wraps the headers::set() call
// in try/catch returning NGHTTP2_ERR_CALLBACK_FAILURE. These tests pin the
// precondition that triggers the bug (set() actually throws on the inputs we
// care about) so a future relaxation of the validation can't silently restore
// the regression. The behavioral guarantee — "callback never throws" — is
// enforced by the noexcept on the callback itself: any escaped exception
// would call std::terminate, immediately failing any test that exercises the
// session path with malformed headers.
TEST_CASE("headers::set rejects bytes that previously crashed nghttp2 callback",
          "[http2][security]") {
    headers h;

    SECTION("CR in value throws") {
        REQUIRE_THROWS_AS(h.set("X-Foo", "bad\rvalue"), std::invalid_argument);
    }
    SECTION("LF in value throws") {
        REQUIRE_THROWS_AS(h.set("X-Foo", "bad\nvalue"), std::invalid_argument);
    }
    SECTION("NUL in value throws") {
        std::string_view bad("bad\0value", 9);
        REQUIRE_THROWS_AS(h.set("X-Foo", bad), std::invalid_argument);
    }
    SECTION("Non-token byte in name throws") {
        REQUIRE_THROWS_AS(h.set("X Foo", "value"), std::invalid_argument);
    }
}
