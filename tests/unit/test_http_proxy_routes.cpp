#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <elio/coro/detail/completion_waiter.hpp>
#include <elio/http/http_client.hpp>
#include <elio/coro/join_wait.hpp>
#include <elio/io/io_awaitables.hpp>
#include <elio/runtime/affinity.hpp>
#include <elio/time/timer.hpp>

#include <openssl/pem.h>
#include <openssl/rsa.h>
#include <openssl/x509v3.h>

#include <array>
#include <coroutine>
#include <cstdio>
#include <exception>
#include <fcntl.h>
#include <memory>
#include <latch>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <unistd.h>
#include <vector>

using namespace elio::http;
using elio::coro::task;

namespace {

using backend = elio::io::io_context::backend_type;

void require_backend(backend selected);

struct backend_guard {
    backend previous;
    explicit backend_guard(backend selected)
        : previous(elio::runtime::detail::worker_io_backend_for_test.load()) {
        require_backend(selected);
        elio::runtime::detail::worker_io_backend_for_test.store(selected);
    }
    ~backend_guard() { elio::runtime::detail::worker_io_backend_for_test.store(previous); }
};

void require_backend(backend selected) {
#if ELIO_HAS_IO_URING
    if (selected == backend::io_uring && !elio::io::io_uring_backend::is_available())
        SKIP("io_uring unavailable on this host");
#else
    if (selected == backend::io_uring) SKIP("io_uring support is not compiled");
#endif
}

struct temporary_pem {
    std::array<char, 32> path{};
    FILE* file = nullptr;
    temporary_pem() {
        constexpr std::string_view pattern = "/tmp/elio-proxy-ca-XXXXXX";
        std::copy(pattern.begin(), pattern.end(), path.begin());
        const auto fd = ::mkstemp(path.data());
        if (fd < 0) throw std::runtime_error("proxy CA fixture creation failed");
        file = ::fdopen(fd, "w");
        if (!file) {
            ::close(fd);
            ::unlink(path.data());
            throw std::runtime_error("proxy CA fixture opening failed");
        }
    }
    ~temporary_pem() {
        if (file) ::fclose(file);
        ::unlink(path.data());
    }
};

struct empty_connection_probe {
    int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    ~empty_connection_probe() { if (fd >= 0) ::close(fd); }
    empty_connection_probe() = default;
    empty_connection_probe(const empty_connection_probe&) = delete;
    empty_connection_probe& operator=(const empty_connection_probe&) = delete;
};

task<int> peek_fixture_payload(elio::net::tcp_stream& stream,
                               elio::coro::cancel_token token) {
    char first;
    for (;;) {
        auto read = co_await elio::io::async_recv(
            stream.fd(), &first, 1, MSG_PEEK, token);
        if (read.was_cancelled()) co_return -ECANCELED;
        if (read.io.result == -EINTR) continue;
        if (read.io.result != -EAGAIN && read.io.result != -EWOULDBLOCK)
            co_return read.io.result;
        auto ready = co_await stream.poll_read(token);
        if (ready.was_cancelled()) co_return -ECANCELED;
        if (ready.io.result < 0) co_return ready.io.result;
    }
}

task<std::optional<elio::net::tcp_stream>> accept_fixture_payload(
        elio::net::tcp_listener& listener, elio::coro::cancel_token token,
        size_t* empty_connections = nullptr, int* accept_error = nullptr) {
    for (;;) {
        auto accepted = co_await listener.accept(token);
        if (!accepted) {
            if (accept_error) *accept_error = errno;
            co_return std::nullopt;
        }
        // Local probes can arrive before the fixture's client. Peek so neither
        // CONNECT/application bytes nor a later TLS ClientHello are consumed.
        const auto payload = co_await peek_fixture_payload(*accepted, token);
        if (payload == 0) {
            if (empty_connections) ++*empty_connections;
            continue;
        }
        if (payload < 0) {
            if (accept_error) *accept_error = -payload;
            co_return std::nullopt;
        }
        co_return accepted;
    }
}

void install_certificate(elio::tls::tls_context& context, temporary_pem& ca,
                         const char* identities = "DNS:localhost") {
    std::unique_ptr<EVP_PKEY_CTX, decltype(&EVP_PKEY_CTX_free)> generator(
        EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, nullptr), EVP_PKEY_CTX_free);
    if (!generator || EVP_PKEY_keygen_init(generator.get()) <= 0 ||
        EVP_PKEY_CTX_set_rsa_keygen_bits(generator.get(), 2048) <= 0)
        throw std::runtime_error("proxy certificate key generation failed");
    EVP_PKEY* raw_key = nullptr;
    if (EVP_PKEY_keygen(generator.get(), &raw_key) <= 0)
        throw std::runtime_error("proxy certificate key generation failed");
    std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)> key(raw_key, EVP_PKEY_free);
    std::unique_ptr<X509, decltype(&X509_free)> certificate(X509_new(), X509_free);
    if (!certificate || X509_set_version(certificate.get(), 2) != 1 ||
        ASN1_INTEGER_set(X509_get_serialNumber(certificate.get()), 1) != 1 ||
        !X509_gmtime_adj(X509_getm_notBefore(certificate.get()), -60) ||
        !X509_gmtime_adj(X509_getm_notAfter(certificate.get()), 3600) ||
        X509_set_pubkey(certificate.get(), key.get()) != 1)
        throw std::runtime_error("proxy certificate construction failed");
    auto* name = X509_get_subject_name(certificate.get());
    if (!name || X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC,
        reinterpret_cast<const unsigned char*>("localhost"), -1, -1, 0) != 1 ||
        X509_set_issuer_name(certificate.get(), name) != 1)
        throw std::runtime_error("proxy certificate subject failed");
    std::unique_ptr<X509_EXTENSION, decltype(&X509_EXTENSION_free)> san(
        X509V3_EXT_conf_nid(nullptr, nullptr, NID_subject_alt_name,
                          const_cast<char*>(identities)), X509_EXTENSION_free);
    if (!san || X509_add_ext(certificate.get(), san.get(), -1) != 1 ||
        X509_sign(certificate.get(), key.get(), EVP_sha256()) <= 0 ||
        SSL_CTX_use_certificate(context.native_handle(), certificate.get()) != 1 ||
        SSL_CTX_use_PrivateKey(context.native_handle(), key.get()) != 1 ||
        SSL_CTX_check_private_key(context.native_handle()) != 1 ||
        PEM_write_X509(ca.file, certificate.get()) != 1 || ::fflush(ca.file) != 0)
        throw std::runtime_error("proxy certificate publication failed");
}

struct request_read_observation {
    size_t bytes = 0;
    int terminal_error = 0;
    bool complete = false;
};

template<typename Stream>
task<std::optional<request>> receive_request(Stream& stream, elio::coro::cancel_token token,
                                           request_read_observation* observed = nullptr) {
    request_parser parser;
    std::array<char, 4096> bytes{};
    while (!parser.is_complete()) {
        const auto received = co_await stream.read(bytes.data(), bytes.size(), token);
        if (received.result <= 0) {
            if (observed) observed->terminal_error = received.result < 0 ? -received.result : 0;
            co_return std::nullopt;
        }
        if (observed) observed->bytes += static_cast<size_t>(received.result);
        const auto [parsed, consumed] = parser.parse(
            std::string_view(bytes.data(), static_cast<size_t>(received.result)));
        (void)consumed;
        if (parsed == parse_result::error) throw std::runtime_error("proxy fixture request parse failed");
    }
    if (observed) observed->complete = true;
    co_return request::from_parser(parser);
}

struct observed_route {
    size_t accepted = 0;
    size_t empty_connections = 0;
    int accept_error = 0;
    request_read_observation connect_read;
    std::vector<request> requests;
    std::string sni;
    bool handshake = false;
    int server_handshake_error = 0;
    size_t client_handshake_failures = 0;
    int client_handshake_error = 0;
    long client_verification = X509_V_OK;
    size_t client_connect_written = 0;
    size_t client_connect_received = 0;
    int client_connect_error = 0;
    bool client_connect_writing = false;
    bool client_connect_reading = false;
    std::optional<std::chrono::steady_clock::time_point> client_setup_deadline;
};

std::atomic<observed_route*> authentication_observed{nullptr};

void observe_handshake_failure(detail::connect_tls_stream& inner, int error) {
    auto& observed = *authentication_observed.load();
    ++observed.client_handshake_failures;
    observed.client_handshake_error = error;
    observed.client_verification = inner.verify_result();
}

void observe_connect_progress(detail::proxy_connect_step step, size_t bytes, int error,
        std::optional<std::chrono::steady_clock::time_point> deadline) {
    auto& observed = *authentication_observed.load();
    observed.client_setup_deadline = deadline;
    if (step == detail::proxy_connect_step::writing) observed.client_connect_writing = true;
    if (step == detail::proxy_connect_step::written) observed.client_connect_written = bytes;
    if (step == detail::proxy_connect_step::reading) observed.client_connect_reading = true;
    if (step == detail::proxy_connect_step::received) observed.client_connect_received = bytes;
    if (error) observed.client_connect_error = error;
}

struct authentication_hooks {
    explicit authentication_hooks(observed_route& observed) {
        authentication_observed.store(&observed);
        detail::tunnel_handshake_failed_for_test.store(observe_handshake_failure);
        detail::proxy_connect_progress_for_test.store(observe_connect_progress);
    }
    ~authentication_hooks() {
        detail::tunnel_handshake_failed_for_test.store(nullptr);
        detail::proxy_connect_progress_for_test.store(nullptr);
        authentication_observed.store(nullptr);
    }
};

bool certificate_rejected(const client_error& error, const observed_route& observed,
                          long expected) {
    return error.stage == client_stage::tls && error.code.value() > 0 &&
        error.code.value() != ETIMEDOUT && error.code.value() != ECANCELED &&
        observed.client_handshake_failures == 1 && observed.client_handshake_error > 0 &&
        observed.client_verification == expected;
}

task<void> serve_route(elio::net::tcp_listener& listener, bool secure,
        elio::tls::tls_context& tls_context, size_t count, observed_route& observed,
        elio::coro::cancel_token token) {
    auto accepted = co_await accept_fixture_payload(listener, token,
        &observed.empty_connections, &observed.accept_error);
    if (!accepted) co_return;
    ++observed.accepted;
    auto stream = std::move(*accepted);
    if (secure) {
        auto setup = co_await receive_request(stream, token, &observed.connect_read);
        if (!setup) co_return;
        observed.requests.push_back(std::move(*setup));
        constexpr std::string_view connected =
            "HTTP/1.1 200 Tunnel\r\nContent-Length: ignored\r\nTransfer-Encoding: ignored\r\n\r\n";
        if ((co_await stream.write_exactly(connected, token)).result <= 0) co_return;
        elio::tls::tls_stream origin(std::move(stream), tls_context);
        observed.handshake = co_await origin.handshake(token);
        if (!observed.handshake) {
            observed.server_handshake_error = errno;
            co_return;
        }
        for (size_t i = 0; i < count; ++i) {
            auto incoming = co_await receive_request(origin, token);
            if (!incoming) break;
            observed.requests.push_back(std::move(*incoming));
            if ((co_await origin.write_exactly(
                "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok", token)).result <= 0) break;
        }
        co_await origin.abort_and_settle();
    } else {
        for (size_t i = 0; i < count; ++i) {
            auto incoming = co_await receive_request(stream, token);
            if (!incoming) break;
            observed.requests.push_back(std::move(*incoming));
            if ((co_await stream.write_exactly(
                "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok", token)).result <= 0) break;
        }
    }
}

task<void> stall_tunnel_response(elio::net::tcp_listener& listener,
        elio::tls::tls_context& tls_context, observed_route& observed,
        elio::coro::cancel_token token) {
    auto accepted = co_await accept_fixture_payload(listener, token,
        &observed.empty_connections, &observed.accept_error);
    if (!accepted) co_return;
    ++observed.accepted;
    auto stream = std::move(*accepted);
    auto setup = co_await receive_request(stream, token, &observed.connect_read);
    if (!setup) co_return;
    observed.requests.push_back(std::move(*setup));
    if ((co_await stream.write_exactly(
            "HTTP/1.1 200 Tunnel\r\n\r\n", token)).result <= 0) co_return;
    elio::tls::tls_stream origin(std::move(stream), tls_context);
    observed.handshake = co_await origin.handshake(token);
    if (!observed.handshake) {
        observed.server_handshake_error = errno;
        co_return;
    }
    auto incoming = co_await receive_request(origin, token);
    if (!incoming) co_return;
    observed.requests.push_back(std::move(*incoming));
    elio::sync::event waiting;
    (void)co_await waiting.wait(token);
    co_await origin.abort_and_settle();
}

int record_sni(SSL* session, int*, void* context) noexcept {
    try {
        if (const auto* name = SSL_get_servername(session, TLSEXT_NAMETYPE_host_name))
            static_cast<observed_route*>(context)->sni = name;
        return SSL_TLSEXT_ERR_OK;
    } catch (...) { return SSL_TLSEXT_ERR_ALERT_FATAL; }
}

task<void> redirect_route(elio::net::tcp_listener& listener,
        elio::tls::tls_context& context, observed_route& observed,
        elio::coro::cancel_token token) {
    auto first = co_await listener.accept(token);
    if (!first) co_return;
    ++observed.accepted;
    auto initial = co_await receive_request(*first, token);
    if (!initial) co_return;
    observed.requests.push_back(std::move(*initial));
    if ((co_await first->write_exactly(
        "HTTP/1.1 302 Found\r\nContent-Length: 0\r\n"
        "Location: https://localhost:9443/final?done=1#client-only\r\n\r\n", token)).result <= 0)
        co_return;
    // Keep the old forward channel open; the redirected CONNECT route must
    // acquire a new, target/security-bound channel, not reinterpret this one.
    co_await serve_route(listener, true, context, 1, observed, token);
}

task<std::vector<client_result<response>>> send_requests(client& agent, const url& target,
                                                       size_t count, bool streaming = false) {
    std::vector<client_result<response>> results;
    for (size_t i = 0; i < count; ++i) {
        request req(method::GET, "/path");
        req.set_query("q=" + std::to_string(i));
        req.set_header("Proxy-Authorization", "Basic caller-secret");
        req.set_header("Authorization", "Bearer origin-secret");
        if (streaming) {
            std::optional<response> received;
            std::string payload;
            auto result = co_await agent.with_response(req, target, {},
                [&](const response& head, response_body_reader& body,
                    elio::coro::cancel_token token) -> task<void> {
                    received = head;
                    std::array<char, 1> bytes{};
                    while (!body.complete()) {
                        auto part = co_await body.read_into(std::span<char>(bytes), token);
                        if (std::holds_alternative<client_error>(part)) co_return;
                        payload.append(bytes.data(), std::get<body_read_progress>(part).transferred);
                    }
                });
            if (const auto* error = std::get_if<client_error>(&result)) results.push_back(*error);
            else {
                if (!received) throw std::runtime_error("proxy streaming handler was not invoked");
                received->set_body(std::move(payload));
                results.push_back(std::move(*received));
            }
        } else results.push_back(co_await agent.send_result(req, target));
        if (std::holds_alternative<client_error>(results.back())) break;
    }
    co_return results;
}

} // namespace

namespace {

struct setup_observation {
    std::chrono::steady_clock::time_point tcp_deadline;
    std::chrono::steady_clock::time_point route_deadline;
    elio::sync::event route_entered;
    elio::sync::event tls_entered;
    elio::sync::event connect_read;
    elio::sync::event expire;
    elio::sync::event write_entered;
    elio::sync::event accepted;
};

std::atomic<setup_observation*> setup_observed{nullptr};

task<elio::coro::cancel_result> observe_tcp_budget(std::chrono::steady_clock::time_point deadline,
                                                 elio::coro::cancel_token token) {
    auto& observed = *setup_observed.load();
    observed.tcp_deadline = deadline;
    elio::sync::event waiting;
    co_return co_await waiting.wait(token);
}

task<elio::coro::cancel_result> observe_route_budget(std::chrono::steady_clock::time_point deadline,
                                                   elio::coro::cancel_token token) {
    auto& observed = *setup_observed.load();
    observed.route_deadline = deadline;
    observed.route_entered.set();
    co_return co_await observed.expire.wait(token);
}

void observe_tls_setup() { setup_observed.load()->tls_entered.set(); }

task<void> gate_connect_write(elio::coro::cancel_token token) {
    setup_observed.load()->write_entered.set();
    elio::sync::event waiting;
    (void)co_await waiting.wait(token);
}

struct setup_hooks {
    explicit setup_hooks(setup_observation& value) {
        setup_observed.store(&value);
        detail::setup_watchdog_wait_for_test.store(observe_tcp_budget);
        detail::route_operation_wait_for_test.store(observe_route_budget);
        detail::tls_setup_entered_for_test.store(observe_tls_setup);
    }
    ~setup_hooks() {
        detail::setup_watchdog_wait_for_test.store(nullptr);
        detail::route_operation_wait_for_test.store(nullptr);
        detail::tls_setup_entered_for_test.store(nullptr);
        setup_observed.store(nullptr);
    }
};

class route_test_phase {
    class awaiter {
    public:
        explicit awaiter(route_test_phase& phase) noexcept
            : phase_(phase), waiter_(phase.slot_) {}
        bool await_ready() const noexcept {
            return phase_.released_.load(std::memory_order_acquire);
        }
        bool await_suspend(std::coroutine_handle<> handle) noexcept {
            return phase_.slot_.register_waiter(waiter_, handle,
                [this] { return await_ready(); });
        }
        void await_resume() const noexcept {}
    private:
        route_test_phase& phase_;
        elio::coro::detail::completion_waiter waiter_;
    };
public:
    auto wait() noexcept { return awaiter(*this); }
    bool is_set() const noexcept {
        return released_.load(std::memory_order_acquire);
    }
    void set() noexcept {
        released_.store(true, std::memory_order_release);
        auto wake = slot_.take();
        if (auto handle = wake.claim()) elio::runtime::schedule_handle(handle);
    }
private:
    std::atomic<bool> released_{false};
    elio::coro::detail::completion_waiter_slot slot_;
};

struct response_admission_observation {
    route_test_phase release;
    std::atomic<bool> entered{false};
    std::atomic<bool> operation_entered{false};
};

std::atomic<response_admission_observation*> response_admission_observed{nullptr};

task<void> pause_before_response_watchdog_start() {
    auto& observed = *response_admission_observed.load(std::memory_order_acquire);
    observed.entered.store(true, std::memory_order_release);
    co_await observed.release.wait();
}

void observe_response_operation_entry() {
    response_admission_observed.load(std::memory_order_acquire)->operation_entered.store(
        true, std::memory_order_release);
}

struct response_admission_guard {
    response_admission_observation* previous_observation;
    task<void> (*previous_start)();
    void (*previous_operation)();
    bool previous_read_observer;
    bool previous_read_staged;
    bool previous_admission_closed;
    explicit response_admission_guard(response_admission_observation& observed)
        : previous_observation(response_admission_observed.exchange(&observed))
        , previous_start(detail::response_watchdog_before_start_for_test.exchange(
              pause_before_response_watchdog_start))
        , previous_operation(detail::response_watchdog_operation_entered_for_test.exchange(
              observe_response_operation_entry))
        , previous_read_observer(
              detail::observe_client_response_read_entry_for_test.exchange(true))
        , previous_read_staged(
              detail::client_response_read_staged_for_test.exchange(false))
        , previous_admission_closed(
              elio::runtime::detail::graceful_admission_closed_for_test.exchange(false)) {}
    ~response_admission_guard() {
        response_admission_observed.store(previous_observation);
        detail::response_watchdog_before_start_for_test.store(previous_start);
        detail::response_watchdog_operation_entered_for_test.store(previous_operation);
        detail::observe_client_response_read_entry_for_test.store(previous_read_observer);
        detail::client_response_read_staged_for_test.store(previous_read_staged);
        elio::runtime::detail::graceful_admission_closed_for_test.store(
            previous_admission_closed);
    }
};

struct route_ready_probe {
    route_test_phase release_parent;
    std::atomic<bool> parent_waiting{false};
    std::atomic<bool> timer_failed{false};
    std::atomic<bool> operation_entered{false};
    std::atomic<bool> destruction_observed{false};
    std::atomic<bool> done{false};
    std::mutex barrier_mutex;
    bool barrier_released = false;
    std::exception_ptr failure;
};

std::atomic<route_ready_probe*> route_ready_observed{nullptr};

task<void> pause_after_route_watchdog_start() {
    auto& probe = *route_ready_observed.load(std::memory_order_acquire);
    probe.parent_waiting.store(true, std::memory_order_release);
    co_await probe.release_parent.wait();
}

task<elio::coro::cancel_result> fail_ready_route_watchdog(
        std::chrono::steady_clock::time_point, elio::coro::cancel_token) {
    co_await elio::runtime::set_affinity(1);
    auto& probe = *route_ready_observed.load(std::memory_order_acquire);
    {
        std::lock_guard lock(probe.barrier_mutex);
        if (!probe.barrier_released)
            elio::coro::detail::pause_before_detached_frame_destroy_for_test.store(true);
        probe.timer_failed.store(true, std::memory_order_release);
    }
    throw std::runtime_error("ready route watchdog failed");
    co_return elio::coro::cancel_result::completed;
}

void observe_route_operation_entry() {
    route_ready_observed.load(std::memory_order_acquire)->operation_entered.store(
        true, std::memory_order_release);
}

void observe_route_destruction_wait() {
    route_ready_observed.load(std::memory_order_acquire)->destruction_observed.store(
        true, std::memory_order_release);
}

struct route_ready_guard {
    route_ready_probe& probe;
    route_ready_probe* previous_probe;
    detail::route_operation_wait_hook previous_wait;
    task<void> (*previous_start)();
    void (*previous_operation)();
    void (*previous_observer)();
    explicit route_ready_guard(route_ready_probe& value)
        : probe(value)
        , previous_probe(route_ready_observed.exchange(&value))
        , previous_wait(detail::route_operation_wait_for_test.exchange(
              fail_ready_route_watchdog))
        , previous_start(detail::route_watchdog_after_start_for_test.exchange(
              pause_after_route_watchdog_start))
        , previous_operation(detail::route_operation_entered_for_test.exchange(
              observe_route_operation_entry))
        , previous_observer(elio::coro::detail::join_destroyed_observer_setup_for_test.exchange(
              observe_route_destruction_wait)) {
        elio::coro::detail::detached_frame_destroy_paused_for_test.store(false);
        elio::coro::detail::pause_before_detached_frame_destroy_for_test.store(false);
    }
    void release() const noexcept {
        {
            std::lock_guard lock(probe.barrier_mutex);
            // Release is terminal: a late timer may not park its worker after
            // controller recovery has started joining all owned frames.
            probe.barrier_released = true;
            elio::coro::detail::pause_before_detached_frame_destroy_for_test.store(false);
            elio::coro::detail::pause_before_detached_frame_destroy_for_test.notify_all();
        }
        probe.release_parent.set();
    }
    ~route_ready_guard() {
        release();
        elio::coro::detail::join_destroyed_observer_setup_for_test.store(previous_observer);
        detail::route_operation_entered_for_test.store(previous_operation);
        detail::route_watchdog_after_start_for_test.store(previous_start);
        detail::route_operation_wait_for_test.store(previous_wait);
        route_ready_observed.store(previous_probe);
        elio::coro::detail::detached_frame_destroy_paused_for_test.store(false);
    }
};

template<typename Predicate>
bool observe_route_ready(Predicate predicate) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (!predicate()) {
        if (std::chrono::steady_clock::now() >= deadline) return false;
        std::this_thread::yield();
    }
    return true;
}

task<void> exercise_ready_route_watchdog(route_ready_probe& probe) {
    try {
        (void)co_await detail::await_route_operation<int>(
            [](elio::coro::cancel_token) -> task<int> { co_return 7; }, {},
            std::chrono::steady_clock::now() + std::chrono::seconds(30));
    } catch (...) { probe.failure = std::current_exception(); }
    probe.done.store(true, std::memory_order_release);
}

struct connect_write_gate {
    connect_write_gate() { detail::proxy_connect_write_wait_for_test.store(gate_connect_write); }
    ~connect_write_gate() { detail::proxy_connect_write_wait_for_test.store(nullptr); }
};

task<void> unread_connect(elio::net::tcp_listener& listener, setup_observation& observed,
        observed_route& route, elio::coro::cancel_token token) {
    auto stream = co_await listener.accept(token);
    if (!stream) {
        route.accept_error = errno;
        co_return;
    }
    ++route.accepted;
    observed.accepted.set();
    (void)co_await receive_request(*stream, token, &route.connect_read);
}

task<void> stalled_connect(elio::net::tcp_listener& listener, bool inner_tls,
        setup_observation& observed, elio::coro::cancel_token token) {
    auto stream = co_await listener.accept(token);
    if (!stream) co_return;
    auto setup = co_await receive_request(*stream, token);
    if (!setup) co_return;
    observed.connect_read.set();
    if (inner_tls) {
        (void)co_await stream->write_exactly("HTTP/1.1 200 Tunnel\r\n\r\n", token);
        std::array<char, 4096> bytes{};
        while ((co_await stream->read(bytes.data(), bytes.size(), token)).result > 0) {}
    } else {
        elio::sync::event waiting;
        (void)co_await waiting.wait(token);
    }
}

} // namespace

namespace {

task<void> reject_connect(elio::net::tcp_listener& listener, observed_route& observed,
                         elio::coro::cancel_token token) {
    for (size_t i = 0; i < 2; ++i) {
        auto stream = co_await listener.accept(token);
        if (!stream) co_return;
        ++observed.accepted;
        auto incoming = co_await receive_request(*stream, token);
        if (!incoming) co_return;
        observed.requests.push_back(std::move(*incoming));
        (void)co_await stream->write_exactly("HTTP/1.1 407 Proxy Authentication Required\r\n"
            "Proxy-Authenticate: Basic realm=proxy\r\nContent-Length: 0\r\n"
            "Connection: keep-alive\r\n\r\n", token);
    }
}

task<std::vector<client_result<response>>> request_targets(client& agent,
                                                         const std::vector<url>& targets) {
    std::vector<client_result<response>> results;
    for (const auto& target : targets) {
        request req(method::GET, "/isolation");
        req.set_header("Proxy-Authorization", "Basic caller-secret");
        results.push_back(co_await agent.send_result(req, target));
    }
    co_return results;
}

task<void> isolate_targets(elio::net::tcp_listener& listener, bool secure,
        elio::tls::tls_context& context, observed_route& observed, elio::coro::cancel_token token) {
    std::array<std::optional<elio::net::stream>, 2> retained;
    for (auto& session : retained) {
        auto accepted = co_await listener.accept(token);
        if (!accepted) co_return;
        ++observed.accepted;
        if (secure) {
            auto setup = co_await receive_request(*accepted, token);
            if (!setup) co_return;
            observed.requests.push_back(std::move(*setup));
            if ((co_await accepted->write_exactly("HTTP/1.1 200 Tunnel\r\n\r\n", token)).result <= 0)
                co_return;
            elio::tls::tls_stream origin(std::move(*accepted), context);
            if (!(co_await origin.handshake(token))) co_return;
            session.emplace(std::move(origin));
        } else session.emplace(std::move(*accepted));
        auto incoming = co_await receive_request(*session, token);
        if (!incoming) co_return;
        observed.requests.push_back(std::move(*incoming));
        if ((co_await session->write_exactly(
            "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok", token)).result <= 0) co_return;
        // Keep the first channel physically open while the different target
        // acquires its own route; accidental reuse would stall on this channel.
    }
    elio::sync::event waiting;
    (void)co_await waiting.wait(token);
    for (auto& session : retained) {
        if (session && session->is_tls()) co_await session->as_tls().abort_and_settle();
    }
}

} // namespace

namespace {

struct handoff_observation {
    std::optional<detail::route_connection> prepared;
    size_t dials = 0;
    std::latch published{1};
    std::latch release{1};
};

std::atomic<handoff_observation*> handoff_observed{nullptr};

task<client_result<detail::route_connection>> handoff_channel(const detail::route_plan&) {
    auto& observed = *handoff_observed.load();
    ++observed.dials;
    if (!observed.prepared) co_return detail::make_client_error(EIO, client_stage::connect);
    auto result = std::move(*observed.prepared);
    observed.prepared.reset();
    co_return result;
}

void pause_published_return() {
    auto& observed = *handoff_observed.load();
    observed.published.count_down();
    observed.release.wait();
}

struct handoff_hooks {
    explicit handoff_hooks(handoff_observation& value, bool pause_return = true) {
        handoff_observed.store(&value);
        detail::route_channel_for_test.store(handoff_channel);
        detail::lease_after_publish_for_test.store(
            pause_return ? pause_published_return : nullptr);
    }
    ~handoff_hooks() {
        detail::route_channel_for_test.store(nullptr);
        detail::lease_after_publish_for_test.store(nullptr);
        handoff_observed.store(nullptr);
    }
};

struct clear_observation {
    std::latch visible{1};
    std::latch release{1};
};

std::atomic<clear_observation*> clear_observed{nullptr};

void pause_visible_clear() {
    auto& observed = *clear_observed.load();
    observed.visible.count_down();
    observed.release.wait();
}

struct clear_hooks {
    explicit clear_hooks(clear_observation& value) {
        clear_observed.store(&value);
        detail::transport_clear_visible_for_test.store(pause_visible_clear);
    }
    ~clear_hooks() {
        detail::transport_clear_visible_for_test.store(nullptr);
        clear_observed.store(nullptr);
    }
};

struct output_observation {
    uint64_t handshake_bytes = 0;
    bool hold_inactive = false;
    std::atomic<bool> held{false};
    elio::sync::event paused;
    route_test_phase release;
};

std::atomic<output_observation*> output_observed{nullptr};

task<void> pause_drained_output(void* context, uint64_t drained) {
    auto& observed = *static_cast<output_observation*>(context);
    if (drained <= observed.handshake_bytes || observed.held.exchange(true)) co_return;
    observed.paused.set();
    // Intentionally retain the owned pump after cancellation, until the test
    // releases it. Its lower write has completed, but its frame has not settled.
    (void)co_await observed.release.wait();
}

void observe_tunnel_output(detail::connect_tls_stream& inner) {
    auto* observed = output_observed.load();
    observed->handshake_bytes = inner.finish_state_for_test().accepted_ciphertext;
    inner.set_output_progress_test_hook(observed,
        observed->hold_inactive ? nullptr : pause_drained_output,
        observed->hold_inactive ? pause_drained_output : nullptr);
}

struct output_hooks {
    explicit output_hooks(output_observation& value) {
        output_observed.store(&value);
        detail::tunnel_ready_for_test.store(observe_tunnel_output);
    }
    ~output_hooks() {
        detail::tunnel_ready_for_test.store(nullptr);
        output_observed.store(nullptr);
    }
};

void settle_connect_fixture(output_observation& output, elio::coro::cancel_source& stop,
        std::exception_ptr& failure, bool cancel = false) noexcept {
    output.release.set();
    if (cancel || failure) {
        try { stop.cancel(); }
        catch (...) { if (!failure) failure = std::current_exception(); }
    }
}

template<typename PeerStart, typename OperationStart>
auto launch_fixture_pair(elio::runtime::scheduler& scheduler,
        elio::coro::cancel_source& peer_stop, PeerStart peer_start,
        OperationStart operation_start) {
    using peer_handle = decltype(scheduler.go_joinable(std::move(peer_start)));
    std::optional<peer_handle> peer;
    try {
        peer.emplace(scheduler.go_joinable(std::move(peer_start)));
        auto operation = scheduler.go_joinable(std::move(operation_start));
        return std::pair{std::move(*peer), std::move(operation)};
    } catch (...) {
        auto failure = std::current_exception();
        try { peer_stop.cancel(); } catch (...) {}
        if (peer) {
            peer->wait_destroyed();
            try { (void)peer->await_resume(); } catch (...) {}
        }
        std::rethrow_exception(failure);
    }
}

template<typename Factory>
auto admit_connect_fixture(Factory&& factory, output_observation& output,
        elio::coro::cancel_source& stop, std::exception_ptr& failure) {
    using handle_type = decltype(
        elio::runtime::scheduler::current()->go_joinable(std::move(factory)));
    std::optional<handle_type> admitted;
    try {
        admitted.emplace(elio::runtime::scheduler::current()->go_joinable(
            std::forward<Factory>(factory)));
    } catch (...) {
        if (!failure) failure = std::current_exception();
        settle_connect_fixture(output, stop, failure, true);
    }
    return admitted;
}

task<elio::coro::cancel_result> observe_connect_phase(
        elio::sync::event& phase, elio::coro::cancel_token token) {
    co_return co_await phase.wait(std::move(token));
}

task<bool> await_connect_fixture_event(elio::sync::event& expected, output_observation& output,
        elio::coro::cancel_source& stop, std::exception_ptr& failure,
        std::chrono::milliseconds budget = std::chrono::seconds(10)) {
    auto* scheduler = elio::runtime::scheduler::current();
    elio::coro::cancel_source phase_stop;
    std::optional<elio::coro::join_handle<elio::coro::cancel_result>> phase;
    bool reached = false;
    try {
        phase.emplace(scheduler->go_joinable(
            observe_connect_phase(expected, phase_stop.get_token())));
        reached = co_await phase->wait_until(std::chrono::steady_clock::now() + budget) ==
            elio::coro::join_wait_outcome::completed;
    } catch (...) { if (!failure) failure = std::current_exception(); }
    if (!reached || failure) settle_connect_fixture(output, stop, failure, true);
    try { phase_stop.cancel(); }
    catch (...) { if (!failure) failure = std::current_exception(); }
    if (phase) {
        try {
            auto& joined = *phase;
            reached = co_await joined == elio::coro::cancel_result::completed && reached;
        } catch (...) { if (!failure) failure = std::current_exception(); }
        try { co_await phase->wait_destroyed_async(); }
        catch (...) { if (!failure) failure = std::current_exception(); }
    }
    if (failure) settle_connect_fixture(output, stop, failure, true);
    co_return reached && !failure;
}

task<bool> await_connect_fixture_phase(output_observation& output,
        elio::coro::cancel_source& stop, std::exception_ptr& failure,
        std::chrono::milliseconds budget = std::chrono::seconds(10)) {
    return await_connect_fixture_event(output.paused, output, stop, failure, budget);
}

struct returned_thread {
    handoff_observation& observed;
    std::thread worker;
    ~returned_thread() {
        if (worker.joinable()) {
            observed.release.count_down();
            worker.join();
        }
    }
    void join() {
        observed.release.count_down();
        worker.join();
    }
};

struct cleared_thread {
    clear_observation& observed;
    std::thread worker;
    ~cleared_thread() {
        if (worker.joinable()) {
            observed.release.count_down();
            worker.join();
        }
    }
    void join() {
        observed.release.count_down();
        worker.join();
    }
};

template<typename T>
T handoff_immediate(task<T> operation) {
    auto frame = elio::coro::detail::task_access::handle(operation);
    frame.resume();
    if (!frame.done()) throw std::runtime_error("handoff fixture unexpectedly suspended");
    return operation.await_resume();
}

} // namespace

TEST_CASE("Idle handoff cannot erase the next tunnel lease retirement owner",
          "[http][proxy][routes][handoff][issue-1249]") {
    using namespace elio::http::detail;
    for (const bool finite : {false, true}) {
        CAPTURE(finite);
        transport_config config;
        if (finite) config.limits = pool_limits{};
        auto owner = std::make_shared<transport>(config);
        const auto target = url::parse("https://localhost/");
        REQUIRE(target);
        std::array<int, 2> descriptors{};
        REQUIRE(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC,
                             0, descriptors.data()) == 0);
        elio::net::tcp_stream peer{descriptors[1]};
        elio::tls::tls_context context(elio::tls::tls_mode::client);
        auto anchor = std::make_shared<route_retirement>();
        connect_channel lower(elio::net::tcp_stream{descriptors[0]}, {}, 0, anchor);
        std::array<char, 1> bytes{};
        auto pending = std::make_unique<task<elio::io::io_result>>(
            lower.read(bytes.data(), bytes.size(), {}));
        handoff_observation observed;
        observed.prepared.emplace(connect_tls_stream(std::move(lower), context), std::move(anchor));
        handoff_hooks hooks(observed);
        auto first_result = handoff_immediate(owner->acquire_lease_for_test(*target));
        REQUIRE(std::holds_alternative<transport::connection_lease_for_test>(first_result));
        auto first = std::move(std::get<transport::connection_lease_for_test>(first_result));
        std::exception_ptr return_failure;
        std::atomic<bool> return_done{false};
        returned_thread returning{observed, std::thread([&] {
            try { transport::return_lease_for_test(first); }
            catch (...) { return_failure = std::current_exception(); }
            return_done.store(true, std::memory_order_release);
        })};
        const auto publish_deadline = std::chrono::steady_clock::now() +
            std::chrono::seconds(5);
        while (!observed.published.try_wait() &&
               !return_done.load(std::memory_order_acquire) &&
               std::chrono::steady_clock::now() < publish_deadline) {
            std::this_thread::yield();
        }
        const bool published = observed.published.try_wait();
        if (!published) {
            returning.join();
            if (return_failure) std::rethrow_exception(return_failure);
            throw std::runtime_error("lease return did not reach the publication hook");
        }
        auto second_result = handoff_immediate(owner->acquire_lease_for_test(*target));
        returning.join();
        if (return_failure) std::rethrow_exception(return_failure);
        REQUIRE(std::holds_alternative<transport::connection_lease_for_test>(second_result));
        REQUIRE(observed.dials == 1);
        auto second = std::move(std::get<transport::connection_lease_for_test>(second_result));
        second.retire();
        REQUIRE(::fcntl(descriptors[0], F_GETFD) >= 0);
        CHECK(owner->active_operations_for_test() == 1);
        if (finite) CHECK(owner->admission_counters_for_test().live == 1);
        pending.reset();
        CHECK(::fcntl(descriptors[0], F_GETFD) == -1);
        CHECK(owner->active_operations_for_test() == 0);
        if (finite) CHECK(owner->admission_counters_for_test().live == 0);
    }
}

TEST_CASE("Idle tunnel roots remain shutdown operations until their lower frames release",
          "[http][proxy][routes][handoff][shutdown][issue-1249]") {
    using namespace elio::http::detail;
    for (const bool finite : {false, true}) {
        CAPTURE(finite);
        transport_config config;
        if (finite) config.limits = pool_limits{};
        auto owner = std::make_shared<transport>(config);
        const auto target = url::parse("https://localhost/");
        REQUIRE(target);
        std::array<int, 2> descriptors{};
        REQUIRE(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC,
                             0, descriptors.data()) == 0);
        elio::net::tcp_stream peer{descriptors[1]};
        elio::tls::tls_context context(elio::tls::tls_mode::client);
        auto anchor = std::make_shared<route_retirement>();
        connect_channel lower(elio::net::tcp_stream{descriptors[0]}, {}, 0, anchor);
        std::array<char, 1> bytes{};
        auto pending = std::make_unique<task<elio::io::io_result>>(
            lower.read(bytes.data(), bytes.size(), {}));
        handoff_observation observed;
        observed.prepared.emplace(connect_tls_stream(std::move(lower), context),
                                  std::move(anchor));
        handoff_hooks hooks(observed, false);
        auto acquired = handoff_immediate(owner->acquire_lease_for_test(*target));
        REQUIRE(std::holds_alternative<transport::connection_lease_for_test>(acquired));
        auto lease = std::move(std::get<transport::connection_lease_for_test>(acquired));
        transport::return_lease_for_test(lease);
        REQUIRE(owner->active_operations_for_test() == 1);
        if (finite) REQUIRE(owner->admission_counters_for_test().idle == 1);

        auto shutdown = owner->shutdown();
        auto frame = elio::coro::detail::task_access::handle(shutdown);
        frame.resume();
        const bool waited_for_root = !frame.done();
        CHECK(waited_for_root);
        CHECK(owner->active_operations_for_test() == (waited_for_root ? 1 : 0));
        if (finite) CHECK(owner->admission_counters_for_test().live == 1);

        pending.reset();
        CHECK(frame.done());
        if (frame.done())
            CHECK(shutdown.await_resume() == elio::coro::cancel_result::completed);
        CHECK(owner->active_operations_for_test() == 0);
        if (finite) CHECK(owner->admission_counters_for_test().live == 0);
        CHECK(::fcntl(descriptors[0], F_GETFD) == -1);
    }
}

TEST_CASE("Clear owns published tunnel retirement before its active lease resets",
          "[http][proxy][routes][handoff][shutdown][issue-1249]") {
    using namespace elio::http::detail;
    for (const bool finite : {false, true}) {
        CAPTURE(finite);
        transport_config config;
        if (finite) config.limits = pool_limits{};
        auto owner = std::make_shared<transport>(config);
        const auto target = url::parse("https://localhost/");
        REQUIRE(target);
        std::array<int, 2> descriptors{};
        REQUIRE(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC,
                             0, descriptors.data()) == 0);
        elio::net::tcp_stream peer{descriptors[1]};
        elio::tls::tls_context context(elio::tls::tls_mode::client);
        auto anchor = std::make_shared<route_retirement>();
        connect_channel lower(elio::net::tcp_stream{descriptors[0]}, {}, 0, anchor);
        std::array<char, 1> bytes{};
        auto pending = std::make_unique<task<elio::io::io_result>>(
            lower.read(bytes.data(), bytes.size(), {}));
        handoff_observation handoff;
        handoff.prepared.emplace(connect_tls_stream(std::move(lower), context),
                                 std::move(anchor));
        handoff_hooks handoff_scope(handoff);
        clear_observation cleared;
        clear_hooks clear_scope(cleared);
        auto acquired = handoff_immediate(owner->acquire_lease_for_test(*target));
        REQUIRE(std::holds_alternative<transport::connection_lease_for_test>(acquired));
        auto lease = std::move(std::get<transport::connection_lease_for_test>(acquired));

        std::exception_ptr return_failure;
        std::atomic<bool> return_done{false};
        returned_thread returning{handoff, std::thread([&] {
            try { transport::return_lease_for_test(lease); }
            catch (...) { return_failure = std::current_exception(); }
            return_done.store(true, std::memory_order_release);
        })};
        const auto publish_deadline = std::chrono::steady_clock::now() +
            std::chrono::seconds(5);
        while (!handoff.published.try_wait() &&
               !return_done.load(std::memory_order_acquire) &&
               std::chrono::steady_clock::now() < publish_deadline) {
            std::this_thread::yield();
        }
        REQUIRE(handoff.published.try_wait());

        std::exception_ptr clear_failure;
        std::atomic<bool> clear_done{false};
        cleared_thread clearing{cleared, std::thread([&] {
            try { owner->clear(); }
            catch (...) { clear_failure = std::current_exception(); }
            clear_done.store(true, std::memory_order_release);
        })};
        const auto clear_deadline = std::chrono::steady_clock::now() +
            std::chrono::seconds(5);
        while (!cleared.visible.try_wait() &&
               !clear_done.load(std::memory_order_acquire) &&
               std::chrono::steady_clock::now() < clear_deadline) {
            std::this_thread::yield();
        }
        REQUIRE(cleared.visible.try_wait());

        returning.join();
        if (return_failure) std::rethrow_exception(return_failure);
        REQUIRE(owner->active_operations_for_test() == 1);
        if (finite) REQUIRE(owner->admission_counters_for_test().live == 1);

        auto shutdown = owner->shutdown();
        auto frame = elio::coro::detail::task_access::handle(shutdown);
        frame.resume();
        CHECK_FALSE(frame.done());

        clearing.join();
        if (clear_failure) std::rethrow_exception(clear_failure);
        CHECK_FALSE(frame.done());
        CHECK(owner->active_operations_for_test() == 1);

        pending.reset();
        CHECK(frame.done());
        if (frame.done())
            CHECK(shutdown.await_resume() == elio::coro::cancel_result::completed);
        CHECK(owner->active_operations_for_test() == 0);
        if (finite) CHECK(owner->admission_counters_for_test().live == 0);
        CHECK(::fcntl(descriptors[0], F_GETFD) == -1);
    }
}

TEST_CASE("Standalone connection pools reject explicit proxy policy rather than dial directly",
          "[http][proxy][routes][compatibility][issue-1249]") {
    transport_config transport_options;
    transport_options.proxy = http_proxy_config{};
    transport_options.proxy->endpoint = "http://proxy.example/";
    REQUIRE_THROWS_AS(connection_pool{transport_options}, std::invalid_argument);
    client_config client_options;
    client_options.proxy = *transport_options.proxy;
    REQUIRE_THROWS_AS(connection_pool{client_options}, std::invalid_argument);
    REQUIRE_NOTHROW(connection_pool{});
}

TEST_CASE("Proxy target authority rejects malformed grammar before dialing",
          "[http][proxy][routes][authority][review-1249][issue-1249]") {
    transport_config config;
    config.proxy = http_proxy_config{};
    config.proxy->endpoint = "http://proxy.example/";
    auto owner = std::make_shared<transport>(config);
    client agent(owner);
    handoff_observation observed;
    handoff_hooks hooks(observed);
    for (const auto input : {"http://user:pa@ss@origin.example/path",
                             "https://user:pa@ss@origin.example/path",
                             "http://origin%ZZ.example/path", "https://origin%ZZ.example/path",
                             "http://origin\\host/path", "https://origin\\host/path",
                             "http://[::gg]/path", "https://[::gg]/path",
                             "http://[not-ip]/path", "https://[not-ip]/path",
                             "http://[v1.name]/path", "https://[v1.name]/path",
                             "http://[fe80::1%25ethA]/path", "https://[fe80::1%25ethA]/path"}) {
        auto result = handoff_immediate(agent.get_result(input));
        const auto* error = std::get_if<client_error>(&result);
        REQUIRE(error);
        CHECK(error->code.value() == EINVAL);
        CHECK(error->stage == client_stage::target);
    }
    CHECK(observed.dials == 0);
}

TEST_CASE("CONNECT rejects unsupported TLS reference names before acquisition",
          "[http][proxy][routes][origin-reference][issue-1249]") {
    transport_config config;
    config.proxy.emplace();
    config.proxy->endpoint = "http://proxy.example/";
    config.limits = pool_limits{};
    auto owner = std::make_shared<transport>(config);
    client agent(owner);
    handoff_observation observed;
    handoff_hooks hooks(observed);
    for (const auto input : {"https://origin%2Fhost/path", "https://origin%00host/path",
                             "https://origin%25host/path", "https://origin%5Chost/path",
                             "https://origin%C3%A9.example/path", "https://.example.com/path",
                             "https://%2eexample.com/path"}) {
        CAPTURE(input);
        auto result = handoff_immediate(agent.get_result(input));
        const auto* error = std::get_if<client_error>(&result);
        CHECK(error);
        if (error) {
            CHECK(error->code.value() == EINVAL);
            CHECK(error->stage == client_stage::target);
        }
    }
    CHECK(observed.dials == 0);
    CHECK(owner->admission_counters_for_test().live == 0);
    CHECK(owner->admission_counters_for_test().dialing == 0);
}

TEST_CASE("Explicit proxy clients reject caller CONNECT before transport acquisition",
          "[http][proxy][routes][caller-connect][issue-1249]") {
    const auto finite = GENERATE(false, true);
    transport_config config;
    config.proxy.emplace();
    config.proxy->endpoint = "http://proxy.example/";
    if (finite) config.limits = pool_limits{};
    auto owner = std::make_shared<transport>(config);
    client agent(owner);
    handoff_observation observed;
    handoff_hooks hooks(observed);
    for (const auto input : {"http://origin.example/path", "https://origin.example/path"}) {
        CAPTURE(finite, input);
        const auto target = url::parse(input);
        REQUIRE(target);
        request req(method::CONNECT, target->path);
        auto typed = handoff_immediate(agent.send_result(req, *target));
        auto text = handoff_immediate(agent.request_result(method::CONNECT, input));
        for (const auto* result : {&typed, &text}) {
            const auto* error = std::get_if<client_error>(result);
            REQUIRE(error);
            CHECK(error->code.value() == ENOTSUP);
            CHECK(error->stage == client_stage::request);
        }
        CHECK_FALSE(handoff_immediate(agent.send(req, *target)));
    }
    CHECK(observed.dials == 0);
    CHECK(owner->active_operations_for_test() == 0);
    if (finite) {
        CHECK(owner->admission_counters_for_test().live == 0);
        CHECK(owner->admission_counters_for_test().dialing == 0);
        CHECK(owner->admission_counters_for_test().idle == 0);
    }
}

TEST_CASE("Completed CONNECT responses do not settle shutdown before owned TLS output",
          "[http][proxy][routes][review-1249][issue-1249]") {
    elio::tls::tls_context server_context(elio::tls::tls_mode::server);
    temporary_pem ca;
    install_certificate(server_context, ca);
    const auto selected = GENERATE(backend::epoll, backend::io_uring);
    for (const size_t workers : {1, 2})
    for (const bool hold_inactive : {false, true})
    for (const bool finite : {false, true}) {
        CAPTURE(selected, workers, hold_inactive, finite);
        backend_guard backend_scope(selected);
        auto listener = elio::net::tcp_listener::bind(elio::net::ipv4_address("127.0.0.1", 0));
        REQUIRE(listener);
        transport_config config;
        config.proxy = http_proxy_config{};
        config.proxy->endpoint = "http://127.0.0.1:" + std::to_string(listener->local_address().port());
        config.configure_tls = [&](transport_tls_config& policy) {
            if (!policy.load_verify_locations(ca.path.data()))
                throw std::runtime_error("output fixture trust setup failed");
        };
        if (finite) config.limits = pool_limits{};
        auto owner = std::make_shared<transport>(config);
        client agent(owner);
        output_observation output;
        output.hold_inactive = hold_inactive;
        observed_route observed;
        authentication_hooks authentication_scope(observed);
        output_hooks hooks(output);
        struct {
            bool paused_before_recovery = false;
            bool call_completed_before_recovery = false;
            bool call_ready_before_recovery = false;
            bool result_ready_before_recovery = false;
            bool observer_expired = false;
            int64_t wait_milliseconds = 0;
        } phase_snapshot;
        std::atomic<bool> call_completed{false};
        elio::coro::cancel_source stop;
        elio::runtime::scheduler scheduler(workers);
        scheduler.start();
        auto [server, controlled] = launch_fixture_pair(scheduler, stop,
            serve_route(*listener, true, server_context, 1, observed, stop.get_token()),
            [&]() -> task<client_result<response>> {
            elio::coro::cancel_source call_stop;
            std::exception_ptr failure;
            auto call = admit_connect_fixture([&]() -> task<client_result<response>> {
                try {
                    auto result = co_await agent.get_result(
                        "https://localhost/path", call_stop.get_token());
                    call_completed.store(true, std::memory_order_release);
                    co_return result;
                } catch (...) {
                    call_completed.store(true, std::memory_order_release);
                    throw;
                }
            }, output, call_stop, failure);
            bool paused = false;
            try {
                if (call) {
                    const auto started = std::chrono::steady_clock::now();
                    elio::coro::cancel_source phase_stop;
                    std::optional<elio::coro::join_handle<elio::coro::cancel_result>> phase;
                    bool observed_pause = false;
                    try {
                        phase.emplace(elio::runtime::scheduler::current()->go_joinable(
                            observe_connect_phase(output.paused, phase_stop.get_token())));
                        observed_pause = co_await phase->wait_until(
                            started + std::chrono::seconds(10)) ==
                            elio::coro::join_wait_outcome::completed;
                    } catch (...) {
                        if (!failure) failure = std::current_exception();
                    }
                    const auto observed_at = std::chrono::steady_clock::now();
                    phase_snapshot.paused_before_recovery = output.paused.is_set();
                    phase_snapshot.call_completed_before_recovery =
                        call_completed.load(std::memory_order_acquire);
                    phase_snapshot.call_ready_before_recovery = call->is_ready();
                    phase_snapshot.observer_expired = !observed_pause;
                    phase_snapshot.wait_milliseconds =
                        std::chrono::duration_cast<std::chrono::milliseconds>(
                            observed_at - started).count();
                    paused = phase_snapshot.paused_before_recovery;
                    try { phase_stop.cancel(); }
                    catch (...) { if (!failure) failure = std::current_exception(); }
                    if (phase) {
                        try {
                            auto& joined = *phase;
                            (void)co_await joined;
                        } catch (...) { if (!failure) failure = std::current_exception(); }
                        try { co_await phase->wait_destroyed_async(); }
                        catch (...) { if (!failure) failure = std::current_exception(); }
                    }
                    if (paused && !failure) {
                        phase_snapshot.result_ready_before_recovery =
                            co_await call->wait_until(std::chrono::steady_clock::now() +
                                std::chrono::seconds(10)) ==
                            elio::coro::join_wait_outcome::completed;
                    } else phase_snapshot.result_ready_before_recovery =
                        phase_snapshot.call_ready_before_recovery;
                    if (!paused || !phase_snapshot.result_ready_before_recovery || failure)
                        settle_connect_fixture(output, call_stop, failure, true);
                }
            } catch (...) {
                if (!failure) failure = std::current_exception();
                settle_connect_fixture(output, call_stop, failure, true);
            }
            std::optional<client_result<response>> result;
            if (call) {
                auto& joined = *call;
                try { result.emplace(co_await joined); }
                catch (...) {
                    if (!failure) failure = std::current_exception();
                    settle_connect_fixture(output, call_stop, failure, true);
                }
                try { co_await call->wait_destroyed_async(); }
                catch (...) {
                    if (!failure) failure = std::current_exception();
                    settle_connect_fixture(output, call_stop, failure, true);
                }
            }
            if (paused && !failure) {
                CHECK(owner->active_operations_for_test() == 1);
                if (finite) {
                    CHECK(owner->admission_counters_for_test().live == 1);
                    CHECK(owner->admission_counters_for_test().idle == 0);
                }
            }
            elio::sync::event shutdown_entered;
            auto shutdown = admit_connect_fixture([&]() -> task<elio::coro::cancel_result> {
                shutdown_entered.set();
                co_return co_await owner->shutdown();
            }, output, call_stop, failure);
            bool shutdown_started = false;
            try {
                if (shutdown) {
                    const auto deadline = std::chrono::steady_clock::now() +
                        std::chrono::seconds(5);
                    while (!shutdown_entered.is_set() && !shutdown->is_ready() &&
                           std::chrono::steady_clock::now() < deadline) {
                        co_await elio::time::yield();
                    }
                    shutdown_started = shutdown_entered.is_set();
                }
            } catch (...) {
                if (!failure) failure = std::current_exception();
                settle_connect_fixture(output, call_stop, failure, true);
            }
            if (!failure) CHECK(shutdown_started);
            if (paused && shutdown_started && !failure) CHECK_FALSE(shutdown->is_ready());
            settle_connect_fixture(output, call_stop, failure);
            std::optional<elio::coro::cancel_result> shutdown_result;
            if (shutdown) {
                auto& joined = *shutdown;
                try { shutdown_result.emplace(co_await joined); }
                catch (...) {
                    if (!failure) failure = std::current_exception();
                    settle_connect_fixture(output, call_stop, failure, true);
                }
                try { co_await shutdown->wait_destroyed_async(); }
                catch (...) { if (!failure) failure = std::current_exception(); }
            }
            if (!failure) {
                REQUIRE(result);
                REQUIRE(shutdown_result);
                CHECK(*shutdown_result == elio::coro::cancel_result::completed);
                CHECK(owner->active_operations_for_test() == 0);
                if (finite) CHECK(owner->admission_counters_for_test().live == 0);
            }
            if (failure) std::rethrow_exception(failure);
            co_return std::move(*result);
        });
        controlled.wait_destroyed();
        stop.cancel();
        server.wait_destroyed();
        scheduler.shutdown();
        auto result = controlled.await_resume();
        server.await_resume();
        const auto* error = std::get_if<client_error>(&result);
        const int error_code = error ? error->code.value() : 0;
        const int error_stage = error ? static_cast<int>(error->stage) : -1;
        CAPTURE(phase_snapshot.paused_before_recovery,
                phase_snapshot.call_completed_before_recovery,
                phase_snapshot.call_ready_before_recovery,
                phase_snapshot.result_ready_before_recovery,
                phase_snapshot.observer_expired, phase_snapshot.wait_milliseconds,
                error_code, error_stage, output.paused.is_set(), output.release.is_set(),
                output.held.load(std::memory_order_acquire), output.handshake_bytes,
                observed.accepted, observed.accept_error, observed.connect_read.bytes,
                observed.connect_read.terminal_error, observed.connect_read.complete,
                observed.handshake, observed.server_handshake_error,
                observed.client_handshake_failures, observed.client_handshake_error,
                observed.client_verification, observed.client_connect_writing,
                observed.client_connect_written, observed.client_connect_reading,
                observed.client_connect_received, observed.client_connect_error,
                observed.client_setup_deadline.has_value());
        CHECK(phase_snapshot.paused_before_recovery);
        CHECK(phase_snapshot.result_ready_before_recovery);
        REQUIRE(std::holds_alternative<response>(result));
        REQUIRE(std::get<response>(result).body() == "ok");
        REQUIRE(observed.accepted == 1);
    }
}

TEST_CASE("CONNECT output fixture cleans up when an early error never reaches its hook",
          "[http][proxy][routes][fixture][issue-1249]") {
    const auto selected = GENERATE(backend::epoll, backend::io_uring);
    backend_guard backend_scope(selected);
    transport_config config;
    config.proxy.emplace();
    config.proxy->endpoint = "http://127.0.0.1:1";
    auto owner = std::make_shared<transport>(config);
    client agent(owner);
    output_observation output;
    elio::coro::cancel_source stop;
    std::exception_ptr failure;
    bool paused = true;
    elio::runtime::scheduler scheduler(1);
    scheduler.start();
    auto controller = scheduler.go_joinable([&]() -> task<client_result<response>> {
        // Invalid identity returns a real owned target error before acquisition;
        // no output hook can fire. The fixture still joins every observer/frame.
        auto call = admit_connect_fixture([&] {
            return agent.get_result("https://[not-ip]/path", stop.get_token());
        }, output, stop, failure);
        try {
            if (call) paused = co_await await_connect_fixture_phase(
                output, stop, failure, std::chrono::milliseconds(10));
        } catch (...) {
            if (!failure) failure = std::current_exception();
            settle_connect_fixture(output, stop, failure, true);
        }
        std::optional<client_result<response>> result;
        if (call) {
            auto& joined = *call;
            try { result.emplace(co_await joined); }
            catch (...) {
                if (!failure) failure = std::current_exception();
                settle_connect_fixture(output, stop, failure, true);
            }
            try { co_await call->wait_destroyed_async(); }
            catch (...) { if (!failure) failure = std::current_exception(); }
        }
        try { (void)co_await owner->shutdown(); }
        catch (...) { if (!failure) failure = std::current_exception(); }
        if (failure) std::rethrow_exception(failure);
        co_return std::move(*result);
    });
    controller.wait_destroyed();
    scheduler.shutdown();
    auto result = controller.await_resume();
    REQUIRE_FALSE(paused);
    REQUIRE(output.release.is_set());
    REQUIRE(stop.is_cancelled());
    REQUIRE_FALSE(failure);
    const auto* error = std::get_if<client_error>(&result);
    REQUIRE(error);
    CHECK(error->stage == client_stage::target);
    CHECK(error->code.value() == EINVAL);
    CHECK(owner->active_operations_for_test() == 0);
}

TEST_CASE("CONNECT 407 is bounded and never pools or automatically replays a rejected channel",
          "[http][proxy][routes][auth][issue-1249]") {
    const auto selected = GENERATE(backend::epoll, backend::io_uring);
    {
        backend_guard backend_scope(selected);
        auto listener = elio::net::tcp_listener::bind(elio::net::ipv4_address("127.0.0.1", 0));
        REQUIRE(listener);
        transport_config config;
        config.proxy.emplace();
        config.proxy->endpoint = "http://127.0.0.1:" + std::to_string(listener->local_address().port());
        config.proxy->basic_auth = proxy_basic_credentials{"user", "password"};
        config.limits = pool_limits{};
        config.limits->max_live_total = 1;
        auto owner = std::make_shared<transport>(config);
        client agent(owner);
        const auto target = url::parse("https://unresolved-origin.invalid/");
        REQUIRE(target);
        const std::vector<url> targets{*target, *target};
        observed_route observed;
        elio::coro::cancel_source stop;
        elio::runtime::scheduler scheduler(1);
        scheduler.start();
        auto [server, calls] = launch_fixture_pair(scheduler, stop,
            reject_connect(*listener, observed, stop.get_token()),
            request_targets(agent, targets));
        calls.wait_destroyed();
        stop.cancel();
        server.wait_destroyed();
        owner->clear();
        scheduler.shutdown();
        auto results = calls.await_resume();
        server.await_resume();
        REQUIRE(results.size() == 2);
        for (const auto& result : results) {
            const auto* error = std::get_if<client_error>(&result);
            REQUIRE(error);
            REQUIRE(error->code.value() == EACCES);
            REQUIRE(error->stage == client_stage::proxy_connect);
        }
        REQUIRE(observed.accepted == 2);
        REQUIRE(observed.requests.size() == 2);
        for (const auto& incoming : observed.requests) {
            REQUIRE(incoming.get_method() == method::CONNECT);
            REQUIRE(incoming.body().empty());
            REQUIRE(incoming.header("Proxy-Authorization") ==
                    detail::proxy_basic_authorization(*config.proxy->basic_auth));
        }
        REQUIRE(owner->admission_counters_for_test().live == 0);
    }
}

TEST_CASE("Functional forward and CONNECT pools do not share channels across target authorities",
          "[http][proxy][routes][isolation][issue-1249]") {
    elio::tls::tls_context server_context(elio::tls::tls_mode::server);
    temporary_pem ca;
    install_certificate(server_context, ca);
    const auto selected = GENERATE(backend::epoll, backend::io_uring);
    for (const bool secure : {false, true}) {
        CAPTURE(selected, secure);
        backend_guard backend_scope(selected);
        auto listener = elio::net::tcp_listener::bind(elio::net::ipv4_address("127.0.0.1", 0));
        REQUIRE(listener);
        transport_config config;
        config.verify_certificate = false;
        config.proxy.emplace();
        config.proxy->endpoint = "http://127.0.0.1:" + std::to_string(listener->local_address().port());
        config.limits = pool_limits{};
        config.limits->max_live_total = 2;
        config.limits->max_live_per_route = 1;
        auto owner = std::make_shared<transport>(config);
        client_config policy;
        policy.read_timeout = std::chrono::seconds(5);
        client agent(owner, policy);
        const auto first = url::parse(secure ? "https://first.invalid:443/" : "http://first.invalid:80/");
        const auto second = url::parse(secure ? "https://second.invalid:8443/" : "http://second.invalid:8081/");
        REQUIRE(first);
        REQUIRE(second);
        const std::vector<url> targets{*first, *second};
        observed_route observed;
        elio::coro::cancel_source stop;
        elio::runtime::scheduler scheduler(1);
        scheduler.start();
        auto [server, calls] = launch_fixture_pair(scheduler, stop,
            isolate_targets(*listener, secure, server_context, observed, stop.get_token()),
            request_targets(agent, targets));
        calls.wait_destroyed();
        stop.cancel();
        server.wait_destroyed();
        owner->clear();
        scheduler.shutdown();
        auto results = calls.await_resume();
        server.await_resume();
        REQUIRE(results.size() == 2);
        for (const auto& result : results) {
            REQUIRE(std::holds_alternative<response>(result));
            REQUIRE(std::get<response>(result).body() == "ok");
        }
        REQUIRE(observed.accepted == 2);
        REQUIRE(observed.requests.size() == (secure ? 4 : 2));
        for (size_t i = 0; i < 2; ++i) {
            if (secure) REQUIRE(observed.requests[i * 2].path() ==
                                detail::route_endpoint::from(targets[i].host, targets[i].effective_port()).authority());
            const auto& incoming = observed.requests[i * (secure ? 2 : 1) + (secure ? 1 : 0)];
            REQUIRE(incoming.header("Host") == targets[i].host_authority());
            REQUIRE(incoming.header("Proxy-Authorization").empty());
        }
        REQUIRE(owner->admission_counters_for_test().live == 0);
    }
}

TEST_CASE("Forward-to-CONNECT redirects keep credentials only on the proxy hop",
          "[http][proxy][routes][redirect][issue-1249]") {
    elio::tls::tls_context server_context(elio::tls::tls_mode::server);
    temporary_pem ca;
    install_certificate(server_context, ca);
    const auto selected = GENERATE(backend::epoll, backend::io_uring);
    {
        CAPTURE(selected);
        backend_guard backend_scope(selected);
        auto listener = elio::net::tcp_listener::bind(elio::net::ipv4_address("127.0.0.1", 0));
        REQUIRE(listener);
        transport_config config;
        config.proxy = http_proxy_config{};
        config.proxy->endpoint = "http://127.0.0.1:" + std::to_string(listener->local_address().port());
        config.proxy->basic_auth = proxy_basic_credentials{"hop", "secret"};
        config.limits = pool_limits{};
        config.limits->max_live_total = 2;
        config.limits->max_live_per_route = 1;
        config.acquisition_timeout = std::chrono::seconds(5);
        config.configure_tls = [&](transport_tls_config& policy) {
            if (!policy.load_verify_locations(ca.path.data()))
                throw std::runtime_error("redirect fixture trust setup failed");
        };
        auto owner = std::make_shared<transport>(config);
        client agent(owner);
        const auto target = url::parse("http://uri-user:uri-secret@first.invalid:8081/path#not-on-wire");
        REQUIRE(target);
        observed_route observed;
        elio::coro::cancel_source stop;
        elio::runtime::scheduler scheduler(1);
        scheduler.start();
        auto [server, calls] = launch_fixture_pair(scheduler, stop,
            redirect_route(*listener, server_context, observed, stop.get_token()),
            send_requests(agent, *target, 1));
        calls.wait_destroyed();
        stop.cancel();
        server.wait_destroyed();
        owner->clear();
        scheduler.shutdown();
        auto results = calls.await_resume();
        server.await_resume();
        REQUIRE(results.size() == 1);
        REQUIRE(std::holds_alternative<response>(results[0]));
        REQUIRE(std::get<response>(results[0]).body() == "ok");
        REQUIRE(observed.accepted == 2);
        REQUIRE(observed.requests.size() == 3);
        const auto authorization = detail::proxy_basic_authorization(*config.proxy->basic_auth);
        REQUIRE(observed.requests[0].path_with_query() == "http://first.invalid:8081/path?q=0");
        REQUIRE(observed.requests[0].header("Proxy-Authorization") == authorization);
        REQUIRE(observed.requests[1].get_method() == method::CONNECT);
        REQUIRE(observed.requests[1].path() == "localhost:9443");
        REQUIRE(observed.requests[1].header("Proxy-Authorization") == authorization);
        REQUIRE(observed.requests[2].path_with_query() == "/final?done=1");
        REQUIRE(observed.requests[2].header("Host") == "localhost:9443");
        REQUIRE(observed.requests[2].header("Proxy-Authorization").empty());
        REQUIRE(owner->admission_counters_for_test().live == 0);
    }
}

TEST_CASE("Route operation joins an already-ready admitted watchdog before rethrowing",
          "[http][proxy][routes][watchdog-ready-destruction][issue-1249]") {
    const auto controller_failure = GENERATE(false, true);
    CAPTURE(controller_failure);
    route_ready_probe probe;
    route_ready_guard ready(probe);
    elio::runtime::scheduler scheduler(2);
    scheduler.start();
    auto operation = scheduler.go_joinable_to(0, exercise_ready_route_watchdog(probe));
    bool timer_ready = false;
    bool parent_observed = false;
    bool completed_before_destroy = false;
    bool destruction_observed = false;
    std::exception_ptr control_failure;
    try {
        timer_ready = observe_route_ready([&] {
            return probe.timer_failed.load(std::memory_order_acquire) &&
                   probe.parent_waiting.load(std::memory_order_acquire) &&
                   elio::coro::detail::detached_frame_destroy_paused_for_test.load(
                       std::memory_order_acquire);
        });
        if (controller_failure) throw std::runtime_error("route fixture controller failed");
        probe.release_parent.set();
        parent_observed = observe_route_ready([&] {
            return probe.done.load(std::memory_order_acquire) ||
                   probe.destruction_observed.load(std::memory_order_acquire);
        });
        completed_before_destroy = probe.done.load(std::memory_order_acquire);
        destruction_observed = probe.destruction_observed.load(std::memory_order_acquire);
    } catch (...) { control_failure = std::current_exception(); }
    ready.release();
    const bool completed = observe_route_ready(
        [&] { return probe.done.load(std::memory_order_acquire); });
    operation.wait_destroyed();
    const bool drained = scheduler.shutdown(std::chrono::seconds(5));
    operation.await_resume();
    REQUIRE(drained);
    CHECK(timer_ready);
    if (controller_failure) {
        REQUIRE(control_failure);
        try { std::rethrow_exception(control_failure); }
        catch (const std::runtime_error& error) {
            CHECK(std::string_view(error.what()) == "route fixture controller failed");
        }
    } else {
        if (control_failure) std::rethrow_exception(control_failure);
        CHECK(parent_observed);
        CHECK_FALSE(completed_before_destroy);
        CHECK(destruction_observed);
    }
    CHECK(completed);
    CHECK_FALSE(probe.operation_entered.load(std::memory_order_acquire));
    REQUIRE(probe.failure);
    try { std::rethrow_exception(probe.failure); }
    catch (const std::runtime_error& error) {
        CHECK(std::string_view(error.what()) == "ready route watchdog failed");
    }
}

TEST_CASE("Rejected response watchdog admission never starts a CONNECT tunnel read",
          "[http][proxy][routes][watchdog][shutdown][issue-1249]") {
    backend_guard backend_scope(backend::epoll);
    elio::tls::tls_context server_context(elio::tls::tls_mode::server);
    temporary_pem ca;
    install_certificate(server_context, ca);
    auto listener = elio::net::tcp_listener::bind(
        elio::net::ipv4_address("127.0.0.1", 0));
    REQUIRE(listener);
    transport_config config;
    config.proxy.emplace();
    config.proxy->endpoint = "http://127.0.0.1:" +
        std::to_string(listener->local_address().port());
    config.configure_tls = [&](transport_tls_config& policy) {
        if (!policy.load_verify_locations(ca.path.data()))
            throw std::runtime_error("response admission trust setup failed");
    };
    auto owner = std::make_shared<transport>(config);
    client_config request_policy;
    request_policy.read_timeout = std::chrono::hours(1);
    client agent(owner, request_policy);
    observed_route observed;
    response_admission_observation admission;
    response_admission_guard hooks(admission);
    elio::coro::cancel_source stop;
    elio::runtime::scheduler scheduler(2);
    scheduler.start();
    auto server = scheduler.go_joinable(stall_tunnel_response(
        *listener, server_context, observed, stop.get_token()));
    auto call = scheduler.go_joinable(agent.get_result("https://localhost/path"));

    const bool entered = observe_route_ready([&] {
        return admission.entered.load(std::memory_order_acquire);
    });
    bool shutdown_drained = false;
    std::optional<std::thread> shutdown;
    if (entered) {
        shutdown.emplace([&] {
            shutdown_drained = scheduler.shutdown(std::chrono::seconds(5));
        });
    }
    const bool draining = entered && observe_route_ready([&] {
        return elio::runtime::detail::graceful_admission_closed_for_test.load(
            std::memory_order_acquire);
    });
    admission.release.set();
    const bool outcome_observed = draining && observe_route_ready([&] {
        return call.is_ready() ||
            admission.operation_entered.load(std::memory_order_acquire);
    });
    const bool completed_before_recovery = outcome_observed && call.is_ready();
    const bool operation_entered_before_recovery =
        admission.operation_entered.load(std::memory_order_acquire);
    const bool read_staged_before_recovery =
        detail::client_response_read_staged_for_test.load(std::memory_order_acquire);

    std::exception_ptr cleanup_failure;
    try { stop.cancel(); }
    catch (...) { cleanup_failure = std::current_exception(); }
    server.wait_destroyed();
    call.wait_destroyed();
    if (shutdown) shutdown->join();
    else shutdown_drained = scheduler.shutdown(std::chrono::seconds(5));

    std::exception_ptr server_failure;
    try { server.await_resume(); }
    catch (...) { server_failure = std::current_exception(); }
    std::string call_failure;
    try { (void)call.await_resume(); }
    catch (const std::exception& error) { call_failure = error.what(); }
    catch (...) { call_failure = "non-standard exception"; }

    if (cleanup_failure) std::rethrow_exception(cleanup_failure);
    if (server_failure) std::rethrow_exception(server_failure);
    REQUIRE(entered);
    REQUIRE(draining);
    REQUIRE(outcome_observed);
    REQUIRE(completed_before_recovery);
    CHECK_FALSE(operation_entered_before_recovery);
    CHECK_FALSE(read_staged_before_recovery);
    REQUIRE(shutdown_drained);
    CHECK(call_failure == "scheduler rejected joinable task before execution");
    CHECK(observed.handshake);
    REQUIRE(observed.requests.size() == 2);
    CHECK(observed.requests.front().get_method() == method::CONNECT);
    CHECK(observed.requests.back().get_method() == method::GET);
    CHECK(owner->active_operations_for_test() == 0);
}

TEST_CASE("CONNECT and inner TLS retain one absolute TCP budget and settle cancellation",
          "[http][proxy][routes][deadline][issue-1249]") {
    const auto selected = GENERATE(backend::epoll, backend::io_uring);
    for (const bool inner_tls : {false, true})
    for (const bool cancelled : {false, true}) {
        CAPTURE(selected, inner_tls, cancelled);
        backend_guard backend_scope(selected);
        auto listener = elio::net::tcp_listener::bind(elio::net::ipv4_address("127.0.0.1", 0));
        REQUIRE(listener);
        transport_config config;
        config.verify_certificate = false;
        config.proxy.emplace();
        config.proxy->endpoint = "http://127.0.0.1:" + std::to_string(listener->local_address().port());
        config.limits = pool_limits{};
        config.limits->max_live_total = 1;
        config.acquisition_timeout = std::chrono::seconds(40);
        auto owner = std::make_shared<transport>(config);
        client_config policy;
        policy.connect_timeout = std::chrono::seconds(20);
        client agent(owner, policy);
        setup_observation observed;
        setup_hooks hooks(observed);
        observed_route authentication;
        authentication_hooks authentication_scope(authentication);
        elio::coro::cancel_source server_stop;
        elio::coro::cancel_source user_stop;
        elio::runtime::scheduler scheduler(1);
        scheduler.start();
        auto [server, controlled] = launch_fixture_pair(scheduler, server_stop,
            stalled_connect(*listener, inner_tls, observed, server_stop.get_token()),
            [&]() -> task<client_result<response>> {
            output_observation cleanup;
            std::exception_ptr failure;
            auto call = admit_connect_fixture([&] {
                return agent.get_result("https://unresolved-origin.invalid/",
                                        user_stop.get_token());
            }, cleanup, user_stop, failure);
            bool reached = false;
            try {
                if (call) reached = co_await await_connect_fixture_event(
                    observed.connect_read, cleanup, user_stop, failure);
                if (reached) reached = co_await await_connect_fixture_event(
                    observed.route_entered, cleanup, user_stop, failure);
                if (reached && inner_tls) reached = co_await await_connect_fixture_event(
                    observed.tls_entered, cleanup, user_stop, failure);
                if (reached) {
                    if (cancelled) user_stop.cancel();
                    else observed.expire.set();
                }
            } catch (...) {
                if (!failure) failure = std::current_exception();
                settle_connect_fixture(cleanup, user_stop, failure, true);
            }
            if (!failure) CHECK(reached);
            std::optional<client_result<response>> result;
            if (call) {
                auto& joined = *call;
                try { result.emplace(co_await joined); }
                catch (...) {
                    if (!failure) failure = std::current_exception();
                    settle_connect_fixture(cleanup, user_stop, failure, true);
                }
                try { co_await call->wait_destroyed_async(); }
                catch (...) { if (!failure) failure = std::current_exception(); }
            }
            if (failure) std::rethrow_exception(failure);
            co_return std::move(*result);
        });
        controlled.wait_destroyed();
        server_stop.cancel();
        server.wait_destroyed();
        owner->clear();
        scheduler.shutdown();
        auto result = controlled.await_resume();
        server.await_resume();
        const auto* error = std::get_if<client_error>(&result);
        REQUIRE(error);
        REQUIRE(error->code.value() == (cancelled ? ECANCELED : ETIMEDOUT));
        REQUIRE(error->stage == (inner_tls ? client_stage::tls : client_stage::proxy_connect));
        CHECK_FALSE(certificate_rejected(*error, authentication, X509_V_ERR_HOSTNAME_MISMATCH));
        CHECK_FALSE(certificate_rejected(*error, authentication, X509_V_ERR_IP_ADDRESS_MISMATCH));
        if (inner_tls) CHECK(authentication.client_verification == X509_V_OK);
        REQUIRE(observed.tcp_deadline == observed.route_deadline);
        REQUIRE(owner->admission_counters_for_test().live == 0);
        REQUIRE(owner->admission_counters_for_test().dialing == 0);
    }
}

TEST_CASE("CONNECT deadline before its first write is not certificate rejection",
          "[http][proxy][routes][deadline][pre-connect-timeout][issue-1249]") {
    const auto selected = GENERATE(backend::epoll, backend::io_uring);
    const auto cancelled = GENERATE(false, true);
    CAPTURE(selected, cancelled);
    backend_guard backend_scope(selected);
    auto listener = elio::net::tcp_listener::bind(elio::net::ipv4_address("127.0.0.1", 0));
    REQUIRE(listener);
    transport_config config;
    config.proxy.emplace();
    config.proxy->endpoint = "http://127.0.0.1:" + std::to_string(listener->local_address().port());
    config.limits = pool_limits{};
    config.acquisition_timeout = std::chrono::seconds(40);
    auto owner = std::make_shared<transport>(config);
    client_config policy;
    policy.connect_timeout = std::chrono::seconds(20);
    client agent(owner, policy);
    setup_observation observed;
    setup_hooks hooks(observed);
    connect_write_gate gate;
    observed_route route;
    authentication_hooks authentication_scope(route);
    elio::coro::cancel_source server_stop;
    elio::coro::cancel_source user_stop;
    elio::runtime::scheduler scheduler(1);
    scheduler.start();
    auto [server, controlled] = launch_fixture_pair(scheduler, server_stop,
        unread_connect(*listener, observed, route, server_stop.get_token()),
        [&]() -> task<client_result<response>> {
        output_observation cleanup;
        std::exception_ptr failure;
        auto call = admit_connect_fixture([&] {
            return agent.get_result("https://127.0.0.1:9443/", user_stop.get_token());
        }, cleanup, user_stop, failure);
        bool reached = false;
        try {
            if (call) reached = co_await await_connect_fixture_event(
                observed.accepted, cleanup, user_stop, failure);
            if (reached) reached = co_await await_connect_fixture_event(
                observed.route_entered, cleanup, user_stop, failure);
            if (reached) reached = co_await await_connect_fixture_event(
                observed.write_entered, cleanup, user_stop, failure);
            if (reached) {
                if (cancelled) user_stop.cancel();
                else observed.expire.set();
            }
        } catch (...) {
            if (!failure) failure = std::current_exception();
            settle_connect_fixture(cleanup, user_stop, failure, true);
        }
        if (!failure) CHECK(reached);
        std::optional<client_result<response>> result;
        if (call) {
            auto& joined = *call;
            try { result.emplace(co_await joined); }
            catch (...) {
                if (!failure) failure = std::current_exception();
                settle_connect_fixture(cleanup, user_stop, failure, true);
            }
            try { co_await call->wait_destroyed_async(); }
            catch (...) { if (!failure) failure = std::current_exception(); }
        }
        if (failure) std::rethrow_exception(failure);
        co_return std::move(*result);
    });
    controlled.wait_destroyed();
    server_stop.cancel();
    server.wait_destroyed();
    owner->clear();
    scheduler.shutdown();
    auto result = controlled.await_resume();
    server.await_resume();
    const auto* error = std::get_if<client_error>(&result);
    REQUIRE(error);
    CHECK(error->stage == client_stage::proxy_connect);
    CHECK(error->code.value() == (cancelled ? ECANCELED : ETIMEDOUT));
    CHECK(route.accepted == 1);
    CHECK(route.accept_error == 0);
    CHECK(route.connect_read.bytes == 0);
    CHECK_FALSE(route.connect_read.complete);
    CHECK(route.client_connect_writing);
    CHECK(route.client_connect_written == 0);
    CHECK_FALSE(route.client_connect_reading);
    CHECK(route.client_handshake_failures == 0);
    CHECK_FALSE(certificate_rejected(*error, route, X509_V_ERR_IP_ADDRESS_MISMATCH));
    CHECK(observed.tcp_deadline == observed.route_deadline);
    CHECK(route.client_setup_deadline == observed.route_deadline);
    CHECK(owner->active_operations_for_test() == 0);
    CHECK(owner->admission_counters_for_test().live == 0);
}

TEST_CASE("CONNECT origin trust hostname and unsupported ALPN fail before origin requests",
          "[http][proxy][routes][tls-policy][issue-1249]") {
    const auto selected = GENERATE(backend::epoll, backend::io_uring);
    for (const int rejection : {0, 1, 2}) {
        CAPTURE(selected, rejection);
        backend_guard backend_scope(selected);
        elio::tls::tls_context server_context(elio::tls::tls_mode::server);
        temporary_pem ca;
        install_certificate(server_context, ca);
        if (rejection == 2) REQUIRE(server_context.set_alpn_protocols("h2"));
        auto listener = elio::net::tcp_listener::bind(elio::net::ipv4_address("127.0.0.1", 0));
        REQUIRE(listener);
        transport_config config;
        config.proxy.emplace();
        config.proxy->endpoint = "http://127.0.0.1:" + std::to_string(listener->local_address().port());
        config.limits = pool_limits{};
        config.acquisition_timeout = std::chrono::seconds(5);
        if (rejection != 0) config.configure_tls = [&](transport_tls_config& policy) {
            if (!policy.load_verify_locations(ca.path.data()) ||
                (rejection == 2 && !policy.set_alpn_protocols("h2")))
                throw std::runtime_error("proxy rejection policy setup failed");
        };
        auto owner = std::make_shared<transport>(config);
        client agent(owner);
        const auto target = url::parse(rejection == 1 ? "https://wrong-origin.invalid/path" :
                                                      "https://localhost/path");
        REQUIRE(target);
        observed_route observed;
        authentication_hooks authentication_scope(observed);
        elio::coro::cancel_source stop;
        elio::runtime::scheduler scheduler(1);
        scheduler.start();
        auto [server, calls] = launch_fixture_pair(scheduler, stop,
            serve_route(*listener, true, server_context, 1, observed, stop.get_token()),
            send_requests(agent, *target, 1));
        calls.wait_destroyed();
        stop.cancel();
        server.wait_destroyed();
        owner->clear();
        scheduler.shutdown();
        auto results = calls.await_resume();
        server.await_resume();
        REQUIRE(results.size() == 1);
        const auto* error = std::get_if<client_error>(&results[0]);
        REQUIRE(error);
        CAPTURE(error->code.value(), observed.accepted, observed.accept_error,
                observed.connect_read.bytes, observed.connect_read.terminal_error,
                observed.connect_read.complete, observed.server_handshake_error,
                observed.client_handshake_error, observed.client_verification,
                observed.client_connect_writing, observed.client_connect_written,
                observed.client_connect_reading, observed.client_connect_received,
                observed.client_connect_error);
        REQUIRE(error->stage == client_stage::tls);
        if (rejection == 2) {
            REQUIRE(error->code.value() == EPROTONOSUPPORT);
            CHECK(observed.client_handshake_failures == 0);
        } else {
            CHECK(certificate_rejected(*error, observed, rejection == 0 ?
                X509_V_ERR_DEPTH_ZERO_SELF_SIGNED_CERT : X509_V_ERR_HOSTNAME_MISMATCH));
        }
        REQUIRE(observed.requests.size() == 1);
        REQUIRE(observed.requests[0].get_method() == method::CONNECT);
        REQUIRE(owner->admission_counters_for_test().live == 0);
    }
}

TEST_CASE("HTTP Transport forward and CONNECT routes perform real I/O and target-bound reuse",
          "[http][proxy][routes][proxy-endpoint-dns][origin-reference][issue-1249]") {
    elio::tls::tls_context server_context(elio::tls::tls_mode::server);
    temporary_pem ca;
    install_certificate(server_context, ca);
    const auto selected = GENERATE(backend::epoll, backend::io_uring);
    for (const bool finite : {false, true})
    for (const bool secure : {false, true})
    for (const bool streaming : {false, true}) {
        CAPTURE(selected, finite, secure, streaming);
        backend_guard backend_scope(selected);
        auto listener = elio::net::tcp_listener::bind(elio::net::ipv4_address("127.0.0.1", 0));
        REQUIRE(listener);
        transport_config config;
        config.proxy = http_proxy_config{};
        config.proxy->endpoint = "http://%31%32%37.0.0.1:" + std::to_string(listener->local_address().port());
        config.proxy->basic_auth = proxy_basic_credentials{"user", "password"};
        config.acquisition_timeout = std::chrono::seconds(5);
        config.configure_tls = [&](transport_tls_config& policy) {
            if (!policy.load_verify_locations(ca.path.data()))
                throw std::runtime_error("proxy origin trust loading failed");
        };
        if (finite) {
            config.limits = pool_limits{};
            config.limits->max_live_total = 1;
            config.limits->max_live_per_route = 1;
        }
        auto owner = std::make_shared<transport>(config);
        const auto frozen_authorization = detail::proxy_basic_authorization(*config.proxy->basic_auth);
        config.proxy->endpoint = "http://unrelated.invalid";
        config.proxy->basic_auth->password = "changed-secret";
        client_config request_policy;
        request_policy.read_timeout = std::chrono::seconds(5);
        client agent(owner, request_policy);
        const auto target = url::parse(secure ? "https://%4COcALhost:9443/path" :
                                               "http://unresolved-origin.invalid:8081/path");
        REQUIRE(target);
        const auto plan = owner->route_plan_for_test(*target);
        REQUIRE(plan.key().mode == (secure ? detail::route_mode::connect_tunnel :
                                            detail::route_mode::forward_proxy));
        REQUIRE(plan.key().target_dns == detail::route_dns_mode::proxy);
        REQUIRE(plan.key().hops.size() == 1);
        CHECK(plan.key().hops.front().endpoint.host == "127.0.0.1");
        if (secure) CHECK(plan.key().target.host == "%4cocalhost");
        observed_route observed;
        SSL_CTX_set_tlsext_servername_callback(server_context.native_handle(), record_sni);
        SSL_CTX_set_tlsext_servername_arg(server_context.native_handle(), &observed);
        elio::coro::cancel_source stop;
        elio::runtime::scheduler scheduler(1);
        scheduler.start();
        auto [server, calls] = launch_fixture_pair(scheduler, stop,
            serve_route(*listener, secure, server_context, 2, observed, stop.get_token()),
            send_requests(agent, *target, 2, streaming));
        calls.wait_destroyed();
        stop.cancel();
        server.wait_destroyed();
        owner->clear();
        scheduler.shutdown();
        auto results = calls.await_resume();
        server.await_resume();
        const auto* first_error = results.empty() ? nullptr :
            std::get_if<client_error>(&results.front());
        const int first_error_code = first_error ? first_error->code.value() : 0;
        const int first_error_stage = first_error ? static_cast<int>(first_error->stage) : -1;
        CAPTURE(first_error_code, first_error_stage, observed.accepted, observed.accept_error,
                observed.handshake, observed.server_handshake_error, observed.sni,
                observed.requests.size(), observed.connect_read.bytes,
                observed.connect_read.terminal_error, observed.connect_read.complete);
        REQUIRE(results.size() == 2);
        for (const auto& result : results) {
            if (const auto* error = std::get_if<client_error>(&result)) {
                CAPTURE(error->code, error->stage);
                REQUIRE_FALSE(error);
            }
            REQUIRE(std::holds_alternative<response>(result));
            REQUIRE(std::get<response>(result).body() == "ok");
        }
        REQUIRE(observed.accepted == 1);
        REQUIRE(observed.requests.size() == (secure ? 3 : 2));
        if (secure) {
            REQUIRE(observed.handshake);
            REQUIRE(observed.sni == "localhost");
            REQUIRE(observed.requests[0].get_method() == method::CONNECT);
            REQUIRE(observed.requests[0].path() == "%4COcALhost:9443");
            REQUIRE(observed.requests[0].header("Host") == "%4COcALhost:9443");
            REQUIRE(observed.requests[0].header("Proxy-Authorization") == frozen_authorization);
        }
        for (size_t i = 0; i < 2; ++i) {
            const auto& received = observed.requests[i + (secure ? 1 : 0)];
            const auto wanted = secure ? "/path?q=" + std::to_string(i) :
                "http://unresolved-origin.invalid:8081/path?q=" + std::to_string(i);
            REQUIRE(received.path_with_query() == wanted);
            REQUIRE(received.header("Authorization") == "Bearer origin-secret");
            REQUIRE(received.header("Proxy-Authorization") == (secure ? "" : frozen_authorization));
            REQUIRE(received.header("Host") == (secure ? "%4COcALhost:9443" :
                                                 "unresolved-origin.invalid:8081"));
        }
        if (finite) REQUIRE(owner->admission_counters_for_test().live == 0);
    }
}

TEST_CASE("Proxy route fixtures skip local peers that close before sending payload",
          "[http][proxy][routes][fixture-peer][issue-1249]") {
    const auto selected = GENERATE(backend::epoll, backend::io_uring);
    CAPTURE(selected);
    backend_guard backend_scope(selected);
    elio::tls::tls_context server_context(elio::tls::tls_mode::server);
    temporary_pem ca;
    install_certificate(server_context, ca);
    auto listener = elio::net::tcp_listener::bind(
        elio::net::ipv4_address("127.0.0.1", 0));
    REQUIRE(listener);
    empty_connection_probe probe;
    REQUIRE(probe.fd >= 0);
    const auto proxy_address = elio::net::ipv4_address(
        "127.0.0.1", listener->local_address().port()).to_sockaddr();
    REQUIRE(::connect(probe.fd, reinterpret_cast<const sockaddr*>(&proxy_address),
                      sizeof(proxy_address)) == 0);
    REQUIRE(::shutdown(probe.fd, SHUT_WR) == 0);
    transport_config config;
    config.proxy.emplace();
    config.proxy->endpoint = "http://127.0.0.1:" +
        std::to_string(listener->local_address().port());
    config.limits = pool_limits{};
    config.acquisition_timeout = std::chrono::seconds(5);
    config.configure_tls = [&](transport_tls_config& policy) {
        if (!policy.load_verify_locations(ca.path.data()))
            throw std::runtime_error("fixture-peer trust loading failed");
    };
    auto owner = std::make_shared<transport>(config);
    client agent(owner);
    const auto target = url::parse("https://localhost:9443/path");
    REQUIRE(target);
    observed_route observed;
    authentication_hooks authentication_scope(observed);
    elio::coro::cancel_source stop;
    elio::runtime::scheduler scheduler(1);
    scheduler.start();
    auto [server, calls] = launch_fixture_pair(scheduler, stop,
        serve_route(*listener, true, server_context, 1, observed, stop.get_token()),
        send_requests(agent, *target, 1));
    calls.wait_destroyed();
    stop.cancel();
    server.wait_destroyed();
    owner->clear();
    scheduler.shutdown();
    const auto results = calls.await_resume();
    server.await_resume();
    REQUIRE(results.size() == 1);
    if (const auto* error = std::get_if<client_error>(&results.front())) {
        CAPTURE(error->stage, error->code.value(), observed.accepted,
                observed.empty_connections, observed.accept_error,
                observed.connect_read.bytes, observed.connect_read.terminal_error,
                observed.connect_read.complete, observed.requests.size(),
                observed.client_connect_writing, observed.client_connect_written,
                observed.client_connect_reading, observed.client_connect_received,
                observed.client_connect_error);
        REQUIRE_FALSE(error);
    }
    REQUIRE(std::holds_alternative<response>(results.front()));
    CHECK(std::get<response>(results.front()).body() == "ok");
    CHECK(observed.empty_connections == 1);
    CHECK(observed.accepted == 1);
    REQUIRE(observed.requests.size() == 2);
    CHECK(observed.requests.front().get_method() == method::CONNECT);
    CHECK(owner->active_operations_for_test() == 0);
    CHECK(owner->admission_counters_for_test().live == 0);
}

TEST_CASE("CONNECT authenticates numeric origin IP SANs without SNI over IPv4 and IPv6 proxies",
          "[http][proxy][routes][ip-origin][issue-1249]") {
    const auto selected = GENERATE(backend::epoll, backend::io_uring);
    const auto ipv6 = GENERATE(false, true);
    const auto identities = GENERATE("IP:127.0.0.1,IP:::1",
                                     "IP:127.0.0.2,IP:::2",
                                     "DNS:127.0.0.1,DNS:::1");
    const auto finite = GENERATE(false, true);
    const auto streaming = GENERATE(false, true);
    CAPTURE(selected, ipv6, identities, finite, streaming);
    backend_guard backend_scope(selected);
    auto listener = ipv6
        ? elio::net::tcp_listener::bind(elio::net::ipv6_address("::1", 0))
        : elio::net::tcp_listener::bind(elio::net::ipv4_address("127.0.0.1", 0));
    if (ipv6 && !listener && (errno == EAFNOSUPPORT || errno == EPROTONOSUPPORT ||
                              errno == EADDRNOTAVAIL))
        SKIP("IPv6 loopback unavailable on this host");
    REQUIRE(listener);
    elio::tls::tls_context server_context(elio::tls::tls_mode::server);
    temporary_pem ca;
    install_certificate(server_context, ca, identities);
    transport_config config;
    config.proxy.emplace();
    config.proxy->endpoint = std::string(ipv6 ? "http://[::1]:" : "http://127.0.0.1:") +
        std::to_string(listener->local_address().port());
    config.acquisition_timeout = std::chrono::seconds(5);
    config.configure_tls = [&](transport_tls_config& policy) {
        if (!policy.load_verify_locations(ca.path.data()))
            throw std::runtime_error("numeric origin trust loading failed");
    };
    if (finite) config.limits = pool_limits{};
    auto owner = std::make_shared<transport>(config);
    client_config request_policy;
    request_policy.read_timeout = std::chrono::seconds(5);
    client agent(owner, request_policy);
    const auto target = url::parse(ipv6 ? "https://[0:0:0:0:0:0:0:1]:9443/path" :
                                          "https://127.0.0.1:9443/path");
    REQUIRE(target);
    CHECK(owner->route_plan_for_test(*target).key().target.host == (ipv6 ? "::1" : "127.0.0.1"));
    observed_route observed;
    authentication_hooks authentication_scope(observed);
    SSL_CTX_set_tlsext_servername_callback(server_context.native_handle(), record_sni);
    SSL_CTX_set_tlsext_servername_arg(server_context.native_handle(), &observed);
    elio::coro::cancel_source stop;
    elio::runtime::scheduler scheduler(1);
    scheduler.start();
    auto [server, calls] = launch_fixture_pair(scheduler, stop,
        serve_route(*listener, true, server_context, 1, observed, stop.get_token()),
        send_requests(agent, *target, 1, streaming));
    calls.wait_destroyed();
    stop.cancel();
    server.wait_destroyed();
    owner->clear();
    scheduler.shutdown();
    const auto results = calls.await_resume();
    server.await_resume();
    CAPTURE(observed.accepted, observed.accept_error, observed.connect_read.bytes,
            observed.connect_read.terminal_error, observed.connect_read.complete,
            observed.server_handshake_error, observed.client_handshake_error,
            observed.client_verification, observed.client_connect_writing,
            observed.client_connect_written, observed.client_connect_reading,
            observed.client_connect_received, observed.client_connect_error);
    REQUIRE(results.size() == 1);
    const bool authentic = std::string_view(identities) == "IP:127.0.0.1,IP:::1";
    if (authentic) {
        REQUIRE(std::holds_alternative<response>(results[0]));
        CHECK(std::get<response>(results[0]).body() == "ok");
        CHECK(observed.handshake);
        REQUIRE(observed.requests.size() == 2);
        CHECK(observed.requests[1].path_with_query() == "/path?q=0");
        CHECK(observed.requests[1].header("Host") == target->host_authority());
    } else {
        const auto* error = std::get_if<client_error>(&results[0]);
        REQUIRE(error);
        CAPTURE(error->code.value());
        CHECK(error->stage == client_stage::tls);
        CHECK(certificate_rejected(*error, observed, X509_V_ERR_IP_ADDRESS_MISMATCH));
        CHECK_FALSE(observed.handshake);
        REQUIRE(observed.requests.size() == 1);
    }
    CHECK(observed.accepted == 1);
    CHECK(observed.sni.empty());
    CHECK(observed.requests[0].get_method() == method::CONNECT);
    CHECK(observed.requests[0].path() == target->host_authority());
    CHECK(observed.requests[0].header("Host") == target->host_authority());
    CHECK(owner->active_operations_for_test() == 0);
    if (finite) CHECK(owner->admission_counters_for_test().live == 0);
}

namespace {
struct direct_output_observation {
    output_observation output;
    int fd = -1;
    std::atomic<bool> client_tls_created{false};
    std::atomic<bool> client_tls_ready{false};
    std::atomic<bool> client_tls_failed{false};
    std::atomic<bool> server_accept_entered{false};
    std::atomic<bool> server_accepted{false};
    std::atomic<size_t> server_empty_connections{0};
    std::atomic<bool> server_handshake_entered{false};
    std::atomic<bool> server_handshake_finished{false};
    std::atomic<bool> server_handshake_ok{false};
    std::atomic<int> server_handshake_error{0};
    std::atomic<bool> server_request_entered{false};
    std::atomic<bool> server_request_received{false};
    elio::sync::event handshake_failed;
    std::atomic<int> handshake_error{0};
    std::atomic<long> verification{X509_V_OK};
};

struct observe_root_close {
    int fd;
    bool& destroyed;
    bool& closed;
    ~observe_root_close() {
        destroyed = true;
        closed = ::fcntl(fd, F_GETFD) < 0 && errno == EBADF;
    }
};

#if ELIO_HAS_IO_URING
struct deferred_close_submission {
    bool previous = elio::io::detail::defer_destructor_close_submission_for_test.exchange(true);
    ~deferred_close_submission() {
        elio::io::detail::defer_destructor_close_submission_for_test.store(previous);
    }
};
#endif

} // namespace

TEST_CASE("Owned Transport roots close before releasing their accounting owner",
          "[http][tls][retirement][issue-1272][root-close][http_client_streaming]") {
    const auto selected = GENERATE(backend::epoll, backend::io_uring);
    const auto direct = GENERATE(false, true);
    backend_guard backend_selection(selected);
#if ELIO_HAS_IO_URING
    deferred_close_submission defer_close;
#endif
    elio::tls::tls_context context(elio::tls::tls_mode::client);
    bool created = false;
    bool destroyed = false;
    bool closed = false;
    auto retire = [&]() -> task<void> {
        const int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
        if (fd < 0) co_return;
        created = true;
        auto owner = std::make_shared<observe_root_close>(fd, destroyed, closed);
        if (direct) {
            elio::tls::tls_stream stream(elio::net::tcp_stream(fd), context);
            (void)elio::tls::detail::tls_retirement_access::bind(stream, owner);
            owner.reset();
        } else {
            detail::owned_prefix_stream<elio::net::tcp_stream> stream(
                elio::net::tcp_stream(fd), {}, 8192, owner);
            owner.reset();
        }
    };
    elio::runtime::scheduler scheduler(1);
    scheduler.start();
    auto retired = scheduler.go_joinable(retire());
    retired.wait_destroyed();
    retired.await_resume();
    REQUIRE(scheduler.shutdown(std::chrono::seconds(10)));
    CHECK(created);
    CHECK(destroyed);
    CHECK(closed);
}

TEST_CASE("Settled root close survives moves and exclusive setup failure",
          "[http][tls][retirement][issue-1272][root-close][http_client_streaming]") {
    const auto selected = GENERATE(backend::epoll, backend::io_uring);
    const auto phase = GENERATE(0, 1, 2);
    backend_guard backend_selection(selected);
#if ELIO_HAS_IO_URING
    deferred_close_submission defer_close;
#endif
    bool created = false;
    bool destroyed = false;
    bool closed = false;
    bool setup_failed = false;
    bool linger_disabled = false;
    auto retire = [&]() -> task<void> {
        const int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
        if (fd < 0) co_return;
        created = true;
        auto owner = std::make_shared<observe_root_close>(fd, destroyed, closed);
        try {
            elio::net::tcp_stream original(fd);
            elio::net::detail::fail_root_linger_configuration_for_test.store(phase == 1);
            elio::net::detail::tcp_retirement_access::mark_settled_root(original);
            ::linger option{};
            socklen_t length = sizeof(option);
            linger_disabled = ::getsockopt(fd, SOL_SOCKET, SO_LINGER, &option, &length) == 0 &&
                option.l_onoff == 0;
            if (phase == 2) {
                detail::owned_prefix_stream<elio::net::tcp_stream> invalid(
                    std::move(original), {'x'}, 0, owner);
            } else {
                elio::net::tcp_stream moved(std::move(original));
                elio::net::tcp_stream assigned(-1);
                assigned = std::move(moved);
            }
        } catch (const std::system_error& error) {
            setup_failed = error.code().value() == ENOMEM;
        } catch (const std::invalid_argument&) {
            setup_failed = true;
        }
        elio::net::detail::fail_root_linger_configuration_for_test.store(false);
        owner.reset();
    };
    elio::runtime::scheduler scheduler(1);
    scheduler.start();
    auto retired = scheduler.go_joinable(retire());
    retired.wait_destroyed();
    retired.await_resume();
    REQUIRE(scheduler.shutdown(std::chrono::seconds(10)));
    CHECK(created);
    CHECK(destroyed);
    CHECK(closed);
    CHECK(setup_failed == (phase != 0));
    if (phase != 1) CHECK(linger_disabled);
}

TEST_CASE("TLS root retirement notifies its preallocated observer without wake allocation",
          "[http][tls][retirement][issue-1272][http_client_streaming]") {
    const auto phase = GENERATE(0, 1, 2);
    auto retirement = std::make_shared<elio::tls::detail::tls_root_retirement>();
    const auto released = retirement->released;
    static_assert(noexcept(released->set()));
    const auto allocations = elio::sync::detail::wake_state_allocations_for_test.load();
    if (phase == 2) {
        auto wait = released->wait();
        CHECK_FALSE(wait.await_ready());
        retirement.reset();
        CHECK_FALSE(wait.await_suspend(std::noop_coroutine()));
        wait.await_resume();
    } else {
        if (phase == 0) retirement.reset();
        size_t completed = 0;
        auto observe = [&]() -> task<void> {
            co_await released->wait();
            ++completed;
        };
        auto waiting = observe();
        auto frame = elio::coro::detail::task_access::handle(waiting);
        frame.resume();
        const bool suspended = !frame.done();
        elio::sync::detail::fail_next_wake_state_allocation_for_test.store(true);
        retirement.reset();
        const bool allocation_unused =
            elio::sync::detail::fail_next_wake_state_allocation_for_test.exchange(false);
        REQUIRE(frame.done());
        waiting.await_resume();
        CHECK(completed == 1);
        CHECK(suspended == (phase == 1));
        CHECK(allocation_unused);
    }
    CHECK(elio::sync::detail::wake_state_allocations_for_test.load() == allocations);
}

namespace {

std::atomic<direct_output_observation*> direct_output_observed{nullptr};

struct direct_phase_diagnostics {
    bool captured = false;
    bool client_tls_created = false;
    bool client_tls_ready = false;
    bool client_tls_failed = false;
    int client_handshake_error = 0;
    long client_verification = X509_V_OK;
    bool server_accept_entered = false;
    bool server_accepted = false;
    size_t server_empty_connections = 0;
    bool server_handshake_entered = false;
    bool server_handshake_finished = false;
    bool server_handshake_ok = false;
    int server_handshake_error = 0;
    bool server_request_entered = false;
    bool server_request_received = false;
};

void capture_direct_phases(void* context) noexcept {
    auto& phases = *static_cast<direct_phase_diagnostics*>(context);
    auto* observed = direct_output_observed.load(std::memory_order_acquire);
    if (!observed) return;
    phases.captured = true;
    phases.client_tls_created = observed->client_tls_created.load(std::memory_order_acquire);
    phases.client_tls_ready = observed->client_tls_ready.load(std::memory_order_acquire);
    phases.client_tls_failed = observed->client_tls_failed.load(std::memory_order_acquire);
    phases.client_handshake_error = observed->handshake_error.load(std::memory_order_acquire);
    phases.client_verification = observed->verification.load(std::memory_order_acquire);
    phases.server_accept_entered = observed->server_accept_entered.load(std::memory_order_acquire);
    phases.server_accepted = observed->server_accepted.load(std::memory_order_acquire);
    phases.server_empty_connections =
        observed->server_empty_connections.load(std::memory_order_acquire);
    phases.server_handshake_entered =
        observed->server_handshake_entered.load(std::memory_order_acquire);
    phases.server_handshake_finished =
        observed->server_handshake_finished.load(std::memory_order_acquire);
    phases.server_handshake_ok = observed->server_handshake_ok.load(std::memory_order_acquire);
    phases.server_handshake_error =
        observed->server_handshake_error.load(std::memory_order_acquire);
    phases.server_request_entered = observed->server_request_entered.load(std::memory_order_acquire);
    phases.server_request_received =
        observed->server_request_received.load(std::memory_order_acquire);
}

ssize_t defer_direct_output_send(void*, int, const void*, size_t, int) {
    errno = EAGAIN;
    return -1;
}

void observe_direct_created(elio::tls::tls_stream&) {
    direct_output_observed.load(std::memory_order_acquire)->client_tls_created.store(
        true, std::memory_order_release);
}

void install_direct_output_hooks(elio::tls::tls_stream& stream) {
    auto& observed = *direct_output_observed.load();
    observed.fd = stream.fd();
    observed.output.handshake_bytes = stream.finish_state_for_test().accepted_ciphertext;
    stream.set_output_test_hooks({nullptr, defer_direct_output_send, nullptr});
    stream.set_output_progress_test_hook(&observed.output,
        observed.output.hold_inactive ? nullptr : pause_drained_output,
        observed.output.hold_inactive ? pause_drained_output : nullptr);
}

void observe_direct_created_with_output(elio::tls::tls_stream& stream) {
    observe_direct_created(stream);
    install_direct_output_hooks(stream);
}

void observe_direct_output(elio::tls::tls_stream& stream) {
    auto& observed = *direct_output_observed.load();
    observed.client_tls_created.store(true, std::memory_order_release);
    observed.client_tls_ready.store(true, std::memory_order_release);
    install_direct_output_hooks(stream);
}

void observe_direct_failure(elio::tls::tls_stream& stream, int error) {
    auto& observed = *direct_output_observed.load();
    observed.client_tls_failed.store(true, std::memory_order_release);
    observed.handshake_error.store(error, std::memory_order_release);
    observed.verification.store(stream.verify_result(), std::memory_order_release);
    observed.handshake_failed.set();
}

struct direct_output_hooks {
    explicit direct_output_hooks(direct_output_observation& observed, bool before_handshake = false) {
        direct_output_observed.store(&observed);
        if (before_handshake) {
            detail::client_tls_created_for_test.store(observe_direct_created_with_output);
            detail::client_tls_failed_for_test.store(observe_direct_failure);
        } else {
            detail::client_tls_created_for_test.store(observe_direct_created);
            detail::client_tls_ready_for_test.store(observe_direct_output);
            detail::client_tls_failed_for_test.store(observe_direct_failure);
        }
    }
    ~direct_output_hooks() {
        detail::client_tls_ready_for_test.store(nullptr);
        detail::client_tls_created_for_test.store(nullptr);
        detail::client_tls_failed_for_test.store(nullptr);
        direct_output_observed.store(nullptr);
    }
};

struct completed_setup_watchdog_observation {
    direct_output_observation direct;
    elio::sync::event watchdog_failed;
    elio::sync::event cleanup_entered;
    elio::coro::cancel_source watchdog_recovery;
    int queued_plaintext = 0;
};

std::atomic<completed_setup_watchdog_observation*>
    completed_setup_watchdog_observed{nullptr};

void observe_completed_tls_setup(elio::tls::tls_stream& stream) {
    auto& observed = *completed_setup_watchdog_observed.load(
        std::memory_order_acquire);
    install_direct_output_hooks(stream);
    constexpr char marker = 'x';
    observed.queued_plaintext =
        elio::tls::detail::tls_idle_access::queue_plaintext_for_test(
            stream, &marker, sizeof(marker));
}

void observe_completed_setup_cleanup() {
    completed_setup_watchdog_observed.load(
        std::memory_order_acquire)->cleanup_entered.set();
}

task<elio::coro::cancel_result> fail_completed_setup_watchdog(
        std::chrono::steady_clock::time_point, elio::coro::cancel_token) {
    auto& observed = *completed_setup_watchdog_observed.load(
        std::memory_order_acquire);
    if (co_await observed.direct.output.paused.wait(
            observed.watchdog_recovery.get_token()) !=
            elio::coro::cancel_result::completed)
        co_return elio::coro::cancel_result::cancelled;
    observed.watchdog_failed.set();
    throw std::runtime_error("injected completed setup watchdog failure");
    co_return elio::coro::cancel_result::completed;
}

void recover_completed_setup_watchdog(void* context) noexcept {
    try {
        static_cast<completed_setup_watchdog_observation*>(context)->
            watchdog_recovery.cancel();
    } catch (...) {}
}

struct completed_setup_watchdog_hooks {
    detail::setup_watchdog_wait_hook previous_wait;
    void (*previous_connected)(elio::tls::tls_stream&);
    void (*previous_failed)(elio::tls::tls_stream&, int);
    void (*previous_cleanup)();
    direct_output_observation* previous_output;
    completed_setup_watchdog_observation* previous_observation;

    explicit completed_setup_watchdog_hooks(
            completed_setup_watchdog_observation& observed)
        : previous_wait(detail::setup_watchdog_wait_for_test.exchange(
              fail_completed_setup_watchdog))
        , previous_connected(detail::client_tls_connected_for_test.exchange(
              observe_completed_tls_setup))
        , previous_failed(detail::client_tls_failed_for_test.exchange(
              observe_direct_failure))
        , previous_cleanup(detail::setup_tls_cleanup_entered_for_test.exchange(
              observe_completed_setup_cleanup))
        , previous_output(direct_output_observed.exchange(&observed.direct))
        , previous_observation(completed_setup_watchdog_observed.exchange(&observed)) {}

    ~completed_setup_watchdog_hooks() {
        completed_setup_watchdog_observed.store(previous_observation);
        direct_output_observed.store(previous_output);
        detail::setup_tls_cleanup_entered_for_test.store(previous_cleanup);
        detail::client_tls_failed_for_test.store(previous_failed);
        detail::client_tls_connected_for_test.store(previous_connected);
        detail::setup_watchdog_wait_for_test.store(previous_wait);
    }
};

struct direct_retry_observation {
    direct_output_observation first;
    uint16_t first_port = 0;
    uint16_t second_port = 0;
    std::atomic<size_t> dns_calls{0};
    std::atomic<size_t> dials{0};
    std::atomic<size_t> tls_sessions{0};
    std::atomic<bool> root_released{false};
    std::atomic<bool> first_closed_at_release{false};
    std::atomic<bool> second_dial_after_release{false};
    elio::sync::event waiting_for_root;
    elio::sync::event second_dial;
};

std::atomic<direct_retry_observation*> direct_retry_observed{nullptr};

elio::net::detail::dns_lookup_result resolve_direct_retry(
        std::string_view, uint16_t) {
    auto& observed = *direct_retry_observed.load(std::memory_order_acquire);
    observed.dns_calls.fetch_add(1, std::memory_order_relaxed);
    elio::net::detail::dns_lookup_result result;
    result.cacheable = false;
    result.addresses.emplace_back(elio::net::ipv4_address("127.0.0.1", observed.first_port));
    result.addresses.emplace_back(elio::net::ipv4_address("127.0.0.1", observed.second_port));
    return result;
}

void observe_direct_retry(
        detail::direct_tls_retry_step step, size_t) {
    auto& observed = *direct_retry_observed.load(std::memory_order_acquire);
    if (step == detail::direct_tls_retry_step::dialing) {
        const auto dial = observed.dials.fetch_add(1, std::memory_order_acq_rel);
        if (dial == 1) {
            observed.second_dial_after_release.store(
                observed.root_released.load(std::memory_order_acquire) &&
                observed.first_closed_at_release.load(std::memory_order_acquire),
                std::memory_order_release);
            observed.second_dial.set();
        }
    } else if (step == detail::direct_tls_retry_step::waiting_for_root) {
        observed.waiting_for_root.set();
    } else {
        errno = 0;
        observed.first_closed_at_release.store(
            ::fcntl(observed.first.fd, F_GETFD) < 0 && errno == EBADF,
            std::memory_order_release);
        observed.root_released.store(true, std::memory_order_release);
    }
}

void observe_direct_retry_created(elio::tls::tls_stream& stream) {
    auto& observed = *direct_retry_observed.load(std::memory_order_acquire);
    const auto session = observed.tls_sessions.fetch_add(1, std::memory_order_acq_rel);
    if (session == 0) {
        observed.first.client_tls_created.store(true, std::memory_order_release);
        install_direct_output_hooks(stream);
    }
}

void observe_direct_retry_failure(elio::tls::tls_stream& stream, int error) {
    auto& observed = *direct_retry_observed.load(std::memory_order_acquire);
    if (observed.tls_sessions.load(std::memory_order_acquire) == 1)
        observe_direct_failure(stream, error);
}

struct direct_retry_hooks {
    explicit direct_retry_hooks(direct_retry_observation& observed) {
        direct_retry_observed.store(&observed, std::memory_order_release);
        direct_output_observed.store(&observed.first, std::memory_order_release);
        elio::net::detail::owned_dns_lookup_for_test.store(
            resolve_direct_retry, std::memory_order_release);
        detail::direct_tls_retry_for_test.store(observe_direct_retry, std::memory_order_release);
        detail::client_tls_created_for_test.store(
            observe_direct_retry_created, std::memory_order_release);
        detail::client_tls_failed_for_test.store(
            observe_direct_retry_failure, std::memory_order_release);
    }
    ~direct_retry_hooks() {
        detail::client_tls_failed_for_test.store(nullptr, std::memory_order_release);
        detail::client_tls_created_for_test.store(nullptr, std::memory_order_release);
        detail::direct_tls_retry_for_test.store(nullptr, std::memory_order_release);
        elio::net::detail::owned_dns_lookup_for_test.store(nullptr, std::memory_order_release);
        direct_output_observed.store(nullptr, std::memory_order_release);
        direct_retry_observed.store(nullptr, std::memory_order_release);
    }
};

void finish_fixture_output(output_observation& output, elio::coro::cancel_source& stop,
        std::exception_ptr& failure, bool cancel = false) noexcept {
    try { output.release.set(); }
    catch (...) { if (!failure) failure = std::current_exception(); }
    if (cancel || failure) {
        try { stop.cancel(); }
        catch (...) { if (!failure) failure = std::current_exception(); }
    }
}

template<typename T>
task<bool> await_fixture_ready(elio::coro::join_handle<T>& joined,
        output_observation& output, elio::coro::cancel_source& stop,
        std::exception_ptr& failure, void* recovery_context = nullptr,
        void (*before_recovery)(void*) noexcept = nullptr) {
    // Bound diagnostic waits as well as public I/O. An early operational error
    // may never reach the retained-output hook; cancellation still joins every
    // frame, and releasing the hook also unblocks terminal output settlement.
    try {
        const auto outcome = co_await joined.wait_until(
            std::chrono::steady_clock::now() + std::chrono::seconds(10));
        if (outcome == elio::coro::join_wait_outcome::completed) co_return true;
    } catch (...) { if (!failure) failure = std::current_exception(); }
    if (before_recovery) before_recovery(recovery_context);
    finish_fixture_output(output, stop, failure, true);
    co_return false;
}

template<typename T>
task<T> await_fixture_result(elio::coro::join_handle<T>& joined,
        output_observation& output, elio::coro::cancel_source& stop,
        std::exception_ptr& failure, void* recovery_context = nullptr,
        void (*before_recovery)(void*) noexcept = nullptr) {
    std::optional<T> result;
    try { result.emplace(co_await joined); }
    catch (...) {
        if (!failure) failure = std::current_exception();
        if (before_recovery) before_recovery(recovery_context);
        finish_fixture_output(output, stop, failure, true);
    }
    try { co_await joined.wait_destroyed_async(); }
    catch (...) {
        if (!failure) failure = std::current_exception();
        if (before_recovery) before_recovery(recovery_context);
        finish_fixture_output(output, stop, failure, true);
    }
    // The caller rethrows the preserved exception only after all siblings join.
    co_return result ? std::move(*result) : T{};
}

task<bool> await_fixture_phase(elio::sync::event& phase,
        output_observation& output, elio::coro::cancel_source& stop,
        std::exception_ptr& failure, void* recovery_context = nullptr,
        void (*before_recovery)(void*) noexcept = nullptr) {
    auto* scheduler = elio::runtime::scheduler::current();
    std::optional<elio::coro::join_handle<elio::coro::cancel_result>> waiting;
    try {
        waiting.emplace(scheduler->go_joinable([&]() -> task<elio::coro::cancel_result> {
            co_return co_await phase.wait(stop.get_token());
        }));
    } catch (...) {
        if (!failure) failure = std::current_exception();
        if (before_recovery) before_recovery(recovery_context);
        finish_fixture_output(output, stop, failure, true);
        co_return false;
    }
    bool reached = false;
    auto result = elio::coro::cancel_result::cancelled;
    try {
        reached = co_await await_fixture_ready(*waiting, output, stop, failure,
            recovery_context, before_recovery);
        result = co_await await_fixture_result(*waiting, output, stop, failure,
            recovery_context, before_recovery);
    } catch (...) {
        if (!failure) failure = std::current_exception();
        if (before_recovery) before_recovery(recovery_context);
        finish_fixture_output(output, stop, failure, true);
    }
    try { co_await waiting->wait_destroyed_async(); }
    catch (...) {
        if (!failure) failure = std::current_exception();
        if (before_recovery) before_recovery(recovery_context);
        finish_fixture_output(output, stop, failure, true);
    }
    co_return reached && result == elio::coro::cancel_result::completed;
}

task<void> serve_direct_output(elio::net::tcp_listener& listener,
        elio::tls::tls_context& context, direct_output_observation& observed,
        elio::coro::cancel_token token) {
    observed.server_accept_entered.store(true, std::memory_order_release);
    size_t empty_connections = 0;
    auto accepted = co_await accept_fixture_payload(
        listener, token, &empty_connections);
    observed.server_empty_connections.store(
        empty_connections, std::memory_order_release);
    if (!accepted) co_return;
    observed.server_accepted.store(true, std::memory_order_release);
    elio::tls::tls_stream stream(std::move(*accepted), context);
    observed.server_handshake_entered.store(true, std::memory_order_release);
    const bool handshake_ok = co_await stream.handshake(token);
    const int handshake_error = handshake_ok ? 0 : errno;
    observed.server_handshake_error.store(handshake_error, std::memory_order_release);
    observed.server_handshake_ok.store(handshake_ok, std::memory_order_release);
    observed.server_handshake_finished.store(true, std::memory_order_release);
    if (handshake_ok) {
        observed.server_request_entered.store(true, std::memory_order_release);
        if (co_await receive_request(stream, token)) {
            observed.server_request_received.store(true, std::memory_order_release);
            (void)co_await stream.write_exactly(
                "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok", token);
            std::array<char, 1> bytes{};
            (void)co_await stream.read(bytes.data(), bytes.size(), token);
        }
    }
    co_await stream.abort_and_settle();
}

task<elio::coro::cancel_result> throw_fixture_observer(
        std::chrono::steady_clock::time_point, elio::coro::cancel_token) {
    throw std::runtime_error("injected retirement observer failure");
    co_return elio::coro::cancel_result::cancelled;
}

struct fixture_observer_hook {
    elio::coro::detail::join_timer_wait_hook previous;
    explicit fixture_observer_hook(bool enabled)
        : previous(elio::coro::detail::join_timer_wait_for_test.exchange(
              enabled ? throw_fixture_observer : nullptr)) {}
    ~fixture_observer_hook() {
        elio::coro::detail::join_timer_wait_for_test.store(previous);
    }
};

struct fixture_frame_probe {
    bool& destroyed;
    ~fixture_frame_probe() { destroyed = true; }
};

void count_fixture_recovery(void* context) noexcept {
    ++*static_cast<size_t*>(context);
}
} // namespace

TEST_CASE("Retirement fixtures release and join held frames before reporting exceptions",
          "[http][tls][retirement][issue-1272][fixture-exception][http_client_streaming]") {
    const auto selected = GENERATE(backend::epoll, backend::io_uring);
    const auto observer_failure = GENERATE(false, true);
    const auto workers = GENERATE(size_t{1}, size_t{2});
    backend_guard backend_scope(selected);
    fixture_observer_hook hook(observer_failure);
    output_observation output;
    elio::sync::event missing_phase;
    bool held_completed = false;
    bool held_destroyed = false;
    bool throwing_destroyed = observer_failure;
    bool cancelled = false;
    size_t recovery_calls = 0;
    std::exception_ptr failure;
    elio::runtime::scheduler scheduler(workers);
    scheduler.start();
    auto controlled = scheduler.go_joinable([&]() -> task<void> {
        elio::coro::cancel_source stop;
        auto held = scheduler.go_joinable([&]() -> task<void> {
            fixture_frame_probe frame{held_destroyed};
            co_await output.release.wait();
            held_completed = true;
        });
        if (observer_failure) {
            (void)co_await await_fixture_phase(missing_phase, output, stop, failure,
                &recovery_calls, count_fixture_recovery);
        } else {
            auto throwing = scheduler.go_joinable([&]() -> task<int> {
                fixture_frame_probe frame{throwing_destroyed};
                throw std::runtime_error("injected retirement request failure");
                co_return 0;
            });
            (void)co_await await_fixture_result(throwing, output, stop, failure,
                &recovery_calls, count_fixture_recovery);
        }
        cancelled = stop.is_cancelled();
        co_await held;
        co_await held.wait_destroyed_async();
    });
    controlled.wait_destroyed();
    controlled.await_resume();
    REQUIRE(scheduler.shutdown(std::chrono::seconds(10)));
    CHECK(held_completed);
    CHECK(held_destroyed);
    CHECK(throwing_destroyed);
    CHECK(cancelled);
    CHECK(recovery_calls > 0);
    REQUIRE(failure);
    try { std::rethrow_exception(failure); }
    catch (const std::runtime_error& error) {
        CHECK(std::string_view(error.what()) == (observer_failure
            ? "injected retirement observer failure" : "injected retirement request failure"));
    }
}

TEST_CASE("Direct HTTPS Transport retains root accounting through late owned output frames",
          "[http][tls][retirement][issue-1272][http_client_streaming]") {
    const auto selected = GENERATE(backend::epoll, backend::io_uring);
    const auto finite = GENERATE(false, true);
    const auto inactive = GENERATE(false, true);
    const auto workers = GENERATE(size_t{1}, size_t{2});
    CAPTURE(selected, finite, inactive, workers);
    backend_guard backend_scope(selected);
    auto listener = elio::net::tcp_listener::bind(elio::net::ipv4_address("127.0.0.1", 0));
    REQUIRE(listener);
    empty_connection_probe probe;
    REQUIRE(probe.fd >= 0);
    const auto direct_address = elio::net::ipv4_address(
        "127.0.0.1", listener->local_address().port()).to_sockaddr();
    REQUIRE(::connect(probe.fd, reinterpret_cast<const sockaddr*>(&direct_address),
                      sizeof(direct_address)) == 0);
    REQUIRE(::shutdown(probe.fd, SHUT_WR) == 0);
    elio::tls::tls_context server_context(elio::tls::tls_mode::server);
    temporary_pem ca;
    install_certificate(server_context, ca);
    transport_config config;
    config.acquisition_timeout = std::chrono::seconds(5);
    config.configure_tls = [&](transport_tls_config& policy) {
        if (!policy.load_verify_locations(ca.path.data()))
            throw std::runtime_error("direct retirement trust loading failed");
    };
    if (finite) config.limits = pool_limits{};
    auto owner = std::make_shared<transport>(config);
    client agent(owner);
    const auto target = "https://localhost:" + std::to_string(listener->local_address().port()) + "/path";
    direct_output_observation observed;
    observed.output.hold_inactive = inactive;
    direct_output_hooks hooks(observed);
    elio::coro::cancel_source stop;
    elio::runtime::scheduler scheduler(workers);
    scheduler.start();
    auto server = scheduler.go_joinable(
        serve_direct_output(*listener, server_context, observed, stop.get_token()));
    struct retirement_snapshot {
        size_t operations = 0;
        size_t idle = 0;
        size_t live = 0;
        bool fd_open = false;
        bool shutdown_ready = false;
        bool fd_closed = false;
        bool pause_reached = false;
        bool request_completed = false;
        direct_phase_diagnostics phases;
        std::exception_ptr failure;
        elio::coro::cancel_result shutdown_result = elio::coro::cancel_result::cancelled;
    } snapshot;
    auto controlled = scheduler.go_joinable([&]() -> task<client_result<response>> {
        elio::coro::cancel_source call_stop;
        std::optional<elio::coro::join_handle<client_result<response>>> call;
        std::optional<elio::coro::join_handle<elio::coro::cancel_result>> shutdown;
        client_result<response> result;
        elio::sync::event entered;
        try {
            call.emplace(scheduler.go_joinable(agent.get_result(target, call_stop.get_token())));
            snapshot.pause_reached = co_await await_fixture_phase(
                observed.output.paused, observed.output, call_stop, snapshot.failure,
                &snapshot.phases, capture_direct_phases);
            snapshot.request_completed = co_await await_fixture_ready(
                *call, observed.output, call_stop, snapshot.failure);
            result = co_await await_fixture_result(*call, observed.output, call_stop, snapshot.failure);
            snapshot.operations = owner->active_operations_for_test();
            if (finite) snapshot.idle = owner->admission_counters_for_test().idle;
            shutdown.emplace(scheduler.go_joinable([&]() -> task<elio::coro::cancel_result> {
                entered.set();
                co_return co_await owner->shutdown();
            }));
            (void)co_await await_fixture_phase(entered, observed.output, call_stop, snapshot.failure);
            snapshot.fd_open = ::fcntl(observed.fd, F_GETFD) >= 0;
            snapshot.shutdown_ready = shutdown->is_ready();
            if (finite) snapshot.live = owner->admission_counters_for_test().live;
            finish_fixture_output(observed.output, call_stop, snapshot.failure);
            snapshot.shutdown_result = co_await await_fixture_result(
                *shutdown, observed.output, call_stop, snapshot.failure);
        } catch (...) {
            if (!snapshot.failure) snapshot.failure = std::current_exception();
            finish_fixture_output(observed.output, call_stop, snapshot.failure, true);
        }
        if (!snapshot.phases.captured) capture_direct_phases(&snapshot.phases);
        if (call) co_await call->wait_destroyed_async();
        if (shutdown) co_await shutdown->wait_destroyed_async();
        snapshot.fd_closed = ::fcntl(observed.fd, F_GETFD) == -1;
        co_return result;
    });
    controlled.wait_destroyed();
    stop.cancel();
    server.wait_destroyed();
    REQUIRE(scheduler.shutdown(std::chrono::seconds(10)));
    const auto result = controlled.await_resume();
    server.await_resume();
    if (snapshot.failure) std::rethrow_exception(snapshot.failure);
    CAPTURE(snapshot.phases.client_tls_created, snapshot.phases.client_tls_ready,
        snapshot.phases.client_tls_failed, snapshot.phases.client_handshake_error,
        snapshot.phases.client_verification, snapshot.phases.server_accept_entered,
        snapshot.phases.server_accepted, snapshot.phases.server_empty_connections,
        snapshot.phases.server_handshake_entered,
        snapshot.phases.server_handshake_finished, snapshot.phases.server_handshake_ok,
        snapshot.phases.server_handshake_error, snapshot.phases.server_request_entered,
        snapshot.phases.server_request_received);
    if (const auto* error = std::get_if<client_error>(&result)) {
        CAPTURE(error->code.value(), error->stage);
        CHECK_FALSE(error);
    }
    CHECK(snapshot.pause_reached);
    CHECK(snapshot.request_completed);
    CHECK(snapshot.phases.server_empty_connections == 1);
    CHECK(snapshot.operations == 1);
    if (finite) CHECK(snapshot.idle == 0);
    CHECK(snapshot.fd_open);
    CHECK_FALSE(snapshot.shutdown_ready);
    if (finite) CHECK(snapshot.live == 1);
    CHECK(snapshot.shutdown_result == elio::coro::cancel_result::completed);
    CHECK(snapshot.fd_closed);
    REQUIRE(std::holds_alternative<response>(result));
    CHECK(std::get<response>(result).body() == "ok");
    CHECK(owner->active_operations_for_test() == 0);
    if (finite) CHECK(owner->admission_counters_for_test().live == 0);
}

TEST_CASE("Failed direct TLS setup retains accounting until its late output root is destroyed",
          "[http][tls][retirement][setup-failure][issue-1272][http_client_streaming]") {
    const auto selected = GENERATE(backend::epoll, backend::io_uring);
    const auto finite = GENERATE(false, true);
    const auto workers = GENERATE(size_t{1}, size_t{2});
    CAPTURE(selected, finite, workers);
    backend_guard backend_scope(selected);
    auto listener = elio::net::tcp_listener::bind(elio::net::ipv4_address("127.0.0.1", 0));
    REQUIRE(listener);
    elio::tls::tls_context server_context(elio::tls::tls_mode::server);
    temporary_pem ca;
    install_certificate(server_context, ca);
    transport_config config;
    config.configure_tls = [&](transport_tls_config& policy) {
        if (!policy.load_verify_locations(ca.path.data()))
            throw std::runtime_error("failed direct setup trust loading failed");
    };
    if (finite) config.limits = pool_limits{};
    auto owner = std::make_shared<transport>(config);
    client_config policy;
    policy.connect_timeout = std::chrono::seconds::zero();
    client agent(owner, policy);
    const auto target = "https://127.0.0.1:" +
        std::to_string(listener->local_address().port()) + "/path";
    direct_output_observation observed;
    observed.output.hold_inactive = true;
    direct_output_hooks hooks(observed, true);
    elio::coro::cancel_source stop;
    elio::runtime::scheduler scheduler(workers);
    scheduler.start();
    auto server = scheduler.go_joinable(
        serve_direct_output(*listener, server_context, observed, stop.get_token()));
    struct failure_snapshot {
        size_t operations = 0;
        size_t live = 0;
        bool fd_open = false;
        bool fd_closed = false;
        bool request_ready = false;
        bool pause_reached = false;
        bool failure_observed = false;
        std::exception_ptr failure;
        elio::coro::cancel_result shutdown_result = elio::coro::cancel_result::cancelled;
    } snapshot;
    auto controlled = scheduler.go_joinable([&]() -> task<client_result<response>> {
        elio::coro::cancel_source call_stop;
        std::optional<elio::coro::join_handle<client_result<response>>> call;
        std::optional<elio::coro::join_handle<elio::coro::cancel_result>> shutdown;
        client_result<response> result;
        try {
            call.emplace(scheduler.go_joinable(agent.get_result(target, call_stop.get_token())));
            snapshot.pause_reached = co_await await_fixture_phase(
                observed.output.paused, observed.output, call_stop, snapshot.failure);
            snapshot.failure_observed = co_await await_fixture_phase(
                observed.handshake_failed, observed.output, call_stop, snapshot.failure);
            snapshot.operations = owner->active_operations_for_test();
            if (finite) snapshot.live = owner->admission_counters_for_test().live;
            snapshot.fd_open = ::fcntl(observed.fd, F_GETFD) >= 0;
            snapshot.request_ready = call->is_ready();
            shutdown.emplace(scheduler.go_joinable(owner->shutdown()));
            finish_fixture_output(observed.output, call_stop, snapshot.failure);
            result = co_await await_fixture_result(*call, observed.output, call_stop, snapshot.failure);
            snapshot.shutdown_result = co_await await_fixture_result(
                *shutdown, observed.output, call_stop, snapshot.failure);
        } catch (...) {
            if (!snapshot.failure) snapshot.failure = std::current_exception();
            finish_fixture_output(observed.output, call_stop, snapshot.failure, true);
        }
        if (call) co_await call->wait_destroyed_async();
        if (shutdown) co_await shutdown->wait_destroyed_async();
        snapshot.fd_closed = ::fcntl(observed.fd, F_GETFD) == -1;
        co_return result;
    });
    controlled.wait_destroyed();
    stop.cancel();
    server.wait_destroyed();
    REQUIRE(scheduler.shutdown(std::chrono::seconds(10)));
    const auto result = controlled.await_resume();
    server.await_resume();
    if (snapshot.failure) std::rethrow_exception(snapshot.failure);
    CHECK(snapshot.pause_reached);
    CHECK(snapshot.failure_observed);
    CHECK(snapshot.operations == 1);
    if (finite) CHECK(snapshot.live == 1);
    CHECK(snapshot.fd_open);
    CHECK_FALSE(snapshot.request_ready);
    CHECK(snapshot.shutdown_result == elio::coro::cancel_result::completed);
    CHECK(snapshot.fd_closed);
    const auto* error = std::get_if<client_error>(&result);
    REQUIRE(error);
    CHECK(error->stage == client_stage::tls);
    CHECK(error->code.value() > 0);
    CHECK(error->code.value() != ETIMEDOUT);
    CHECK(error->code.value() != ECANCELED);
    CHECK(observed.verification == X509_V_ERR_IP_ADDRESS_MISMATCH);
    CHECK(observed.handshake_error > 0);
    CHECK(owner->active_operations_for_test() == 0);
    if (finite) CHECK(owner->admission_counters_for_test().live == 0);
}

TEST_CASE("Completed setup watchdog fixture recovers after an early direct TLS failure",
          "[http][tls][retirement][fixture-recovery][issue-1272][http_client_streaming]") {
    const auto selected = GENERATE(backend::epoll, backend::io_uring);
    const auto version = GENERATE(elio::tls::tls_version::tls_1_2,
                                  elio::tls::tls_version::tls_1_3);
    CAPTURE(selected, version);
    backend_guard backend_scope(selected);
    auto listener = elio::net::tcp_listener::bind(
        elio::net::ipv4_address("127.0.0.1", 0));
    REQUIRE(listener);
    elio::tls::tls_context server_context(elio::tls::tls_mode::server, version);
    temporary_pem ca;
    install_certificate(server_context, ca, "DNS:localhost");
    transport_config config;
    config.limits = pool_limits{};
    config.limits->max_live_total = 1;
    config.limits->max_live_per_route = 1;
    config.configure_tls = [&](transport_tls_config& policy) {
        if (!policy.load_verify_locations(ca.path.data()))
            throw std::runtime_error("watchdog recovery trust loading failed");
    };
    auto owner = std::make_shared<transport>(config);
    client_config policy;
    policy.connect_timeout = std::chrono::seconds(30);
    client agent(owner, policy);
    const auto target = "https://127.0.0.1:" +
        std::to_string(listener->local_address().port()) + "/path";
    completed_setup_watchdog_observation observed;
    completed_setup_watchdog_hooks hooks(observed);
    elio::coro::cancel_source stop;
    elio::runtime::scheduler scheduler(2);
    scheduler.start();
    auto server = scheduler.go_joinable(serve_direct_output(
        *listener, server_context, observed.direct, stop.get_token()));
    struct recovery_snapshot {
        bool handshake_failed = false;
        bool request_ready_before_recovery = true;
        bool recovery_sent = false;
        std::exception_ptr failure;
        elio::coro::cancel_result shutdown_result =
            elio::coro::cancel_result::cancelled;
    } snapshot;
    auto controlled = scheduler.go_joinable([&]() -> task<client_result<response>> {
        elio::coro::cancel_source fixture_stop;
        std::optional<elio::coro::join_handle<client_result<response>>> call;
        client_result<response> result;
        try {
            call.emplace(scheduler.go_joinable(
                agent.get_result(target, fixture_stop.get_token())));
            snapshot.handshake_failed = co_await await_fixture_phase(
                observed.direct.handshake_failed, observed.direct.output,
                fixture_stop, snapshot.failure, &observed,
                recover_completed_setup_watchdog);
            snapshot.request_ready_before_recovery = call->is_ready();
            recover_completed_setup_watchdog(&observed);
            snapshot.recovery_sent = true;
            result = co_await await_fixture_result(
                *call, observed.direct.output, fixture_stop, snapshot.failure);
            snapshot.shutdown_result = co_await owner->shutdown();
        } catch (...) {
            if (!snapshot.failure) snapshot.failure = std::current_exception();
            recover_completed_setup_watchdog(&observed);
            finish_fixture_output(
                observed.direct.output, fixture_stop, snapshot.failure, true);
        }
        if (call) co_await call->wait_destroyed_async();
        co_return result;
    });
    controlled.wait_destroyed();
    stop.cancel();
    server.wait_destroyed();
    REQUIRE(scheduler.shutdown(std::chrono::seconds(10)));
    const auto result = controlled.await_resume();
    server.await_resume();
    if (snapshot.failure) std::rethrow_exception(snapshot.failure);
    CHECK(snapshot.handshake_failed);
    CHECK_FALSE(snapshot.request_ready_before_recovery);
    CHECK(snapshot.recovery_sent);
    CHECK_FALSE(observed.watchdog_failed.is_set());
    CHECK(observed.direct.client_tls_failed.load(std::memory_order_acquire));
    CHECK(observed.direct.verification.load(std::memory_order_acquire) ==
          X509_V_ERR_IP_ADDRESS_MISMATCH);
    const auto* error = std::get_if<client_error>(&result);
    REQUIRE(error);
    CHECK(error->stage == client_stage::tls);
    CHECK(error->code.value() > 0);
    CHECK(snapshot.shutdown_result == elio::coro::cancel_result::completed);
    CHECK(owner->active_operations_for_test() == 0);
    CHECK(owner->admission_counters_for_test().live == 0);
}

TEST_CASE("Failed setup watchdog settles an already-connected direct TLS root",
          "[http][tls][retirement][setup-watchdog-failure][issue-1272][http_client_streaming]") {
    const auto selected = GENERATE(backend::epoll, backend::io_uring);
    const auto version = GENERATE(elio::tls::tls_version::tls_1_2,
                                  elio::tls::tls_version::tls_1_3);
    const auto inactive = GENERATE(false, true);
    CAPTURE(selected, version, inactive);
    backend_guard backend_scope(selected);
    auto listener = elio::net::tcp_listener::bind(
        elio::net::ipv4_address("127.0.0.1", 0));
    REQUIRE(listener);
    elio::tls::tls_context server_context(elio::tls::tls_mode::server, version);
    temporary_pem ca;
    install_certificate(server_context, ca, "DNS:localhost");
    transport_config config;
    config.limits = pool_limits{};
    config.limits->max_live_total = 1;
    config.limits->max_live_per_route = 1;
    config.configure_tls = [&](transport_tls_config& policy) {
        if (!policy.load_verify_locations(ca.path.data()))
            throw std::runtime_error("completed setup watchdog trust loading failed");
    };
    auto owner = std::make_shared<transport>(config);
    client_config policy;
    policy.connect_timeout = std::chrono::seconds(30);
    client agent(owner, policy);
    const auto target = "https://localhost:" +
        std::to_string(listener->local_address().port()) + "/path";
    completed_setup_watchdog_observation observed;
    observed.direct.output.hold_inactive = inactive;
    completed_setup_watchdog_hooks hooks(observed);
    elio::coro::cancel_source stop;
    elio::runtime::scheduler scheduler(2);
    scheduler.start();
    auto server = scheduler.go_joinable(serve_direct_output(
        *listener, server_context, observed.direct, stop.get_token()));
    struct failure_snapshot {
        size_t operations = 0;
        size_t live = 0;
        bool output_paused = false;
        bool watchdog_failed = false;
        bool cleanup_entered = false;
        bool fd_open = false;
        bool fd_closed = false;
        bool request_ready = false;
        bool shutdown_ready = false;
        bool request_returned = false;
        bool watchdog_exception = false;
        size_t operations_after_exception = 1;
        size_t live_after_exception = 1;
        bool fd_closed_on_exception = false;
        std::exception_ptr failure;
        elio::coro::cancel_result shutdown_result =
            elio::coro::cancel_result::cancelled;
    } snapshot;
    auto controlled = scheduler.go_joinable([&]() -> task<void> {
        elio::coro::cancel_source fixture_stop;
        std::optional<elio::coro::join_handle<client_result<response>>> call;
        std::optional<elio::coro::join_handle<elio::coro::cancel_result>> shutdown;
        try {
            call.emplace(scheduler.go_joinable(
                agent.get_result(target, fixture_stop.get_token())));
            snapshot.output_paused = co_await await_fixture_phase(
                observed.direct.output.paused, observed.direct.output,
                fixture_stop, snapshot.failure, &observed,
                recover_completed_setup_watchdog);
            snapshot.watchdog_failed = snapshot.output_paused &&
                co_await await_fixture_phase(
                    observed.watchdog_failed, observed.direct.output,
                    fixture_stop, snapshot.failure, &observed,
                    recover_completed_setup_watchdog);
            snapshot.cleanup_entered = snapshot.watchdog_failed &&
                co_await await_fixture_phase(
                    observed.cleanup_entered, observed.direct.output,
                    fixture_stop, snapshot.failure, &observed,
                    recover_completed_setup_watchdog);
            snapshot.operations = owner->active_operations_for_test();
            snapshot.live = owner->admission_counters_for_test().live;
            snapshot.fd_open = ::fcntl(observed.direct.fd, F_GETFD) >= 0;
            snapshot.request_ready = call->is_ready();
            shutdown.emplace(scheduler.go_joinable(owner->shutdown()));
            snapshot.shutdown_ready = shutdown->is_ready();
            finish_fixture_output(
                observed.direct.output, fixture_stop, snapshot.failure);
            try {
                auto& joined = *call;
                (void)co_await joined;
                snapshot.request_returned = true;
            } catch (const std::runtime_error& error) {
                snapshot.watchdog_exception = std::string_view(error.what()) ==
                    "injected completed setup watchdog failure";
                if (!snapshot.watchdog_exception)
                    snapshot.failure = std::current_exception();
            } catch (...) {
                snapshot.failure = std::current_exception();
            }
            snapshot.operations_after_exception = owner->active_operations_for_test();
            snapshot.live_after_exception = owner->admission_counters_for_test().live;
            snapshot.fd_closed_on_exception =
                ::fcntl(observed.direct.fd, F_GETFD) == -1;
            try { co_await call->wait_destroyed_async(); }
            catch (...) { if (!snapshot.failure) snapshot.failure = std::current_exception(); }
            snapshot.shutdown_result = co_await await_fixture_result(
                *shutdown, observed.direct.output, fixture_stop, snapshot.failure);
        } catch (...) {
            if (!snapshot.failure) snapshot.failure = std::current_exception();
            recover_completed_setup_watchdog(&observed);
            finish_fixture_output(
                observed.direct.output, fixture_stop, snapshot.failure, true);
        }
        if (call) co_await call->wait_destroyed_async();
        if (shutdown) co_await shutdown->wait_destroyed_async();
        snapshot.fd_closed = ::fcntl(observed.direct.fd, F_GETFD) == -1;
    });
    controlled.wait_destroyed();
    stop.cancel();
    server.wait_destroyed();
    REQUIRE(scheduler.shutdown(std::chrono::seconds(10)));
    controlled.await_resume();
    server.await_resume();
    if (snapshot.failure) std::rethrow_exception(snapshot.failure);
    CHECK(snapshot.output_paused);
    CHECK(observed.queued_plaintext == 1);
    CHECK(snapshot.watchdog_failed);
    CHECK(snapshot.cleanup_entered);
    CHECK(snapshot.operations == 1);
    CHECK(snapshot.live == 1);
    CHECK(snapshot.fd_open);
    CHECK_FALSE(snapshot.request_ready);
    CHECK_FALSE(snapshot.shutdown_ready);
    CHECK_FALSE(snapshot.request_returned);
    CHECK(snapshot.watchdog_exception);
    CHECK(snapshot.operations_after_exception == 0);
    CHECK(snapshot.live_after_exception == 0);
    CHECK(snapshot.fd_closed_on_exception);
    CHECK(snapshot.shutdown_result == elio::coro::cancel_result::completed);
    CHECK(snapshot.fd_closed);
    CHECK(observed.direct.server_accepted.load(std::memory_order_acquire));
    CHECK(owner->active_operations_for_test() == 0);
    CHECK(owner->admission_counters_for_test().live == 0);
}

TEST_CASE("Direct TLS retry waits for the failed physical root to close",
          "[http][tls][retirement][retry][issue-1272][http_client_streaming]") {
    const auto selected = GENERATE(backend::epoll, backend::io_uring);
    CAPTURE(selected);
    backend_guard backend_scope(selected);
    auto first_listener = elio::net::tcp_listener::bind(
        elio::net::ipv4_address("127.0.0.1", 0));
    auto second_listener = elio::net::tcp_listener::bind(
        elio::net::ipv4_address("127.0.0.1", 0));
    REQUIRE(first_listener);
    REQUIRE(second_listener);
    elio::tls::tls_context first_context(elio::tls::tls_mode::server);
    elio::tls::tls_context second_context(elio::tls::tls_mode::server);
    temporary_pem first_ca;
    temporary_pem second_ca;
    install_certificate(first_context, first_ca, "DNS:not-retry.elio.test");
    install_certificate(second_context, second_ca, "DNS:retry.elio.test");
    transport_config config;
    config.resolve_options.use_cache = false;
    config.rotate_resolved_addresses = false;
    config.limits = pool_limits{};
    config.configure_tls = [&](transport_tls_config& policy) {
        if (!policy.load_verify_locations(first_ca.path.data()) ||
            !policy.load_verify_locations(second_ca.path.data()))
            throw std::runtime_error("direct retry trust loading failed");
    };
    auto owner = std::make_shared<transport>(config);
    client_config policy;
    policy.connect_timeout = std::chrono::seconds::zero();
    client agent(owner, policy);
    direct_retry_observation observed;
    observed.first_port = first_listener->local_address().port();
    observed.second_port = second_listener->local_address().port();
    observed.first.output.hold_inactive = true;
    direct_output_observation second_observed;
    direct_retry_hooks hooks(observed);
    const std::string target = "https://retry.elio.test:" +
        std::to_string(observed.first_port) + "/path";
    elio::coro::cancel_source stop;
    elio::runtime::scheduler scheduler(2);
    scheduler.start();
    auto first_server = scheduler.go_joinable(serve_direct_output(
        *first_listener, first_context, observed.first, stop.get_token()));
    auto second_server = scheduler.go_joinable(serve_direct_output(
        *second_listener, second_context, second_observed, stop.get_token()));
    struct retry_snapshot {
        bool pause_reached = false;
        bool failure_observed = false;
        bool root_wait_observed = false;
        bool second_dial_observed = false;
        bool root_released_before_barrier = false;
        bool first_open_before_barrier = false;
        bool request_ready_before_barrier = false;
        size_t dials_before_barrier = 0;
        size_t tls_sessions_before_barrier = 0;
        bool request_completed_after_second_dial = false;
        std::exception_ptr failure;
        elio::coro::cancel_result shutdown_result = elio::coro::cancel_result::cancelled;
    } snapshot;
    auto controlled = scheduler.go_joinable([&]() -> task<client_result<response>> {
        elio::coro::cancel_source call_stop;
        std::optional<elio::coro::join_handle<client_result<response>>> call;
        client_result<response> result;
        try {
            call.emplace(scheduler.go_joinable(agent.get_result(target, call_stop.get_token())));
            snapshot.pause_reached = co_await await_fixture_phase(
                observed.first.output.paused, observed.first.output, call_stop, snapshot.failure);
            snapshot.failure_observed = co_await await_fixture_phase(
                observed.first.handshake_failed, observed.first.output,
                call_stop, snapshot.failure);
            snapshot.root_wait_observed = co_await await_fixture_phase(
                observed.waiting_for_root, observed.first.output, call_stop, snapshot.failure);
            snapshot.dials_before_barrier = observed.dials.load(std::memory_order_acquire);
            snapshot.tls_sessions_before_barrier =
                observed.tls_sessions.load(std::memory_order_acquire);
            snapshot.root_released_before_barrier =
                observed.root_released.load(std::memory_order_acquire);
            snapshot.first_open_before_barrier = ::fcntl(observed.first.fd, F_GETFD) >= 0;
            snapshot.request_ready_before_barrier = call->is_ready();
            finish_fixture_output(observed.first.output, call_stop, snapshot.failure);
            snapshot.second_dial_observed = co_await await_fixture_phase(
                observed.second_dial, observed.first.output, call_stop, snapshot.failure);
            snapshot.request_completed_after_second_dial = co_await await_fixture_ready(
                *call, observed.first.output, call_stop, snapshot.failure);
            result = co_await await_fixture_result(
                *call, observed.first.output, call_stop, snapshot.failure);
            snapshot.shutdown_result = co_await owner->shutdown();
        } catch (...) {
            if (!snapshot.failure) snapshot.failure = std::current_exception();
            finish_fixture_output(observed.first.output, call_stop, snapshot.failure, true);
        }
        if (call) co_await call->wait_destroyed_async();
        co_return result;
    });
    controlled.wait_destroyed();
    stop.cancel();
    first_server.wait_destroyed();
    second_server.wait_destroyed();
    REQUIRE(scheduler.shutdown(std::chrono::seconds(10)));
    const auto result = controlled.await_resume();
    first_server.await_resume();
    second_server.await_resume();
    if (snapshot.failure) std::rethrow_exception(snapshot.failure);
    CHECK(snapshot.pause_reached);
    CHECK(snapshot.failure_observed);
    CHECK(snapshot.root_wait_observed);
    CHECK(snapshot.dials_before_barrier == 1);
    CHECK(snapshot.tls_sessions_before_barrier == 1);
    CHECK_FALSE(snapshot.root_released_before_barrier);
    CHECK(snapshot.first_open_before_barrier);
    CHECK_FALSE(snapshot.request_ready_before_barrier);
    CHECK(snapshot.second_dial_observed);
    CHECK(snapshot.request_completed_after_second_dial);
    CHECK(observed.dns_calls.load(std::memory_order_acquire) == 1);
    CHECK(observed.dials.load(std::memory_order_acquire) == 2);
    CHECK(observed.tls_sessions.load(std::memory_order_acquire) == 2);
    CHECK(observed.root_released.load(std::memory_order_acquire));
    CHECK(observed.first_closed_at_release.load(std::memory_order_acquire));
    CHECK(observed.second_dial_after_release.load(std::memory_order_acquire));
    CHECK(snapshot.shutdown_result == elio::coro::cancel_result::completed);
    CHECK(observed.first.verification == X509_V_ERR_HOSTNAME_MISMATCH);
    CHECK(observed.first.handshake_error > 0);
    CHECK_FALSE(observed.first.server_handshake_ok.load(std::memory_order_acquire));
    CHECK(second_observed.server_handshake_ok.load(std::memory_order_acquire));
    REQUIRE(std::holds_alternative<response>(result));
    CHECK(std::get<response>(result).body() == "ok");
    CHECK(owner->active_operations_for_test() == 0);
    CHECK(owner->admission_counters_for_test().live == 0);
}

TEST_CASE("CONNECT retirement holds physical capacity until the owned lower frame releases",
          "[http][proxy][routes][capacity][issue-1249]") {
    using namespace elio::http::detail;
    pool_limits limits;
    limits.max_live_total = 1;
    bounded_pool<route_connection> pool(limits, std::chrono::seconds(60));
    connection_key key;
    key.target = route_endpoint::from("localhost", 443);
    auto acquired = pool.acquire(key, {}, {});
    auto frame = elio::coro::detail::task_access::handle(acquired);
    frame.resume();
    REQUIRE(frame.done());
    auto admitted = acquired.await_resume();
    REQUIRE(std::holds_alternative<bounded_pool<route_connection>::grant>(admitted));
    auto granted = std::move(std::get<bounded_pool<route_connection>::grant>(admitted));
    granted.capacity.dial_complete();
    std::array<int, 2> descriptors{};
    REQUIRE(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC,
                         0, descriptors.data()) == 0);
    elio::net::tcp_stream peer(descriptors[1]);
    auto anchor = std::make_shared<route_retirement>();
    elio::tls::tls_context context(elio::tls::tls_mode::client);
    std::array<char, 1> bytes{};
    {
        connect_channel lower(elio::net::tcp_stream{descriptors[0]}, {}, 0, anchor);
        // A lazy lower operation already owns the state, modeling a pump's
        // retained borrowed-I/O frame without timer scheduling or sleeps.
        auto pending = lower.read(bytes.data(), bytes.size(), {});
        route_connection stream(connect_tls_stream(std::move(lower), context), anchor);
        REQUIRE(pool.retain(granted.capacity, stream).retained);
        { auto retired = pool.clear(); }
        anchor.reset();
        REQUIRE(::fcntl(descriptors[0], F_GETFD) >= 0);
        REQUIRE(pool.counters_for_test().live == 1);
    }
    REQUIRE(::fcntl(descriptors[0], F_GETFD) == -1);
    REQUIRE(pool.counters_for_test().live == 0);
}
