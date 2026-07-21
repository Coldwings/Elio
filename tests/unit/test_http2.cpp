#include <catch2/catch_test_macros.hpp>
#include <elio/http/http2_client.hpp>
#include <elio/http/http2_session.hpp>
#include <elio/net/tcp.hpp>
#include <elio/runtime/scheduler.hpp>
#include <elio/time/timer.hpp>
#if defined(ELIO_HAS_TLS) && ELIO_HAS_TLS
#include <elio/tls/tls_context.hpp>
#include <elio/tls/tls_stream.hpp>
#include <openssl/evp.h>
#include <openssl/rsa.h>
#include <openssl/x509.h>
#endif
#include "../test_main.cpp"

#include <atomic>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>

using namespace elio::http;

namespace {

#if defined(ELIO_HAS_TLS) && ELIO_HAS_TLS
bool install_test_certificate(elio::tls::tls_context& ctx) {
    using key_ctx_ptr = std::unique_ptr<EVP_PKEY_CTX, decltype(&EVP_PKEY_CTX_free)>;
    using key_ptr = std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)>;
    using cert_ptr = std::unique_ptr<X509, decltype(&X509_free)>;

    key_ctx_ptr key_ctx(EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, nullptr),
                        EVP_PKEY_CTX_free);
    if (!key_ctx) {
        return false;
    }
    if (EVP_PKEY_keygen_init(key_ctx.get()) <= 0 ||
        EVP_PKEY_CTX_set_rsa_keygen_bits(key_ctx.get(), 2048) <= 0) {
        return false;
    }

    EVP_PKEY* raw_key = nullptr;
    if (EVP_PKEY_keygen(key_ctx.get(), &raw_key) <= 0) {
        return false;
    }
    key_ptr pkey(raw_key, EVP_PKEY_free);

    cert_ptr cert(X509_new(), X509_free);
    if (!cert) {
        return false;
    }

    if (X509_set_version(cert.get(), 2) != 1 ||
        ASN1_INTEGER_set(X509_get_serialNumber(cert.get()), 1) != 1 ||
        X509_gmtime_adj(X509_getm_notBefore(cert.get()), 0) == nullptr ||
        X509_gmtime_adj(X509_getm_notAfter(cert.get()), 3600) == nullptr ||
        X509_set_pubkey(cert.get(), pkey.get()) != 1) {
        return false;
    }

    auto* name = X509_get_subject_name(cert.get());
    if (!name ||
        X509_NAME_add_entry_by_txt(
            name, "CN", MBSTRING_ASC,
            reinterpret_cast<const unsigned char*>("localhost"),
            -1, -1, 0) != 1 ||
        X509_set_issuer_name(cert.get(), name) != 1 ||
        X509_sign(cert.get(), pkey.get(), EVP_sha256()) <= 0) {
        return false;
    }

    return SSL_CTX_use_certificate(ctx.native_handle(), cert.get()) == 1 &&
           SSL_CTX_use_PrivateKey(ctx.native_handle(), pkey.get()) == 1 &&
           SSL_CTX_check_private_key(ctx.native_handle()) == 1;
}

template<typename Pred>
bool wait_until(Pred pred, std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (pred()) {
            return true;
        }
        std::this_thread::sleep_for(elio::test::scaled_ms(10));
    }
    return pred();
}

class scoped_sigpipe_ignore {
public:
    scoped_sigpipe_ignore() {
#ifdef SIGPIPE
        old_ = std::signal(SIGPIPE, SIG_IGN);
#endif
    }

    ~scoped_sigpipe_ignore() {
#ifdef SIGPIPE
        std::signal(SIGPIPE, old_);
#endif
    }

    scoped_sigpipe_ignore(const scoped_sigpipe_ignore&) = delete;
    scoped_sigpipe_ignore& operator=(const scoped_sigpipe_ignore&) = delete;

private:
#ifdef SIGPIPE
    void (*old_)(int) = SIG_DFL;
#endif
};
#endif

} // namespace

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
        REQUIRE_FALSE(stream.response_status_seen);
        REQUIRE_FALSE(stream.current_header_status_seen);
        REQUIRE_FALSE(stream.current_header_status_required);
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
        stream.response_status_seen = true;
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

TEST_CASE("HTTP/2 response status pseudo-header validation",
          "[http2][security]") {
    status parsed = status::ok;

    SECTION("accepts canonical three-digit statuses") {
        REQUIRE(detail::parse_h2_response_status("200", parsed));
        REQUIRE(parsed == status::ok);

        REQUIRE(detail::parse_h2_response_status("599", parsed));
        REQUIRE(static_cast<uint16_t>(parsed) == 599);
    }

    SECTION("rejects malformed status values") {
        REQUIRE_FALSE(detail::parse_h2_response_status("", parsed));
        REQUIRE_FALSE(detail::parse_h2_response_status("20", parsed));
        REQUIRE_FALSE(detail::parse_h2_response_status("2000", parsed));
        REQUIRE_FALSE(detail::parse_h2_response_status("200junk", parsed));
        REQUIRE_FALSE(detail::parse_h2_response_status("2 0", parsed));
        REQUIRE_FALSE(detail::parse_h2_response_status("abc", parsed));
    }

    SECTION("rejects out-of-range status values") {
        REQUIRE_FALSE(detail::parse_h2_response_status("099", parsed));
        REQUIRE_FALSE(detail::parse_h2_response_status("600", parsed));
    }
}

TEST_CASE("HTTP/2 response status tracking handles informational headers",
          "[http2][security]") {
    SECTION("accepts informational status before final status") {
        h2_stream stream;

        detail::begin_h2_response_header_block(stream, NGHTTP2_HCAT_RESPONSE);
        REQUIRE(stream.current_header_status_required);
        REQUIRE(detail::record_h2_response_status(stream, "103"));
        REQUIRE(stream.current_header_status_seen);
        REQUIRE_FALSE(stream.response_status_seen);
        REQUIRE(detail::finish_h2_response_header_block(stream));
        REQUIRE(stream.error == h2_error::none);

        stream.response_headers.set("link", "</style.css>; rel=preload");

        detail::begin_h2_response_header_block(stream, NGHTTP2_HCAT_HEADERS);
        REQUIRE(stream.current_header_status_required);
        REQUIRE_FALSE(stream.response_headers.contains("link"));
        REQUIRE(detail::record_h2_response_status(stream, "200"));
        REQUIRE(stream.response_status_seen);
        REQUIRE(stream.response_status == status::ok);
        REQUIRE(detail::finish_h2_response_header_block(stream));
        REQUIRE(stream.error == h2_error::none);
    }

    SECTION("rejects missing final status after informational status") {
        h2_stream stream;

        detail::begin_h2_response_header_block(stream, NGHTTP2_HCAT_RESPONSE);
        REQUIRE(detail::record_h2_response_status(stream, "103"));
        REQUIRE(detail::finish_h2_response_header_block(stream));

        detail::begin_h2_response_header_block(stream, NGHTTP2_HCAT_HEADERS);
        REQUIRE(stream.current_header_status_required);
        REQUIRE_FALSE(detail::finish_h2_response_header_block(stream));
        REQUIRE(stream.error == h2_error::protocol_error);
        REQUIRE_FALSE(stream.response_status_seen);
    }

    SECTION("rejects 101 switching protocols") {
        h2_stream stream;

        detail::begin_h2_response_header_block(stream, NGHTTP2_HCAT_RESPONSE);
        REQUIRE_FALSE(detail::record_h2_response_status(stream, "101"));
        REQUIRE(stream.error == h2_error::protocol_error);
        REQUIRE_FALSE(stream.response_status_seen);
    }

    SECTION("does not require status in trailers after final status") {
        h2_stream stream;

        detail::begin_h2_response_header_block(stream, NGHTTP2_HCAT_RESPONSE);
        REQUIRE(detail::record_h2_response_status(stream, "200"));
        REQUIRE(detail::finish_h2_response_header_block(stream));

        detail::begin_h2_response_header_block(stream, NGHTTP2_HCAT_HEADERS);
        REQUIRE_FALSE(stream.current_header_status_required);
        REQUIRE(detail::finish_h2_response_header_block(stream));
        REQUIRE(stream.error == h2_error::none);
    }

    SECTION("rejects status in trailers after final status") {
        h2_stream stream;

        detail::begin_h2_response_header_block(stream, NGHTTP2_HCAT_RESPONSE);
        REQUIRE(detail::record_h2_response_status(stream, "200"));
        REQUIRE(detail::finish_h2_response_header_block(stream));

        detail::begin_h2_response_header_block(stream, NGHTTP2_HCAT_HEADERS);
        REQUIRE_FALSE(detail::record_h2_response_status(stream, "204"));
        REQUIRE(stream.error == h2_error::protocol_error);
    }

    SECTION("rejects duplicate status in one header block") {
        h2_stream stream;

        detail::begin_h2_response_header_block(stream, NGHTTP2_HCAT_RESPONSE);
        REQUIRE(detail::record_h2_response_status(stream, "200"));
        REQUIRE_FALSE(detail::record_h2_response_status(stream, "204"));
        REQUIRE(stream.error == h2_error::protocol_error);
    }
}

TEST_CASE("HTTP/2 stream response body enforces max size",
          "[http2][security]") {
    h2_stream stream;

    REQUIRE(stream.append_response_data("1234", 5));
    REQUIRE(stream.response_body == "1234");
    REQUIRE_FALSE(stream.response_size_exceeded);
    REQUIRE(stream.error == h2_error::none);

    REQUIRE(stream.append_response_data("5", 5));
    REQUIRE(stream.response_body == "12345");

    REQUIRE_FALSE(stream.append_response_data("6", 5));
    REQUIRE(stream.response_body == "12345");
    REQUIRE(stream.response_size_exceeded);
    REQUIRE(stream.error == h2_error::cancel);
    REQUIRE(stream.body_complete);
    REQUIRE(stream.closed);
}

TEST_CASE("HTTP/2 response headers enforce per-stream limits",
          "[http2][security][headers]") {
    SECTION("exact count and byte boundaries accept duplicate field lines") {
        h2_stream stream;

        REQUIRE(stream.append_response_header("a", "1", 2, 4));
        REQUIRE(stream.append_response_header("a", "2", 2, 4));
        REQUIRE(stream.response_header_count == 2);
        REQUIRE(stream.response_header_bytes == 4);
        REQUIRE(stream.response_headers.get_all("a").size() == 2);
        REQUIRE_FALSE(stream.response_headers_exceeded);

        REQUIRE_FALSE(stream.append_response_header("b", "", 2, 4));
        REQUIRE(stream.response_headers_exceeded);
        REQUIRE(stream.error == h2_error::enhance_your_calm);
        REQUIRE(detail::h2_stream_errno(stream) == EMSGSIZE);
        REQUIRE_FALSE(stream.response_headers.contains("b"));
        REQUIRE(stream.response_header_count == 2);
        REQUIRE(stream.response_header_bytes == 4);
    }

    SECTION("one field over the count limit is rejected independently") {
        h2_stream stream;

        REQUIRE(stream.append_response_header("a", "1", 1, 100));
        REQUIRE_FALSE(stream.append_response_header("b", "2", 1, 100));
        REQUIRE(stream.response_header_count == 1);
        REQUIRE(stream.response_header_bytes == 2);
        REQUIRE_FALSE(stream.response_headers.contains("b"));
        REQUIRE(stream.response_headers_exceeded);
        REQUIRE(detail::h2_stream_errno(stream) == EMSGSIZE);
    }

    SECTION("one byte over the aggregate limit is rejected before copying") {
        h2_stream stream;

        REQUIRE(stream.append_response_header("abc", "de", 10, 5));
        REQUIRE(stream.response_header_bytes == 5);
        REQUIRE_FALSE(stream.append_response_header("f", "", 10, 5));
        REQUIRE(stream.response_header_bytes == 5);
        REQUIRE_FALSE(stream.response_headers.contains("f"));
        REQUIRE(stream.response_headers_exceeded);
    }

    SECTION("one oversized field is rejected without size overflow") {
        h2_stream stream;

        REQUIRE_FALSE(stream.append_response_header("abc", "def", 10, 5));
        REQUIRE(stream.response_header_count == 0);
        REQUIRE(stream.response_header_bytes == 0);
        REQUIRE(stream.response_headers.empty());
        REQUIRE(stream.response_headers_exceeded);
    }
}

TEST_CASE("HTTP/2 header-limit reset remains stream-local",
          "[http2][security][headers]") {
    nghttp2_session_callbacks* callbacks = nullptr;
    REQUIRE(nghttp2_session_callbacks_new(&callbacks) == 0);
    nghttp2_session* client = nullptr;
    REQUIRE(nghttp2_session_client_new(&client, callbacks, nullptr) == 0);
    nghttp2_session_callbacks_del(callbacks);

    using session_ptr =
        std::unique_ptr<nghttp2_session, decltype(&nghttp2_session_del)>;
    session_ptr client_owner(client, nghttp2_session_del);
    const auto discard_available_client_output = [&] {
        while (true) {
            const uint8_t* data = nullptr;
            const ssize_t len = nghttp2_session_mem_send(client, &data);
            if (len < 0) {
                return false;
            }
            if (len == 0) {
                return true;
            }
        }
    };

    nghttp2_settings_entry settings[] = {
        {NGHTTP2_SETTINGS_MAX_CONCURRENT_STREAMS, 100}};
    REQUIRE(nghttp2_submit_settings(client, NGHTTP2_FLAG_NONE, settings, 1) ==
            0);

    const auto nv = [](std::string_view name, std::string_view value) {
        return nghttp2_nv{
            reinterpret_cast<uint8_t*>(const_cast<char*>(name.data())),
            reinterpret_cast<uint8_t*>(const_cast<char*>(value.data())),
            name.size(), value.size(), NGHTTP2_NV_FLAG_NONE};
    };
    std::array<nghttp2_nv, 4> request_headers{
        nv(":method", "GET"), nv(":scheme", "https"),
        nv(":authority", "example.test"), nv(":path", "/")};
    const int32_t limited_stream = nghttp2_submit_request(
        client, nullptr, request_headers.data(), request_headers.size(),
        nullptr, nullptr);
    const int32_t sibling_stream = nghttp2_submit_request(
        client, nullptr, request_headers.data(), request_headers.size(),
        nullptr, nullptr);
    REQUIRE(limited_stream > 0);
    REQUIRE(sibling_stream > limited_stream);
    REQUIRE(discard_available_client_output());

    REQUIRE(detail::reject_excess_h2_response_headers(client,
                                                       limited_stream) ==
            NGHTTP2_ERR_TEMPORAL_CALLBACK_FAILURE);
    const uint8_t* reset_data = nullptr;
    const ssize_t reset_len = nghttp2_session_mem_send(client, &reset_data);
    REQUIRE(reset_len == 13);
    REQUIRE(reset_data[3] == NGHTTP2_RST_STREAM);
    const uint32_t reset_stream =
        (static_cast<uint32_t>(reset_data[5] & 0x7f) << 24) |
        (static_cast<uint32_t>(reset_data[6]) << 16) |
        (static_cast<uint32_t>(reset_data[7]) << 8) |
        static_cast<uint32_t>(reset_data[8]);
    const uint32_t reset_error =
        (static_cast<uint32_t>(reset_data[9]) << 24) |
        (static_cast<uint32_t>(reset_data[10]) << 16) |
        (static_cast<uint32_t>(reset_data[11]) << 8) |
        static_cast<uint32_t>(reset_data[12]);
    REQUIRE(reset_stream == static_cast<uint32_t>(limited_stream));
    REQUIRE(reset_error == NGHTTP2_ENHANCE_YOUR_CALM);

    const int32_t followup_stream = nghttp2_submit_request(
        client, nullptr, request_headers.data(), request_headers.size(),
        nullptr, nullptr);
    REQUIRE(followup_stream > sibling_stream);
    REQUIRE(nghttp2_session_want_read(client));
}

TEST_CASE("HTTP/2 response header accounting resets informational blocks only",
          "[http2][security][headers]") {
    h2_stream stream;

    detail::begin_h2_response_header_block(stream, NGHTTP2_HCAT_RESPONSE);
    REQUIRE(detail::record_h2_response_status(stream, "103"));
    REQUIRE(stream.append_response_header("link", "a", 1, 16));
    REQUIRE(detail::finish_h2_response_header_block(stream));
    REQUIRE(stream.response_header_count == 1);

    detail::begin_h2_response_header_block(stream, NGHTTP2_HCAT_HEADERS);
    REQUIRE(stream.response_headers.empty());
    REQUIRE(stream.response_header_count == 0);
    REQUIRE(stream.response_header_bytes == 0);
    REQUIRE(detail::record_h2_response_status(stream, "200"));
    REQUIRE(stream.append_response_header("etag", "b", 1, 16));
    REQUIRE(detail::finish_h2_response_header_block(stream));

    detail::begin_h2_response_header_block(stream, NGHTTP2_HCAT_HEADERS);
    REQUIRE_FALSE(stream.current_header_status_required);
    REQUIRE_FALSE(stream.append_response_header("trailer", "c", 1, 16));
    REQUIRE(stream.response_headers.contains("etag"));
    REQUIRE_FALSE(stream.response_headers.contains("trailer"));
    REQUIRE(stream.response_headers_exceeded);
    REQUIRE(stream.error == h2_error::enhance_your_calm);
}

TEST_CASE("HTTP/2 response materialization preserves peer headers",
          "[http2][message]") {
    SECTION("Peer Content-Length is not rewritten") {
        h2_stream stream;
        stream.response_status = status::ok;
        stream.response_status_seen = true;
        stream.response_body = "hello";
        stream.response_headers.set("content-type", "text/plain");
        stream.response_headers.set("content-length", "999");

        auto resp = detail::materialize_h2_response(stream);

        REQUIRE(resp.status_code() == 200);
        REQUIRE(resp.body() == "hello");
        REQUIRE(resp.header("Content-Type") == "text/plain");
        REQUIRE(resp.header("Content-Length") == "999");
    }

    SECTION("Missing peer Content-Length stays missing") {
        h2_stream stream;
        stream.response_status = status::ok;
        stream.response_status_seen = true;
        stream.response_body = "hello";
        stream.response_headers.set("content-type", "text/plain");

        auto resp = detail::materialize_h2_response(stream);

        REQUIRE(resp.body() == "hello");
        REQUIRE(resp.header("Content-Type") == "text/plain");
        REQUIRE_FALSE(resp.get_headers().contains("Content-Length"));
    }

    SECTION("Duplicate peer headers remain available individually") {
        h2_stream stream;
        stream.response_status = status::ok;
        stream.response_status_seen = true;
        stream.response_headers.add("set-cookie", "a=1");
        stream.response_headers.add("set-cookie", "b=2");

        auto resp = detail::materialize_h2_response(stream);

        auto cookies = resp.get_headers().get_all("Set-Cookie");
        REQUIRE(cookies.size() == 2);
        REQUIRE(cookies[0] == "a=1");
        REQUIRE(cookies[1] == "b=2");
        REQUIRE(resp.header("Set-Cookie") == "a=1");
    }
}

TEST_CASE("HTTP/2 client configuration", "[http2][config]") {
    SECTION("new limits preserve legacy positional aggregate initialization") {
        h2_session_config session_cfg{
            7, 32768, 4096, "legacy-session-agent", true};
        REQUIRE(session_cfg.user_agent == "legacy-session-agent");
        REQUIRE(session_cfg.enable_push);
        REQUIRE(session_cfg.max_response_headers == 100);
        REQUIRE(session_cfg.max_response_header_bytes == 64 * 1024);

        h2_client_config client_cfg{
            std::chrono::seconds{1}, std::chrono::seconds{2},
            7, 32768, 4096, "legacy-client-agent", true};
        REQUIRE(client_cfg.user_agent == "legacy-client-agent");
        REQUIRE(client_cfg.enable_push);
        REQUIRE(client_cfg.max_response_headers == 100);
        REQUIRE(client_cfg.max_response_header_bytes == 64 * 1024);
    }

    SECTION("session config defaults match documented client defaults") {
        h2_session_config cfg;
        REQUIRE(cfg.max_concurrent_streams == 100);
        REQUIRE(cfg.initial_window_size == NGHTTP2_INITIAL_WINDOW_SIZE);
        REQUIRE(cfg.max_response_size == 16 * 1024 * 1024);
        REQUIRE(cfg.max_response_headers == 100);
        REQUIRE(cfg.max_response_header_bytes == 64 * 1024);
        REQUIRE(cfg.user_agent == "elio-http2/1.0");
        REQUIRE_FALSE(cfg.enable_push);
    }

    SECTION("h2_client preserves custom public config values") {
        h2_client_config cfg;
        cfg.max_concurrent_streams = 7;
        cfg.initial_window_size = 32768;
        cfg.max_response_size = 4096;
        cfg.max_response_headers = 17;
        cfg.max_response_header_bytes = 8192;
        cfg.user_agent = "elio-test-agent/1.0";
        cfg.enable_push = true;

        h2_client client(cfg);

        REQUIRE(client.config().max_concurrent_streams == 7);
        REQUIRE(client.config().initial_window_size == 32768);
        REQUIRE(client.config().max_response_size == 4096);
        REQUIRE(client.config().max_response_headers == 17);
        REQUIRE(client.config().max_response_header_bytes == 8192);
        REQUIRE(client.config().user_agent == "elio-test-agent/1.0");
        REQUIRE(client.config().enable_push);
    }

    SECTION("client response-header limits propagate to the session") {
        h2_client_config client_cfg;
        client_cfg.max_response_headers = 9;
        client_cfg.max_response_header_bytes = 1234;

        auto session_cfg = detail::make_h2_session_config(client_cfg);

        REQUIRE(session_cfg.max_response_headers == 9);
        REQUIRE(session_cfg.max_response_header_bytes == 1234);
    }
}

TEST_CASE("HTTP/2 custom send rejects non-HTTPS targets",
          "[http2][client][security]") {
    using elio::coro::task;
    using elio::runtime::scheduler;

    scheduler sched(1);
    sched.start();

    std::atomic<bool> client_done{false};
    bool request_failed = false;
    int client_errno = 0;

    sched.go([&]() -> task<void> {
        h2_client client;
        url target;
        target.scheme = "http";
        target.host = "127.0.0.1";
        target.path = "/";

        errno = 0;
        auto resp = co_await client.send(method::GET, target);
        request_failed = !resp;
        client_errno = errno;
        client_done = true;
    });

    for (int i = 0; i < 500 && !client_done; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    sched.shutdown();

    REQUIRE(client_done);
    REQUIRE(request_failed);
    REQUIRE(client_errno == EPROTONOSUPPORT);
}

TEST_CASE("HTTP/2 custom send rejects invalid request targets",
          "[http2][client][security]") {
    using elio::coro::task;
    using elio::runtime::scheduler;

    scheduler sched(1);
    sched.start();

    std::atomic<bool> client_done{false};
    bool request_failed = false;
    int client_errno = 0;

    sched.go([&]() -> task<void> {
        h2_client client;
        url target;
        target.scheme = "https";
        target.host = "127.0.0.1";
        target.path = "/bad path";

        errno = 0;
        auto resp = co_await client.send(method::GET, target);
        request_failed = !resp;
        client_errno = errno;
        client_done = true;
    });

    for (int i = 0; i < 500 && !client_done; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    sched.shutdown();

    REQUIRE(client_done);
    REQUIRE(request_failed);
    REQUIRE(client_errno == EINVAL);
}

TEST_CASE("HTTP/2 custom send rejects CONNECT before connecting",
          "[http2][client]") {
    using elio::coro::task;
    using elio::runtime::scheduler;

    scheduler sched(1);
    sched.start();

    std::atomic<bool> client_done{false};
    bool request_failed = false;
    int client_errno = 0;

    sched.go([&]() -> task<void> {
        h2_client client;
        url target;
        target.scheme = "https";
        target.host = "127.0.0.1";
        target.path = "/";

        errno = 0;
        auto resp = co_await client.send(method::CONNECT, target);
        request_failed = !resp;
        client_errno = errno;
        client_done = true;
    });

    for (int i = 0; i < 500 && !client_done; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    sched.shutdown();

    REQUIRE(client_done);
    REQUIRE(request_failed);
    REQUIRE(client_errno == EOPNOTSUPP);
}

TEST_CASE("HTTP/2 session submit validates outbound request fields",
          "[http2][session][security]") {
    url target;
    target.scheme = "https";
    target.host = "example.com";
    target.path = "/";

    REQUIRE(detail::validate_h2_submit_request_fields(
                method::GET, target, "elio-test-agent/1.0", "") == 0);
    REQUIRE(detail::validate_h2_submit_request_fields(
                method::POST, target, "elio-test-agent/1.0",
                "application/json") == 0);

    SECTION("CONNECT is unsupported") {
        REQUIRE(detail::validate_h2_submit_request_fields(
                    method::CONNECT, target, "elio-test-agent/1.0", "") ==
                EOPNOTSUPP);
    }

    SECTION("non-HTTPS target") {
        auto bad = target;
        bad.scheme = "http";
        REQUIRE(detail::validate_h2_submit_request_fields(
                    method::GET, bad, "elio-test-agent/1.0", "") ==
                EPROTONOSUPPORT);
    }

    SECTION("invalid authority") {
        auto bad = target;
        bad.host = "bad host";
        REQUIRE(detail::validate_h2_submit_request_fields(
                    method::GET, bad, "elio-test-agent/1.0", "") == EINVAL);
    }

    SECTION("invalid path") {
        auto bad = target;
        bad.path = "/bad path";
        REQUIRE(detail::validate_h2_submit_request_fields(
                    method::GET, bad, "elio-test-agent/1.0", "") == EINVAL);
    }

    SECTION("invalid user-agent") {
        REQUIRE(detail::validate_h2_submit_request_fields(
                    method::GET, target, "bad\r\nUser-Agent: injected", "") ==
                EINVAL);
    }

    SECTION("invalid content-type") {
        REQUIRE(detail::validate_h2_submit_request_fields(
                    method::POST, target, "elio-test-agent/1.0",
                    "text/plain\r\nX-Injected: yes") == EINVAL);
    }
}

TEST_CASE("nghttp2 library version", "[http2]") {
    // Basic check that nghttp2 is linked and functional
    nghttp2_info* info = nghttp2_version(0);
    REQUIRE(info != nullptr);
    REQUIRE(info->version_num > 0);

    INFO("nghttp2 version: " << info->version_str);
}

#if defined(ELIO_HAS_TLS) && ELIO_HAS_TLS
TEST_CASE("HTTP/2 read_timeout aborts stalled session initialization",
          "[http2][timeout]") {
    using elio::coro::task;
    using elio::net::ipv4_address;
    using elio::net::tcp_listener;
    using elio::runtime::scheduler;

    auto listener = tcp_listener::bind(ipv4_address("127.0.0.1", 0));
    REQUIRE(listener.has_value());
    const uint16_t port = listener->local_address().port();

    scoped_sigpipe_ignore ignore_sigpipe;

    elio::tls::tls_context server_ctx(elio::tls::tls_mode::server);
    REQUIRE(install_test_certificate(server_ctx));
    REQUIRE(server_ctx.set_alpn_protocols("h2"));

    scheduler sched(2);
    sched.start();

    std::atomic<bool> server_handshake{false};
    std::atomic<bool> server_done{false};
    std::atomic<bool> client_done{false};
    std::atomic<bool> release_server{false};
    std::atomic<int> client_errno{0};
    std::atomic<int64_t> client_elapsed_ms{-1};

    sched.go([&]() -> task<void> {
        auto stream = co_await listener->accept();
        REQUIRE(stream.has_value());

        elio::tls::tls_stream tls(std::move(*stream), server_ctx);
        bool ok = co_await tls.handshake();
        server_handshake.store(ok, std::memory_order_release);

        while (ok && !release_server.load(std::memory_order_acquire)) {
            co_await elio::time::sleep_for(elio::test::scaled_ms(10));
        }

        tls.shutdown_socket();
        server_done.store(true, std::memory_order_release);
        co_return;
    });

    sched.go([&]() -> task<void> {
        h2_client_config cfg;
        cfg.read_timeout = std::chrono::seconds(1);
        h2_client client(cfg);
        client.tls_context().set_verify_mode(elio::tls::verify_mode::none);

        const auto start = std::chrono::steady_clock::now();
        auto resp = co_await client.get(
            std::string("https://127.0.0.1:") + std::to_string(port) + "/");
        client_elapsed_ms.store(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - start).count(),
            std::memory_order_release);

        if (!resp) {
            client_errno.store(errno, std::memory_order_release);
        }
        client_done.store(true, std::memory_order_release);
        release_server.store(true, std::memory_order_release);
        co_return;
    });

    REQUIRE(wait_until([&] {
        return client_done.load(std::memory_order_acquire);
    }, elio::test::scaled_sec(5)));

    release_server.store(true, std::memory_order_release);
    REQUIRE(wait_until([&] {
        return server_done.load(std::memory_order_acquire);
    }, elio::test::scaled_sec(5)));

    REQUIRE(sched.shutdown(elio::test::scaled_sec(5)));

    REQUIRE(server_handshake.load(std::memory_order_acquire));
    REQUIRE(client_errno.load(std::memory_order_acquire) == ETIMEDOUT);
    REQUIRE(client_elapsed_ms.load(std::memory_order_acquire) >= 0);
    REQUIRE(client_elapsed_ms.load(std::memory_order_acquire) <
            elio::test::scaled_sec(5).count() * 1000);
}

TEST_CASE("HTTP/2 connect_timeout aborts stalled TLS handshake",
          "[http2][timeout][tls]") {
    using elio::coro::task;
    using elio::net::ipv4_address;
    using elio::net::tcp_listener;
    using elio::runtime::scheduler;

    auto listener = tcp_listener::bind(ipv4_address("127.0.0.1", 0));
    REQUIRE(listener.has_value());
    const uint16_t port = listener->local_address().port();

    scheduler sched(2);
    sched.start();

    std::atomic<bool> server_done{false};
    std::atomic<bool> client_done{false};
    std::atomic<int> client_errno{0};
    std::atomic<int64_t> client_elapsed_ms{-1};

    sched.go([&]() -> task<void> {
        auto stream = co_await listener->accept();
        REQUIRE(stream.has_value());

        co_await elio::time::sleep_for(elio::test::scaled_sec(3));
        stream->shutdown_socket();
        server_done.store(true, std::memory_order_release);
        co_return;
    });

    sched.go([&]() -> task<void> {
        h2_client_config cfg;
        cfg.connect_timeout = std::chrono::seconds(1);
        cfg.read_timeout = std::chrono::seconds(10);
        h2_client client(cfg);
        client.tls_context().set_verify_mode(elio::tls::verify_mode::none);

        const auto start = std::chrono::steady_clock::now();
        auto resp = co_await client.get(
            std::string("https://127.0.0.1:") + std::to_string(port) + "/");
        client_elapsed_ms.store(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - start).count(),
            std::memory_order_release);

        if (!resp) {
            client_errno.store(errno, std::memory_order_release);
        }
        client_done.store(true, std::memory_order_release);
        co_return;
    });

    REQUIRE(wait_until([&] {
        return client_done.load(std::memory_order_acquire);
    }, elio::test::scaled_sec(5)));

    REQUIRE(wait_until([&] {
        return server_done.load(std::memory_order_acquire);
    }, elio::test::scaled_sec(5)));

    REQUIRE(sched.shutdown(elio::test::scaled_sec(5)));

    REQUIRE(client_errno.load(std::memory_order_acquire) == ETIMEDOUT);
    REQUIRE(client_elapsed_ms.load(std::memory_order_acquire) >= 0);
    REQUIRE(client_elapsed_ms.load(std::memory_order_acquire) <
            elio::test::scaled_sec(3).count() * 1000);
}
#endif

// Regression for the PR #67 / on_header_callback noexcept bug:
//
// PR #67 made elio::http::headers reject invalid names/values with
// std::invalid_argument when the name is not an RFC 7230 token or the value
// contains CR/LF/NUL. That is the right behavior for caller code (it blocks
// header injection / response
// smuggling) but the same mutation path is also invoked from h2_session::
// on_header_callback, which is registered with nghttp2 (a C library) and runs
// inside nghttp2_session_mem_recv(). Throwing across a C frame is undefined
// behavior — nghttp2 does not unwind, internal HPACK / stream state would be
// left corrupt.
//
// The fix marks on_header_callback noexcept and wraps the headers mutation call
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
