#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <elio/coro/detail/completion_waiter.hpp>
#include <elio/http/http_client.hpp>
#include <elio/coro/join_wait.hpp>
#include <elio/runtime/affinity.hpp>

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
#include <stdexcept>
#include <string>
#include <thread>
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
    auto accepted = co_await listener.accept(token);
    if (!accepted) {
        observed.accept_error = errno;
        co_return;
    }
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
    void set() noexcept {
        released_.store(true, std::memory_order_release);
        auto wake = slot_.take();
        if (auto handle = wake.claim()) elio::runtime::schedule_handle(handle);
    }
private:
    std::atomic<bool> released_{false};
    elio::coro::detail::completion_waiter_slot slot_;
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
    explicit handoff_hooks(handoff_observation& value) {
        handoff_observed.store(&value);
        detail::route_channel_for_test.store(handoff_channel);
        detail::lease_after_publish_for_test.store(pause_published_return);
    }
    ~handoff_hooks() {
        detail::route_channel_for_test.store(nullptr);
        detail::lease_after_publish_for_test.store(nullptr);
        handoff_observed.store(nullptr);
    }
};

struct output_observation {
    uint64_t handshake_bytes = 0;
    bool hold_inactive = false;
    std::atomic<bool> held{false};
    elio::sync::event paused;
    elio::sync::event release;
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
    try { output.release.set(); }
    catch (...) { if (!failure) failure = std::current_exception(); }
    if (cancel || failure) {
        try { stop.cancel(); }
        catch (...) { if (!failure) failure = std::current_exception(); }
    }
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

template<typename T>
task<T> join_connect_fixture(elio::coro::join_handle<T>& joined,
        output_observation& output, elio::coro::cancel_source& stop,
        std::exception_ptr& failure) {
    std::optional<T> result;
    try { result.emplace(co_await joined); }
    catch (...) {
        if (!failure) failure = std::current_exception();
        settle_connect_fixture(output, stop, failure, true);
    }
    try { co_await joined.wait_destroyed_async(); }
    catch (...) {
        if (!failure) failure = std::current_exception();
        settle_connect_fixture(output, stop, failure, true);
    }
    co_return result ? std::move(*result) : T{};
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
        returned_thread returning{observed, std::thread([&] { transport::return_lease_for_test(first); })};
        observed.published.wait();
        auto second_result = handoff_immediate(owner->acquire_lease_for_test(*target));
        returning.join();
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
        output_hooks hooks(output);
        observed_route observed;
        elio::coro::cancel_source stop;
        elio::runtime::scheduler scheduler(workers);
        scheduler.start();
        auto server = scheduler.go_joinable(serve_route(*listener, true, server_context,
                                                       1, observed, stop.get_token()));
        auto controlled = scheduler.go_joinable([&]() -> task<client_result<response>> {
            elio::coro::cancel_source call_stop;
            std::exception_ptr failure;
            auto call = scheduler.go_joinable(agent.get_result(
                "https://localhost/path", call_stop.get_token()));
            const bool paused = co_await await_connect_fixture_phase(output, call_stop, failure);
            auto result = co_await join_connect_fixture(call, output, call_stop, failure);
            CHECK(paused);
            if (paused && !failure) {
                CHECK(owner->active_operations_for_test() == 1);
                if (finite) {
                    CHECK(owner->admission_counters_for_test().live == 1);
                    CHECK(owner->admission_counters_for_test().idle == 0);
                }
            }
            elio::sync::event shutdown_entered;
            auto shutdown = scheduler.go_joinable([&]() -> task<elio::coro::cancel_result> {
                shutdown_entered.set();
                co_return co_await owner->shutdown();
            });
            co_await shutdown_entered.wait();
            if (paused && !failure) CHECK_FALSE(shutdown.is_ready());
            settle_connect_fixture(output, call_stop, failure);
            CHECK(co_await join_connect_fixture(shutdown, output, call_stop, failure) ==
                  elio::coro::cancel_result::completed);
            CHECK(owner->active_operations_for_test() == 0);
            if (finite) CHECK(owner->admission_counters_for_test().live == 0);
            if (failure) std::rethrow_exception(failure);
            co_return result;
        });
        controlled.wait_destroyed();
        stop.cancel();
        server.wait_destroyed();
        scheduler.shutdown();
        auto result = controlled.await_resume();
        server.await_resume();
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
        auto call = scheduler.go_joinable(agent.get_result(
            "https://[not-ip]/path", stop.get_token()));
        paused = co_await await_connect_fixture_phase(
            output, stop, failure, std::chrono::milliseconds(10));
        auto result = co_await join_connect_fixture(call, output, stop, failure);
        (void)co_await owner->shutdown();
        if (failure) std::rethrow_exception(failure);
        co_return result;
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
        auto server = scheduler.go_joinable(reject_connect(*listener, observed, stop.get_token()));
        auto calls = scheduler.go_joinable(request_targets(agent, targets));
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
        auto server = scheduler.go_joinable(isolate_targets(*listener, secure, server_context,
                                                           observed, stop.get_token()));
        auto calls = scheduler.go_joinable(request_targets(agent, targets));
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
        auto server = scheduler.go_joinable(redirect_route(*listener, server_context,
                                                          observed, stop.get_token()));
        auto calls = scheduler.go_joinable(send_requests(agent, *target, 1));
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
        auto server = scheduler.go_joinable(stalled_connect(*listener, inner_tls, observed,
                                                            server_stop.get_token()));
        auto controlled = scheduler.go_joinable([&]() -> task<client_result<response>> {
            output_observation cleanup;
            std::exception_ptr failure;
            auto call = scheduler.go_joinable(agent.get_result("https://unresolved-origin.invalid/",
                                                               user_stop.get_token()));
            bool reached = co_await await_connect_fixture_event(
                observed.connect_read, cleanup, user_stop, failure);
            if (reached) reached = co_await await_connect_fixture_event(
                observed.route_entered, cleanup, user_stop, failure);
            if (reached && inner_tls) reached = co_await await_connect_fixture_event(
                observed.tls_entered, cleanup, user_stop, failure);
            CHECK(reached);
            if (reached) {
                if (cancelled) user_stop.cancel();
                else observed.expire.set();
            }
            auto result = co_await join_connect_fixture(call, cleanup, user_stop, failure);
            if (failure) std::rethrow_exception(failure);
            co_return result;
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
    auto server = scheduler.go_joinable(unread_connect(*listener, observed, route,
                                                      server_stop.get_token()));
    auto controlled = scheduler.go_joinable([&]() -> task<client_result<response>> {
        output_observation cleanup;
        std::exception_ptr failure;
        auto call = scheduler.go_joinable(agent.get_result("https://127.0.0.1:9443/",
                                                           user_stop.get_token()));
        bool reached = co_await await_connect_fixture_event(
            observed.accepted, cleanup, user_stop, failure);
        if (reached) reached = co_await await_connect_fixture_event(
            observed.route_entered, cleanup, user_stop, failure);
        if (reached) reached = co_await await_connect_fixture_event(
            observed.write_entered, cleanup, user_stop, failure);
        CHECK(reached);
        if (reached) {
            if (cancelled) user_stop.cancel();
            else observed.expire.set();
        }
        auto result = co_await join_connect_fixture(call, cleanup, user_stop, failure);
        if (failure) std::rethrow_exception(failure);
        co_return result;
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
        auto server = scheduler.go_joinable(serve_route(*listener, true, server_context,
                                                       1, observed, stop.get_token()));
        auto calls = scheduler.go_joinable(send_requests(agent, *target, 1));
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
        auto server = scheduler.go_joinable(serve_route(*listener, secure, server_context,
                                                       2, observed, stop.get_token()));
        auto calls = scheduler.go_joinable(send_requests(agent, *target, 2, streaming));
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
    auto server = scheduler.go_joinable(serve_route(*listener, true, server_context,
                                                   1, observed, stop.get_token()));
    auto calls = scheduler.go_joinable(send_requests(agent, *target, 1, streaming));
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
