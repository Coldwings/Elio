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

TEST_CASE("HTTP/2 client configuration", "[http2][config]") {
    SECTION("session config defaults match documented client defaults") {
        h2_session_config cfg;
        REQUIRE(cfg.max_concurrent_streams == 100);
        REQUIRE(cfg.initial_window_size == NGHTTP2_INITIAL_WINDOW_SIZE);
        REQUIRE(cfg.user_agent == "elio-http2/1.0");
        REQUIRE_FALSE(cfg.enable_push);
    }

    SECTION("h2_client preserves custom public config values") {
        h2_client_config cfg;
        cfg.max_concurrent_streams = 7;
        cfg.initial_window_size = 32768;
        cfg.user_agent = "elio-test-agent/1.0";
        cfg.enable_push = true;

        h2_client client(cfg);

        REQUIRE(client.config().max_concurrent_streams == 7);
        REQUIRE(client.config().initial_window_size == 32768);
        REQUIRE(client.config().user_agent == "elio-test-agent/1.0");
        REQUIRE(client.config().enable_push);
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
#endif

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
