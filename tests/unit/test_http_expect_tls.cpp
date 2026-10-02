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
#include <poll.h>
#include <stdexcept>
#include <string>
#include <system_error>
#include <thread>
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

enum class expect_action {
    fallback, response_timeout, external_cancel, timer_exception,
    write_error, write_exception, continue_response, final_rejection,
    post_upload_timeout
};

struct expect_observation {
    sync::event headers_received;
    sync::event body_received;
    sync::event upload_timer_release;
    sync::event failed_read_observed;
    sync::event release_response;
    bool blocked_upload = false;
    bool positive_before_cancel = false;
    std::atomic<int> client_fd{-1};
    std::atomic<bool> upload_timer_started{false};
    std::atomic<bool> positive_read{false};
    expect_action action = expect_action::fallback;
    coro::cancel_source* request_stop = nullptr;
    bool accepted = false;
    bool handshake = false;
    bool saw_expect = false;
    bool early_body = false;
    bool received_body = false;
    bool expiry = false;
    bool read_staged = false;
    bool extra_body = false;
    size_t timer_calls = 0;
    int server_error = 0;
};
std::atomic<expect_observation*> expect_observed{nullptr};

coro::task<coro::cancel_result> hold_upload_timer(std::chrono::nanoseconds,
        coro::cancel_token token) {
    auto& observed = *expect_observed.load();
    observed.upload_timer_started.store(true, std::memory_order_release);
    co_return co_await observed.upload_timer_release.wait(token);
}

void limit_upload_buffer(int fd) {
    const int size = 4096;
    if (::setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &size, sizeof(size)) != 0)
        throw std::system_error(errno, std::generic_category(), "upload send buffer");
    expect_observed.load()->client_fd.store(fd, std::memory_order_release);
}

coro::task<void> wait_for_failed_sibling_read(const std::optional<http::client_error>& result) {
    if (result && result->code.value() == ETIMEDOUT)
        co_await expect_observed.load()->failed_read_observed.wait();
}

void observe_response_transport(io::io_result result) {
    auto& observed = *expect_observed.load();
    if (result.result <= 0) observed.failed_read_observed.set();
    else observed.positive_read.store(true, std::memory_order_release);
}

io::io_result fail_deferred_upload(std::string_view data) {
    if (data != "payload") throw std::logic_error("unexpected write after Expect headers");
    if (expect_observed.load()->action == expect_action::write_exception)
        throw std::runtime_error("Expect upload failure");
    return {-ENOSPC, 0};
}

coro::task<coro::cancel_result> expire_after_headers(std::chrono::nanoseconds remaining,
        coro::cancel_token stop, http::client_stage) {
    auto& observed = *expect_observed.load();
    const auto call = observed.timer_calls++;
    if (observed.action == expect_action::continue_response ||
        observed.action == expect_action::final_rejection)
        co_return co_await time::sleep_for(remaining, stop);
    if (call != 0) {
        if (observed.action == expect_action::post_upload_timeout)
            co_return co_await observed.body_received.wait(stop);
        co_return co_await time::sleep_for(remaining, stop);
    }
    auto ready = co_await observed.headers_received.wait(stop);
    if (ready != coro::cancel_result::completed) co_return ready;
    observed.read_staged = http::detail::client_response_read_staged_for_test.load();
    if (observed.blocked_upload)
        http::detail::fd_watchdog_wait_for_test.store(hold_upload_timer);
    if (observed.action == expect_action::external_cancel) {
        observed.request_stop->cancel();
        co_return coro::cancel_result::cancelled;
    }
    if (observed.action == expect_action::timer_exception)
        throw std::runtime_error("Expect watchdog failure");
    if (observed.action == expect_action::write_error ||
        observed.action == expect_action::write_exception)
        http::detail::request_write_result_for_test.store(fail_deferred_upload);
    observed.expiry = true;
    co_return coro::cancel_result::completed;
}

struct expect_hooks {
    backend previous_backend;
    http::detail::response_watchdog_wait_hook previous_wait;
    http::detail::request_write_hook previous_write;
    http::detail::fd_watchdog_wait_hook previous_fd_wait;
    void(*previous_upload)(int);
    http::detail::deferred_upload_result_hook previous_upload_result;
    void(*previous_response_result)(io::io_result);
    bool previous_observer;
    bool previous_staged;
    expect_observation* previous_observation;
    expect_hooks(backend selected, expect_observation& observed)
        : previous_backend(runtime::detail::worker_io_backend_for_test.exchange(selected))
        , previous_wait(http::detail::response_watchdog_wait_for_test.exchange(expire_after_headers))
        , previous_write(http::detail::request_write_result_for_test.exchange(nullptr))
        , previous_fd_wait(http::detail::fd_watchdog_wait_for_test.exchange(nullptr))
        , previous_upload(http::detail::deferred_upload_for_test.exchange(nullptr))
        , previous_upload_result(http::detail::deferred_upload_result_for_test.exchange(nullptr))
        , previous_response_result(http::detail::response_transport_result_for_test.exchange(nullptr))
        , previous_observer(http::detail::observe_client_response_read_entry_for_test.exchange(true))
        , previous_staged(http::detail::client_response_read_staged_for_test.exchange(false))
        , previous_observation(expect_observed.exchange(&observed)) {}
    ~expect_hooks() {
        expect_observed.store(previous_observation);
        http::detail::client_response_read_staged_for_test.store(previous_staged);
        http::detail::observe_client_response_read_entry_for_test.store(previous_observer);
        http::detail::response_watchdog_wait_for_test.store(previous_wait);
        http::detail::request_write_result_for_test.store(previous_write);
        http::detail::fd_watchdog_wait_for_test.store(previous_fd_wait);
        http::detail::deferred_upload_for_test.store(previous_upload);
        http::detail::deferred_upload_result_for_test.store(previous_upload_result);
        http::detail::response_transport_result_for_test.store(previous_response_result);
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
    if (observed.blocked_upload) {
        const int size = 4096;
        if (::setsockopt(peer->fd(), SOL_SOCKET, SO_RCVBUF, &size, sizeof(size)) != 0)
            throw std::system_error(errno, std::generic_category(), "upload receive buffer");
    }
    observed.headers_received.set();
    if (observed.blocked_upload) {
        if (observed.positive_before_cancel &&
            co_await observed.release_response.wait(token) == coro::cancel_result::completed)
            (void)co_await peer->write_exactly("HTTP/1.1 103 Early Hints\r\n\r\n", token);
        sync::event stopped;
        (void)co_await stopped.wait(token);
        co_return;
    }
    if (observed.action == expect_action::final_rejection) {
        (void)co_await peer->write_exactly(
            "HTTP/1.1 417 Expectation Failed\r\nContent-Length: 2\r\nConnection: close\r\n\r\nno", token);
        const auto read = co_await peer->read(bytes.data(), bytes.size(), token);
        observed.extra_body = read.result > 0;
        co_return;
    }
    if (observed.action == expect_action::continue_response) {
        (void)co_await peer->write_exactly("HTTP/1.1 100 Continue\r\n\r\n", token);
    }
    while (!parser.is_complete()) {
        const auto read = co_await peer->read(bytes.data(), bytes.size(), token);
        if (read.result <= 0) { observed.server_error = -read.result; co_return; }
        const auto [parsed, consumed] = parser.parse(
            std::string_view(bytes.data(), static_cast<size_t>(read.result)));
        if (parser.is_complete() && consumed < static_cast<size_t>(read.result))
            observed.extra_body = true;
        if (parsed == http::parse_result::error) throw std::runtime_error("Expect body parse failed");
    }
    observed.received_body = parser.body() == "payload";
    observed.body_received.set();
    if (observed.action == expect_action::post_upload_timeout) {
        (void)co_await observed.headers_received.wait(token);
        sync::event stopped;
        (void)co_await stopped.wait(token);
        co_return;
    }
    const auto written = co_await peer->write_exactly(
        "HTTP/1.1 200 OK\r\nContent-Length: 2\r\nConnection: close\r\n\r\nok", token);
    if (written.result <= 0) observed.server_error = -written.result;
    const auto extra = co_await peer->read(bytes.data(), bytes.size(), token);
    if (extra.result > 0) observed.extra_body = true;
}
} // namespace

TEST_CASE("Expect client distinguishes upload expiry cancellation and failures",
          "[http][client][expect-continue][issue-1275]") {
    const auto selected = GENERATE(backend::epoll, backend::io_uring);
    const auto encrypted = GENERATE(false, true);
    const auto streaming = GENERATE(false, true);
    const auto action = GENERATE(expect_action::fallback, expect_action::response_timeout,
        expect_action::external_cancel, expect_action::timer_exception,
        expect_action::write_error, expect_action::write_exception,
        expect_action::continue_response, expect_action::final_rejection,
        expect_action::post_upload_timeout);
    CAPTURE(selected, encrypted, streaming, static_cast<int>(action));
#if ELIO_HAS_IO_URING
    if (selected == backend::io_uring && !io::io_uring_backend::is_available())
        SKIP("io_uring unavailable on this host");
#else
    if (selected == backend::io_uring) SKIP("io_uring support is not compiled");
#endif
    expect_observation observed;
    observed.action = action;
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
    if (action == expect_action::response_timeout)
        policy.expect_continue_timeout = std::chrono::seconds(40);
    http::client client(owner, policy);
    const auto target = http::url::parse(std::string(encrypted ? "https" : "http") +
        "://127.0.0.1:" + std::to_string(listener->local_address().port()) + "/");
    REQUIRE(target);
    http::request request(http::method::POST, "/");
    request.set_body(std::string_view("payload"));
    request.set_expect_continue();
    coro::cancel_source stop;
    coro::cancel_source request_stop;
    observed.request_stop = &request_stop;
    bool successful = false;
    bool handler_called = false;
    uint16_t response_status = 0;
    std::string response_body;
    std::optional<http::client_error> error;
    std::string exception_message;
    std::exception_ptr unexpected;
    runtime::scheduler scheduler(1);
    scheduler.start();
    auto server = scheduler.go_joinable(serve_no_continue(
        *listener, server_context, encrypted, observed, stop.get_token()));
    auto requested = scheduler.go_joinable([&]() -> coro::task<void> {
        try {
            if (streaming) {
                auto result = co_await client.with_response(request, *target, request_stop.get_token(),
                    [&](const http::response& head, http::response_body_reader& body,
                            coro::cancel_token token) -> coro::task<void> {
                        handler_called = true;
                        response_status = head.status_code();
                        std::array<char, 3> buffer{};
                        while (!body.complete()) {
                            auto read = co_await body.read_into(std::span<char>(buffer), token);
                            if (auto* failed = std::get_if<http::client_error>(&read)) {
                                error = *failed;
                                co_return;
                            }
                            response_body.append(buffer.data(),
                                std::get<http::body_read_progress>(read).transferred);
                        }
                    });
                if (auto* failed = std::get_if<http::client_error>(&result)) error = *failed;
                else successful = true;
            } else {
                auto result = co_await client.send_result(request, *target, request_stop.get_token());
                if (auto* failed = std::get_if<http::client_error>(&result)) error = *failed;
                else {
                    successful = true;
                    response_status = std::get<http::response>(result).status_code();
                    response_body = std::get<http::response>(result).body();
                }
            }
        } catch (const std::runtime_error& failure) {
            exception_message = failure.what();
        } catch (...) { unexpected = std::current_exception(); }
    });
    requested.wait_destroyed();
    auto shutdown = scheduler.go_joinable(owner->shutdown());
    shutdown.wait_destroyed();
    stop.cancel();
    server.wait_destroyed();
    const auto stopped = scheduler.shutdown(std::chrono::seconds(10));
    requested.await_resume();
    const auto shutdown_result = shutdown.await_resume();
    server.await_resume();
    if (unexpected) std::rethrow_exception(unexpected);
    REQUIRE(stopped);
    CAPTURE(observed.server_error);
    CHECK(observed.accepted);
    if (encrypted) CHECK(observed.handshake);
    CHECK(observed.saw_expect);
    CHECK_FALSE(observed.early_body);
    const bool protocol_reply = action == expect_action::continue_response ||
        action == expect_action::final_rejection;
    if (!protocol_reply) CHECK(observed.read_staged);
    CHECK_FALSE(observed.extra_body);
    const bool uploaded = action == expect_action::fallback ||
        action == expect_action::continue_response || action == expect_action::post_upload_timeout;
    CHECK(observed.received_body == uploaded);
    const bool expected_success = action == expect_action::fallback || protocol_reply;
    CHECK(successful == expected_success);
    if (expected_success) {
        CHECK_FALSE(error);
        CHECK(exception_message.empty());
        CHECK(response_status == (action == expect_action::final_rejection ? 417 : 200));
        CHECK(response_body == (action == expect_action::final_rejection ? "no" : "ok"));
        if (streaming) CHECK(handler_called);
    } else if (action == expect_action::timer_exception || action == expect_action::write_exception) {
        CHECK_FALSE(error);
        CHECK(exception_message == (action == expect_action::timer_exception
            ? "Expect watchdog failure" : "Expect upload failure"));
    } else {
        REQUIRE(error);
        CHECK(exception_message.empty());
        CHECK(error->stage == (action == expect_action::write_error
            ? http::client_stage::request : http::client_stage::headers));
        CHECK(error->code.value() == (action == expect_action::write_error ? ENOSPC
            : action == expect_action::external_cancel ? ECANCELED : ETIMEDOUT));
    }
    CHECK(shutdown_result == coro::cancel_result::completed);
    CHECK(owner->admission_counters_for_test().live == 0);
}

TEST_CASE("Blocked Expect uploads preserve timeout provenance and join cancellation",
          "[http][client][expect-continue][backpressure][issue-1275]") {
    const auto selected = GENERATE(backend::epoll, backend::io_uring);
    const auto encrypted = GENERATE(false, true);
    const auto workers = GENERATE(size_t{1}, size_t{2});
    const auto outcome = GENERATE(0, 1, 2); // Deadline, cancellation, positive read then cancellation.
    CAPTURE(selected, encrypted, workers, outcome);
#if ELIO_HAS_IO_URING
    if (selected == backend::io_uring && !io::io_uring_backend::is_available())
        SKIP("io_uring unavailable on this host");
#else
    if (selected == backend::io_uring) SKIP("io_uring support is not compiled");
#endif
    expect_observation observed;
    observed.blocked_upload = true;
    observed.positive_before_cancel = outcome == 2;
    expect_hooks hooks(selected, observed);
    http::detail::deferred_upload_for_test.store(limit_upload_buffer);
    http::detail::deferred_upload_result_for_test.store(wait_for_failed_sibling_read);
    http::detail::response_transport_result_for_test.store(observe_response_transport);
    auto listener = net::tcp_listener::bind(net::ipv4_address("127.0.0.1", 0));
    REQUIRE(listener);
    tls::tls_context server_context(tls::tls_mode::server);
    certificate_file ca;
    if (encrypted) install_certificate(server_context, ca);
    http::transport_config config;
    config.limits = http::pool_limits{};
    if (encrypted) config.configure_tls = [&](http::transport_tls_config& policy) {
        if (!policy.load_verify_locations(ca.path.data()))
            throw std::runtime_error("blocked Expect CA setup failed");
    };
    auto owner = std::make_shared<http::transport>(config);
    http::client_config policy;
    policy.read_timeout = std::chrono::seconds(30);
    policy.expect_continue_timeout = std::chrono::seconds(10);
    http::client client(owner, policy);
    auto target = http::url::parse(std::string(encrypted ? "https" : "http") +
        "://127.0.0.1:" + std::to_string(listener->local_address().port()) + "/");
    REQUIRE(target);
    http::request request(http::method::POST, "/");
    request.set_body(std::string(4 * 1024 * 1024, 'u'));
    request.set_expect_continue();
    coro::cancel_source stop;
    coro::cancel_source request_stop;
    observed.request_stop = &request_stop;
    std::optional<http::client_error> error;
    std::exception_ptr failure;
    runtime::scheduler scheduler(workers);
    scheduler.start();
    auto server = scheduler.go_joinable(serve_no_continue(
        *listener, server_context, encrypted, observed, stop.get_token()));
    auto requested = scheduler.go_joinable([&]() -> coro::task<void> {
        try {
            auto result = co_await client.send_result(request, *target, request_stop.get_token());
            if (auto* failed = std::get_if<http::client_error>(&result)) error = *failed;
        } catch (...) { failure = std::current_exception(); }
    });
    auto observe = [](auto predicate) {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        while (!predicate()) {
            if (std::chrono::steady_clock::now() >= deadline) return false;
            std::this_thread::yield();
        }
        return true;
    };
    bool parked = false;
    (void)observe([&] {
        if (requested.is_ready()) return true;
        const int fd = observed.client_fd.load(std::memory_order_acquire);
        if (fd < 0 || !observed.upload_timer_started.load(std::memory_order_acquire)) return false;
        pollfd readiness{fd, POLLOUT, 0};
        if (::poll(&readiness, 1, 0) != 0) return false;
        size_t pending = 0;
        for (size_t index = 0; index < workers; ++index)
            pending += scheduler.get_worker(index)->io_context().pending_count();
        // No server I/O or timer is admitted here: these are the actual
        // response read and lower output write on a non-writable socket.
        parked = pending >= 2;
        return parked;
    });
    bool positive = false;
    if (parked && outcome == 2) {
        observed.release_response.set();
        positive = observe([&] { return observed.positive_read.load(std::memory_order_acquire); });
    }
    if (parked && outcome == 0) observed.upload_timer_release.set();
    else request_stop.cancel();
    const bool completed = observe([&] { return requested.is_ready(); });
    if (!completed) {
        // Release every test-owned barrier before awaiting normal teardown.
        observed.failed_read_observed.set();
        observed.upload_timer_release.set();
        request_stop.cancel();
    }
    requested.wait_destroyed();
    auto shutdown = scheduler.go_joinable(owner->shutdown());
    shutdown.wait_destroyed();
    stop.cancel();
    server.wait_destroyed();
    const auto stopped = scheduler.shutdown(std::chrono::seconds(10));
    requested.await_resume();
    const auto shutdown_result = shutdown.await_resume();
    server.await_resume();
    if (failure) std::rethrow_exception(failure);
    REQUIRE(stopped);
    CHECK(parked);
    CHECK(completed);
    if (outcome == 2) CHECK(positive);
    CHECK(observed.accepted);
    if (encrypted) CHECK(observed.handshake);
    CHECK(observed.saw_expect);
    CHECK_FALSE(observed.early_body);
    CHECK_FALSE(observed.received_body);
    REQUIRE(error);
    CAPTURE(error->stage, error->code.value());
    CHECK(error->code.value() == (outcome == 0 ? ETIMEDOUT : ECANCELED));
    if (outcome == 0) CHECK(error->stage == http::client_stage::request);
    CHECK(shutdown_result == coro::cancel_result::completed);
    CHECK(owner->admission_counters_for_test().live == 0);
}
