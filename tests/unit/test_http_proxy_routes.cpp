#include <catch2/catch_test_macros.hpp>
#include <elio/http/http_client.hpp>

#include <openssl/pem.h>
#include <openssl/rsa.h>
#include <openssl/x509v3.h>

#include <array>
#include <cstdio>
#include <exception>
#include <fcntl.h>
#include <memory>
#include <latch>
#include <stdexcept>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

using namespace elio::http;
using elio::coro::task;

namespace {

using backend = elio::io::io_context::backend_type;

struct backend_guard {
    backend previous;
    explicit backend_guard(backend selected)
        : previous(elio::runtime::detail::worker_io_backend_for_test.exchange(selected)) {}
    ~backend_guard() { elio::runtime::detail::worker_io_backend_for_test.store(previous); }
};

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

void install_certificate(elio::tls::tls_context& context, temporary_pem& ca) {
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
                          const_cast<char*>("DNS:localhost")), X509_EXTENSION_free);
    if (!san || X509_add_ext(certificate.get(), san.get(), -1) != 1 ||
        X509_sign(certificate.get(), key.get(), EVP_sha256()) <= 0 ||
        SSL_CTX_use_certificate(context.native_handle(), certificate.get()) != 1 ||
        SSL_CTX_use_PrivateKey(context.native_handle(), key.get()) != 1 ||
        SSL_CTX_check_private_key(context.native_handle()) != 1 ||
        PEM_write_X509(ca.file, certificate.get()) != 1 || ::fflush(ca.file) != 0)
        throw std::runtime_error("proxy certificate publication failed");
}

template<typename Stream>
task<std::optional<request>> receive_request(Stream& stream, elio::coro::cancel_token token) {
    request_parser parser;
    std::array<char, 4096> bytes{};
    while (!parser.is_complete()) {
        const auto received = co_await stream.read(bytes.data(), bytes.size(), token);
        if (received.result <= 0) co_return std::nullopt;
        const auto [parsed, consumed] = parser.parse(
            std::string_view(bytes.data(), static_cast<size_t>(received.result)));
        (void)consumed;
        if (parsed == parse_result::error) throw std::runtime_error("proxy fixture request parse failed");
    }
    co_return request::from_parser(parser);
}

struct observed_route {
    size_t accepted = 0;
    std::vector<request> requests;
    std::string sni;
    bool handshake = false;
};

task<void> serve_route(elio::net::tcp_listener& listener, bool secure,
        elio::tls::tls_context& tls_context, size_t count, observed_route& observed,
        elio::coro::cancel_token token) {
    auto accepted = co_await listener.accept(token);
    if (!accepted) co_return;
    ++observed.accepted;
    auto stream = std::move(*accepted);
    if (secure) {
        auto setup = co_await receive_request(stream, token);
        if (!setup) co_return;
        observed.requests.push_back(std::move(*setup));
        constexpr std::string_view connected =
            "HTTP/1.1 200 Tunnel\r\nContent-Length: ignored\r\nTransfer-Encoding: ignored\r\n\r\n";
        if ((co_await stream.write_exactly(connected, token)).result <= 0) co_return;
        elio::tls::tls_stream origin(std::move(stream), tls_context);
        observed.handshake = co_await origin.handshake(token);
        if (!observed.handshake) co_return;
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

TEST_CASE("CONNECT 407 is bounded and never pools or automatically replays a rejected channel",
          "[http][proxy][routes][auth][issue-1249]") {
    for (const auto selected : {backend::epoll, backend::io_uring}) {
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
    for (const auto selected : {backend::epoll, backend::io_uring})
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

TEST_CASE("CONNECT and inner TLS retain one absolute TCP budget and settle cancellation",
          "[http][proxy][routes][deadline][issue-1249]") {
    for (const auto selected : {backend::epoll, backend::io_uring})
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
        elio::coro::cancel_source server_stop;
        elio::coro::cancel_source user_stop;
        elio::runtime::scheduler scheduler(1);
        scheduler.start();
        auto server = scheduler.go_joinable(stalled_connect(*listener, inner_tls, observed,
                                                            server_stop.get_token()));
        auto controlled = scheduler.go_joinable([&]() -> task<client_result<response>> {
            auto call = scheduler.go_joinable(agent.get_result("https://unresolved-origin.invalid/",
                                                               user_stop.get_token()));
            co_await observed.connect_read.wait();
            co_await observed.route_entered.wait();
            if (inner_tls) co_await observed.tls_entered.wait();
            if (cancelled) user_stop.cancel();
            else observed.expire.set();
            auto result = co_await call;
            co_await call.wait_destroyed_async();
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
        REQUIRE(observed.tcp_deadline == observed.route_deadline);
        REQUIRE(owner->admission_counters_for_test().live == 0);
        REQUIRE(owner->admission_counters_for_test().dialing == 0);
    }
}

TEST_CASE("CONNECT origin trust hostname and unsupported ALPN fail before origin requests",
          "[http][proxy][routes][tls-policy][issue-1249]") {
    for (const auto selected : {backend::epoll, backend::io_uring})
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
        REQUIRE(error->stage == client_stage::tls);
        REQUIRE(error->code.value() > 0);
        if (rejection == 2) REQUIRE(error->code.value() == EPROTONOSUPPORT);
        REQUIRE(observed.requests.size() == 1);
        REQUIRE(observed.requests[0].get_method() == method::CONNECT);
        REQUIRE(owner->admission_counters_for_test().live == 0);
    }
}

TEST_CASE("HTTP Transport forward and CONNECT routes perform real I/O and target-bound reuse",
          "[http][proxy][routes][issue-1249]") {
    elio::tls::tls_context server_context(elio::tls::tls_mode::server);
    temporary_pem ca;
    install_certificate(server_context, ca);
    for (const auto selected : {backend::epoll, backend::io_uring})
    for (const bool finite : {false, true})
    for (const bool secure : {false, true})
    for (const bool streaming : {false, true}) {
        CAPTURE(selected, finite, secure, streaming);
        backend_guard backend_scope(selected);
        auto listener = elio::net::tcp_listener::bind(elio::net::ipv4_address("127.0.0.1", 0));
        REQUIRE(listener);
        transport_config config;
        config.proxy = http_proxy_config{};
        config.proxy->endpoint = "http://127.0.0.1:" + std::to_string(listener->local_address().port());
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
        const auto target = url::parse(secure ? "https://localhost:9443/path" :
                                               "http://unresolved-origin.invalid:8081/path");
        REQUIRE(target);
        const auto plan = owner->route_plan_for_test(*target);
        REQUIRE(plan.key().mode == (secure ? detail::route_mode::connect_tunnel :
                                            detail::route_mode::forward_proxy));
        REQUIRE(plan.key().target_dns == detail::route_dns_mode::proxy);
        REQUIRE(plan.key().hops.size() == 1);
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
            REQUIRE(observed.requests[0].path() == "localhost:9443");
            REQUIRE(observed.requests[0].header("Host") == "localhost:9443");
            REQUIRE(observed.requests[0].header("Proxy-Authorization") == frozen_authorization);
        }
        for (size_t i = 0; i < 2; ++i) {
            const auto& received = observed.requests[i + (secure ? 1 : 0)];
            const auto wanted = secure ? "/path?q=" + std::to_string(i) :
                "http://unresolved-origin.invalid:8081/path?q=" + std::to_string(i);
            REQUIRE(received.path_with_query() == wanted);
            REQUIRE(received.header("Authorization") == "Bearer origin-secret");
            REQUIRE(received.header("Proxy-Authorization") == (secure ? "" : frozen_authorization));
            REQUIRE(received.header("Host") == (secure ? "localhost:9443" :
                                                 "unresolved-origin.invalid:8081"));
        }
        if (finite) REQUIRE(owner->admission_counters_for_test().live == 0);
    }
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
