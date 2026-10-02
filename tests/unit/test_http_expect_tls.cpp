#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <elio/http/http_client.hpp>
#include <elio/sync/event.hpp>
#include <openssl/pem.h>
#include <openssl/rsa.h>
#include <openssl/x509v3.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdio>
#include <memory>
#include <stdexcept>
#include <string>
#include <unistd.h>

namespace {
using namespace elio;
using backend = io::io_context::backend_type;

struct certificate_file {
    std::array<char, 32> path{};
    FILE* file = nullptr;
    certificate_file() {
        constexpr std::string_view pattern = "/tmp/elio-expect-ca-XXXXXX";
        std::copy(pattern.begin(), pattern.end(), path.begin());
        const int fd = ::mkstemp(path.data());
        if (fd < 0) throw std::runtime_error("Expect CA fixture creation failed");
        file = ::fdopen(fd, "w");
        if (!file) {
            ::close(fd);
            ::unlink(path.data());
            throw std::runtime_error("Expect CA fixture opening failed");
        }
    }
    ~certificate_file() {
        if (file) ::fclose(file);
        ::unlink(path.data());
    }
};

void install_certificate(tls::tls_context& context, certificate_file& ca) {
    std::unique_ptr<EVP_PKEY_CTX, decltype(&EVP_PKEY_CTX_free)> generator(
        EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, nullptr), EVP_PKEY_CTX_free);
    if (!generator || EVP_PKEY_keygen_init(generator.get()) <= 0 ||
        EVP_PKEY_CTX_set_rsa_keygen_bits(generator.get(), 2048) <= 0)
        throw std::runtime_error("Expect certificate key generation failed");
    EVP_PKEY* raw = nullptr;
    if (EVP_PKEY_keygen(generator.get(), &raw) <= 0)
        throw std::runtime_error("Expect certificate key generation failed");
    std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)> key(raw, EVP_PKEY_free);
    std::unique_ptr<X509, decltype(&X509_free)> certificate(X509_new(), X509_free);
    if (!certificate || X509_set_version(certificate.get(), 2) != 1 ||
        ASN1_INTEGER_set(X509_get_serialNumber(certificate.get()), 1) != 1 ||
        !X509_gmtime_adj(X509_getm_notBefore(certificate.get()), -60) ||
        !X509_gmtime_adj(X509_getm_notAfter(certificate.get()), 3600) ||
        X509_set_pubkey(certificate.get(), key.get()) != 1)
        throw std::runtime_error("Expect certificate construction failed");
    auto* name = X509_get_subject_name(certificate.get());
    if (!name || X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC,
        reinterpret_cast<const unsigned char*>("localhost"), -1, -1, 0) != 1 ||
        X509_set_issuer_name(certificate.get(), name) != 1)
        throw std::runtime_error("Expect certificate subject failed");
    std::unique_ptr<X509_EXTENSION, decltype(&X509_EXTENSION_free)> san(
        X509V3_EXT_conf_nid(nullptr, nullptr, NID_subject_alt_name,
                          const_cast<char*>("IP:127.0.0.1")), X509_EXTENSION_free);
    if (!san || X509_add_ext(certificate.get(), san.get(), -1) != 1 ||
        X509_sign(certificate.get(), key.get(), EVP_sha256()) <= 0 ||
        SSL_CTX_use_certificate(context.native_handle(), certificate.get()) != 1 ||
        SSL_CTX_use_PrivateKey(context.native_handle(), key.get()) != 1 ||
        SSL_CTX_check_private_key(context.native_handle()) != 1 ||
        PEM_write_X509(ca.file, certificate.get()) != 1 || ::fflush(ca.file) != 0)
        throw std::runtime_error("Expect certificate publication failed");
}

struct expect_observation {
    sync::event headers_received;
    bool accepted = false;
    bool handshake = false;
    bool saw_expect = false;
    bool early_body = false;
    bool received_body = false;
    bool expiry = false;
    bool read_staged = false;
    size_t timer_calls = 0;
    int server_error = 0;
};
std::atomic<expect_observation*> expect_observed{nullptr};

coro::task<coro::cancel_result> expire_after_headers(std::chrono::nanoseconds remaining,
        coro::cancel_token stop, http::client_stage) {
    auto& observed = *expect_observed.load();
    if (observed.timer_calls++ != 0)
        co_return co_await time::sleep_for(remaining, stop);
    auto ready = co_await observed.headers_received.wait(stop);
    if (ready != coro::cancel_result::completed) co_return ready;
    observed.read_staged = http::detail::client_response_read_staged_for_test.load();
    observed.expiry = true;
    co_return coro::cancel_result::completed;
}

struct expect_hooks {
    backend previous_backend;
    http::detail::response_watchdog_wait_hook previous_wait;
    bool previous_observer;
    bool previous_staged;
    expect_observation* previous_observation;
    expect_hooks(backend selected, expect_observation& observed)
        : previous_backend(runtime::detail::worker_io_backend_for_test.exchange(selected))
        , previous_wait(http::detail::response_watchdog_wait_for_test.exchange(expire_after_headers))
        , previous_observer(http::detail::observe_client_response_read_entry_for_test.exchange(true))
        , previous_staged(http::detail::client_response_read_staged_for_test.exchange(false))
        , previous_observation(expect_observed.exchange(&observed)) {}
    ~expect_hooks() {
        expect_observed.store(previous_observation);
        http::detail::client_response_read_staged_for_test.store(previous_staged);
        http::detail::observe_client_response_read_entry_for_test.store(previous_observer);
        http::detail::response_watchdog_wait_for_test.store(previous_wait);
        runtime::detail::worker_io_backend_for_test.store(previous_backend);
    }
};

coro::task<void> serve_no_continue(net::tcp_listener& listener, tls::tls_context& context,
        bool encrypted, expect_observation& observed, coro::cancel_token token) {
    auto tcp = co_await listener.accept(token);
    if (!tcp) { observed.server_error = errno; co_return; }
    observed.accepted = true;
    std::optional<net::stream> peer;
    if (encrypted) {
        tls::tls_stream secure(std::move(*tcp), context);
        if (!co_await secure.handshake(token)) { observed.server_error = errno; co_return; }
        observed.handshake = true;
        peer.emplace(std::move(secure));
    } else {
        peer.emplace(std::move(*tcp));
    }
    http::request_parser parser;
    std::array<char, 1024> bytes{};
    while (!parser.declared_content_length()) {
        const auto read = co_await peer->read(bytes.data(), bytes.size(), token);
        if (read.result <= 0) { observed.server_error = -read.result; co_return; }
        const auto [parsed, consumed] = parser.parse(
            std::string_view(bytes.data(), static_cast<size_t>(read.result)));
        (void)consumed;
        if (parsed == http::parse_result::error) throw std::runtime_error("Expect headers parse failed");
    }
    observed.saw_expect = parser.get_headers().get("Expect") == "100-continue";
    observed.early_body = !parser.body().empty();
    // No informational response: the client must keep TLS healthy and upload.
    observed.headers_received.set();
    while (!parser.is_complete()) {
        const auto read = co_await peer->read(bytes.data(), bytes.size(), token);
        if (read.result <= 0) { observed.server_error = -read.result; co_return; }
        const auto [parsed, consumed] = parser.parse(
            std::string_view(bytes.data(), static_cast<size_t>(read.result)));
        (void)consumed;
        if (parsed == http::parse_result::error) throw std::runtime_error("Expect body parse failed");
    }
    observed.received_body = parser.body() == "payload";
    const auto written = co_await peer->write_exactly(
        "HTTP/1.1 200 OK\r\nContent-Length: 2\r\nConnection: close\r\n\r\nok", token);
    if (written.result <= 0) observed.server_error = -written.result;
}
} // namespace

TEST_CASE("Expect-only expiry uploads without cancelling a healthy TLS read",
          "[http][client][expect-continue][issue-1275]") {
    const auto selected = GENERATE(backend::epoll, backend::io_uring);
    const auto encrypted = GENERATE(false, true);
    CAPTURE(selected, encrypted);
#if ELIO_HAS_IO_URING
    if (selected == backend::io_uring && !io::io_uring_backend::is_available())
        SKIP("io_uring unavailable on this host");
#else
    if (selected == backend::io_uring) SKIP("io_uring support is not compiled");
#endif
    expect_observation observed;
    expect_hooks hooks(selected, observed);
    auto listener = net::tcp_listener::bind(net::ipv4_address("127.0.0.1", 0));
    REQUIRE(listener);
    tls::tls_context server_context(tls::tls_mode::server);
    certificate_file ca;
    if (encrypted) install_certificate(server_context, ca);
    http::transport_config config;
    config.limits = http::pool_limits{};
    if (encrypted) config.configure_tls = [&](http::transport_tls_config& policy) {
        if (!policy.load_verify_locations(ca.path.data()))
            throw std::runtime_error("Expect CA trust setup failed");
    };
    auto owner = std::make_shared<http::transport>(config);
    http::client_config policy;
    policy.read_timeout = std::chrono::seconds(30);
    policy.expect_continue_timeout = std::chrono::seconds(10);
    http::client client(owner, policy);
    const auto target = http::url::parse(std::string(encrypted ? "https" : "http") +
        "://127.0.0.1:" + std::to_string(listener->local_address().port()) + "/");
    REQUIRE(target);
    http::request request(http::method::POST, "/");
    request.set_body(std::string_view("payload"));
    request.set_expect_continue();
    coro::cancel_source stop;
    runtime::scheduler scheduler(1);
    scheduler.start();
    auto server = scheduler.go_joinable(serve_no_continue(
        *listener, server_context, encrypted, observed, stop.get_token()));
    auto requested = scheduler.go_joinable(client.send_result(request, *target));
    requested.wait_destroyed();
    auto shutdown = scheduler.go_joinable(owner->shutdown());
    shutdown.wait_destroyed();
    stop.cancel();
    server.wait_destroyed();
    const auto stopped = scheduler.shutdown(std::chrono::seconds(10));
    const auto result = requested.await_resume();
    const auto shutdown_result = shutdown.await_resume();
    server.await_resume();
    REQUIRE(stopped);
    CAPTURE(observed.server_error);
    CHECK(observed.accepted);
    if (encrypted) CHECK(observed.handshake);
    CHECK(observed.saw_expect);
    CHECK_FALSE(observed.early_body);
    CHECK(observed.expiry);
    CHECK(observed.read_staged);
    CHECK(observed.received_body);
    if (const auto* error = std::get_if<http::client_error>(&result)) {
        CAPTURE(error->stage, error->code.value());
        CHECK_FALSE(error);
    }
    CHECK(std::holds_alternative<http::response>(result));
    if (const auto* response = std::get_if<http::response>(&result)) {
        CHECK(response->status_code() == 200);
        CHECK(response->body() == "ok");
    }
    CHECK(shutdown_result == coro::cancel_result::completed);
    CHECK(owner->admission_counters_for_test().live == 0);
}
