#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <elio/http/http_client.hpp>
#include <elio/io/io_awaitables.hpp>
#include <elio/sync/event.hpp>
#include <elio/tls/tls_stream.hpp>
#include <openssl/evp.h>
#include <openssl/rsa.h>
#include <openssl/x509.h>
#include "../test_main.cpp"

#include <array>
#include <atomic>
#include <chrono>
#include <exception>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {
using namespace elio;
using backend_type = io::io_context::backend_type;

struct backend_guard {
    explicit backend_guard(backend_type backend)
        : previous(runtime::detail::worker_io_backend_for_test.exchange(backend)) {}
    ~backend_guard() { runtime::detail::worker_io_backend_for_test.store(previous); }
    backend_type previous;
};

struct empty_probe {
    int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    ~empty_probe() { if (fd >= 0) ::close(fd); }
    empty_probe() = default;
    empty_probe(const empty_probe&) = delete;
    empty_probe& operator=(const empty_probe&) = delete;
};

void require_backend(backend_type backend) {
#if ELIO_HAS_IO_URING
    if (backend == backend_type::io_uring && !io::io_uring_backend::is_available()) {
        SKIP("io_uring unavailable on this host");
    }
#else
    if (backend == backend_type::io_uring) SKIP("io_uring support is not compiled");
#endif
}

template<typename Predicate>
bool observe(Predicate predicate) {
    const auto deadline = std::chrono::steady_clock::now() + test::scaled_ms(10000);
    while (!predicate()) {
        if (std::chrono::steady_clock::now() >= deadline) return false;
        std::this_thread::yield();
    }
    return true;
}

bool install_certificate(tls::tls_context& context) {
    std::unique_ptr<EVP_PKEY_CTX, decltype(&EVP_PKEY_CTX_free)> generator(
        EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, nullptr), EVP_PKEY_CTX_free);
    if (!generator || EVP_PKEY_keygen_init(generator.get()) <= 0 ||
        EVP_PKEY_CTX_set_rsa_keygen_bits(generator.get(), 2048) <= 0) return false;
    EVP_PKEY* raw = nullptr;
    if (EVP_PKEY_keygen(generator.get(), &raw) <= 0) return false;
    std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)> key(raw, EVP_PKEY_free);
    std::unique_ptr<X509, decltype(&X509_free)> certificate(X509_new(), X509_free);
    if (!certificate || X509_set_version(certificate.get(), 2) != 1 ||
        ASN1_INTEGER_set(X509_get_serialNumber(certificate.get()), 1) != 1 ||
        !X509_gmtime_adj(X509_getm_notBefore(certificate.get()), 0) ||
        !X509_gmtime_adj(X509_getm_notAfter(certificate.get()), 3600) ||
        X509_set_pubkey(certificate.get(), key.get()) != 1) return false;
    auto* name = X509_get_subject_name(certificate.get());
    if (!name || X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC,
        reinterpret_cast<const unsigned char*>("localhost"), -1, -1, 0) != 1 ||
        X509_set_issuer_name(certificate.get(), name) != 1 ||
        X509_sign(certificate.get(), key.get(), EVP_sha256()) <= 0) return false;
    return SSL_CTX_use_certificate(context.native_handle(), certificate.get()) == 1 &&
        SSL_CTX_use_PrivateKey(context.native_handle(), key.get()) == 1 &&
        SSL_CTX_check_private_key(context.native_handle()) == 1;
}

coro::task<int> peek_client_payload(net::tcp_stream& stream, coro::cancel_token token) {
    char first;
    for (;;) {
        auto read = co_await io::async_recv(stream.fd(), &first, 1, MSG_PEEK, token);
        if (read.was_cancelled()) co_return -ECANCELED;
        if (read.io.result == -EINTR) continue;
        if (read.io.result != -EAGAIN && read.io.result != -EWOULDBLOCK) {
            co_return read.io.result;
        }
        auto ready = co_await stream.poll_read(token);
        if (ready.was_cancelled()) co_return -ECANCELED;
        if (ready.io.result < 0) co_return ready.io.result;
    }
}

struct exchange_fixture {
    exchange_fixture(backend_type backend, bool encrypted,
                     http::client_config config = {})
        : guard(backend), encrypted(encrypted), sched(2),
          server_tls(tls::tls_mode::server), client_config(std::move(config)) {
        listener = net::tcp_listener::bind(net::ipv4_address("127.0.0.1", 0));
        REQUIRE(listener);
        if (encrypted) REQUIRE(install_certificate(server_tls));
        target = *http::url::parse(std::string(encrypted ? "https" : "http") +
            "://127.0.0.1:" + std::to_string(listener->local_address().port()) + "/range");
        client_config.verify_certificate = false;
        client_config.read_timeout = test::scaled_sec(2);
    }

    coro::task<std::optional<net::stream>> accept() {
        for (;;) {
            auto tcp = co_await listener->accept(stop.get_token());
            if (!tcp) {
                accept_error = errno;
                accept_stage = "tcp";
                co_return std::nullopt;
            }
            // Local port probes can arrive before the fixture's client. Peek
            // before TLS so neither a ClientHello nor HTTP bytes are consumed.
            const auto payload = co_await peek_client_payload(*tcp, stop.get_token());
            if (payload == 0) {
                ++empty_connections;
                continue;
            }
            if (payload < 0) {
                accept_error = -payload;
                accept_stage = "peek";
                co_return std::nullopt;
            }
            ++accepted;
            if (!encrypted) co_return net::stream(std::move(*tcp));
            tls::tls_stream secure(std::move(*tcp), server_tls);
            if (!co_await secure.handshake(stop.get_token())) {
                accept_error = errno;
                accept_stage = "tls";
                co_return std::nullopt;
            }
            co_return net::stream(std::move(secure));
        }
    }

    template<typename Server, typename Consumer>
    void run(Server server, Consumer consumer, std::function<void()> interrupt = {}) {
        std::atomic<bool> server_done{false};
        std::atomic<bool> client_done{false};
        std::exception_ptr server_failure;
        std::exception_ptr client_failure;
        sched.start();
        auto server_job = sched.go_joinable([&]() -> coro::task<void> {
            try { co_await server(); } catch (...) { server_failure = std::current_exception(); }
            server_done.store(true, std::memory_order_release);
        });
        auto client_job = sched.go_joinable([&]() -> coro::task<void> {
            try {
                http::client client(client_config);
                co_await consumer(client);
            } catch (...) { client_failure = std::current_exception(); }
            client_done.store(true, std::memory_order_release);
        });
        if (interrupt) interrupt();
        const bool done = observe([&] {
            return server_done.load(std::memory_order_acquire) &&
                client_done.load(std::memory_order_acquire);
        });
        if (!done) stop.cancel();
        REQUIRE(sched.shutdown(test::scaled_ms(5000)));
        CAPTURE(server_done.load(), client_done.load(),
                static_cast<bool>(server_failure), static_cast<bool>(client_failure),
                accepted, empty_connections, accept_error, accept_stage, client_diagnostic);
        REQUIRE(done);
        if (server_failure) std::rethrow_exception(server_failure);
        if (client_failure) std::rethrow_exception(client_failure);
    }

    backend_guard guard;
    bool encrypted;
    runtime::scheduler sched;
    tls::tls_context server_tls;
    http::client_config client_config;
    std::optional<net::tcp_listener> listener;
    http::url target;
    coro::cancel_source stop;
    size_t accepted = 0;
    size_t empty_connections = 0;
    int accept_error = 0;
    std::string_view accept_stage;
    std::string client_diagnostic;
};

struct request_read_observation {
    int last_result = 0;
    size_t reads = 0;
    int64_t elapsed_ms = 0;
};

coro::task<std::string> request_headers(net::stream& stream, coro::cancel_token token,
                                      request_read_observation* observation = nullptr) {
    std::string bytes;
    std::array<char, 1024> buffer;
    const auto started = std::chrono::steady_clock::now();
    while (bytes.find("\r\n\r\n") == std::string::npos) {
        auto read = co_await stream.read(buffer.data(), buffer.size(), token);
        if (observation) {
            observation->last_result = read.result;
            ++observation->reads;
            observation->elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - started).count();
        }
        if (read.result <= 0) break;
        bytes.append(buffer.data(), static_cast<size_t>(read.result));
    }
    co_return bytes;
}

struct body_observation {
    std::string bytes;
    std::optional<http::client_error> error;
    bool complete = false;
    bool empty_nonterminal = false;
    bool repeated_eof = false;
    size_t reads = 0;
};

coro::task<void> consume(http::response_body_reader& body, coro::cancel_token token,
                         body_observation& observation, size_t capacity = 3) {
    std::array<char, 257> buffer;
    auto empty = co_await body.read_into(std::span<char>{}, token);
    if (auto* progress = std::get_if<http::body_read_progress>(&empty)) {
        observation.empty_nonterminal = progress->transferred == 0 && !progress->complete;
    }
    for (;;) {
        auto read = co_await body.read_into(std::span<char>(buffer).first(capacity), token);
        ++observation.reads;
        if (auto* error = std::get_if<http::client_error>(&read)) {
            observation.error = *error;
            co_return;
        }
        auto progress = std::get<http::body_read_progress>(read);
        observation.bytes.append(buffer.data(), progress.transferred);
        if (progress.complete) {
            observation.complete = body.complete();
            auto again = co_await body.read_into(std::span<char>(buffer), token);
            auto* eof = std::get_if<http::body_read_progress>(&again);
            observation.repeated_eof = eof && eof->complete && eof->transferred == 0;
            co_return;
        }
    }
}

int result_error(const http::client_result<std::monostate>& result) {
    const auto* error = std::get_if<http::client_error>(&result);
    return error ? error->code.value() : 0;
}

struct response_observer_guard {
    response_observer_guard() {
        http::detail::client_response_read_staged_for_test.store(false);
        http::detail::response_read_stage_for_test.store(http::client_stage::headers);
        http::detail::observe_client_response_read_entry_for_test.store(true);
    }
    ~response_observer_guard() {
        http::detail::observe_client_response_read_entry_for_test.store(false);
    }
};

bool body_read_parked(exchange_fixture& fixture) {
    // TLS waits for socket readiness rather than using cancellable async_recv,
    // so the recv-only hook is not a portable parking observation. The peer is
    // behind an event here: two pending operations mean receive + its watchdog.
    return http::detail::response_read_stage_for_test.load(std::memory_order_acquire) ==
            http::client_stage::body &&
        fixture.sched.get_worker(0)->io_context().pending_count() +
            fixture.sched.get_worker(1)->io_context().pending_count() >= 2;
}

std::vector<std::chrono::steady_clock::time_point>* observed_deadlines = nullptr;
void record_deadline(std::chrono::steady_clock::time_point deadline) {
    observed_deadlines->push_back(deadline);
}
struct deadline_observer_guard {
    explicit deadline_observer_guard(std::vector<std::chrono::steady_clock::time_point>& values) {
        observed_deadlines = &values;
        http::detail::response_deadline_for_test.store(record_deadline);
    }
    ~deadline_observer_guard() {
        http::detail::response_deadline_for_test.store(nullptr);
        observed_deadlines = nullptr;
    }
};

struct expect_expiry_guard {
    explicit expect_expiry_guard(bool enabled)
        : previous(http::detail::expire_expect_after_headers_for_test.exchange(enabled)) {}
    ~expect_expiry_guard() {
        http::detail::expire_expect_after_headers_for_test.store(previous);
    }
    bool previous;
};

sync::event* failing_watchdog_release = nullptr;
coro::task<coro::cancel_result> failing_body_watchdog(
        std::chrono::nanoseconds duration, coro::cancel_token token,
        http::client_stage stage) {
    if (stage != http::client_stage::body) {
        co_return co_await time::sleep_for(duration, token);
    }
    auto result = co_await failing_watchdog_release->wait(token);
    if (result == coro::cancel_result::completed) {
        throw std::runtime_error("injected response watchdog failure");
    }
    co_return result;
}

struct watchdog_failure_guard {
    explicit watchdog_failure_guard(sync::event& release) {
        failing_watchdog_release = &release;
        http::detail::response_watchdog_wait_for_test.store(failing_body_watchdog);
    }
    ~watchdog_failure_guard() {
        http::detail::response_watchdog_wait_for_test.store(nullptr);
        failing_watchdog_release = nullptr;
    }
};
} // namespace

TEST_CASE("HTTP streaming fixture ignores empty connections before admitting the client",
          "[http][client][streaming][http_client_streaming][fixture]") {
    const auto backend = GENERATE(backend_type::epoll, backend_type::io_uring);
    require_backend(backend);
    const bool encrypted = GENERATE(false, true);
    CAPTURE(static_cast<int>(backend), encrypted);
    http::client_config config;
    config.connect_timeout = test::scaled_sec(2);
    exchange_fixture fixture(backend, encrypted, config);
    empty_probe probe;
    REQUIRE(probe.fd >= 0);
    const auto address = net::ipv4_address("127.0.0.1", fixture.target.port).to_sockaddr();
    // Queue the unrelated connection before either scheduler worker starts.
    REQUIRE(::connect(probe.fd, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == 0);
    REQUIRE(::shutdown(probe.fd, SHUT_WR) == 0);

    bool own_request_seen = false;
    body_observation body;
    http::client_result<std::monostate> result;
    fixture.run([&]() -> coro::task<void> {
        auto stream = co_await fixture.accept();
        if (!stream) throw std::runtime_error("server accept failed");
        const auto headers = co_await request_headers(*stream, fixture.stop.get_token());
        own_request_seen = headers.starts_with("GET /range ");
        (void)co_await stream->write_all(
            "HTTP/1.1 200 OK\r\nContent-Length: 7\r\nConnection: close\r\n\r\nhealthy",
            fixture.stop.get_token());
        http::detail::abort_stream_io(*stream);
    }, [&](http::client& client) -> coro::task<void> {
        result = co_await client.with_response(http::request(http::method::GET, "/range"),
            fixture.target, {}, [&](const http::response&, http::response_body_reader& reader,
                                    coro::cancel_token token) -> coro::task<void> {
                co_await consume(reader, token, body);
            });
    });
    CAPTURE(own_request_seen, fixture.accepted, fixture.empty_connections,
            result_error(result), body.bytes.size());
    REQUIRE(result_error(result) == 0);
    REQUIRE(own_request_seen);
    REQUIRE(body.bytes == "healthy");
    REQUIRE(body.complete);
    REQUIRE(fixture.accepted == 1);
    REQUIRE(fixture.empty_connections >= 1);
}

TEST_CASE("HTTP streaming client decodes bounded fragmented payloads", "[http][client][streaming][http_client_streaming]") {
    const auto backend = GENERATE(backend_type::epoll, backend_type::io_uring);
    require_backend(backend);
    const bool encrypted = GENERATE(false, true);
    const auto framing = GENERATE(0, 1, 2);
    const bool fragmented = GENERATE(false, true);
    CAPTURE(static_cast<int>(backend), encrypted, framing, fragmented);
    // Keep tiny-fragment decoding separate from large-body coverage: neither
    // contract requires tens of thousands of pulls within a fixed I/O budget.
    const std::string payload(fragmented ? 257 : 65539, 'r');
    const std::string range = "bytes 0-" + std::to_string(payload.size() - 1) +
        "/" + std::to_string(payload.size());
    http::client_config config;
    config.max_response_size = 1;
    config.read_buffer_size = fragmented ? 7 : 4096;
    exchange_fixture fixture(backend, encrypted, config);
    body_observation observation;
    bool headers_ok = false;
    int payload_written = 0;
    http::client_result<std::monostate> result;
    const auto started = std::chrono::steady_clock::now();
    fixture.run([&]() -> coro::task<void> {
        auto stream = co_await fixture.accept();
        if (!stream) throw std::runtime_error("server accept failed");
        (void)co_await request_headers(*stream, fixture.stop.get_token());
        std::string head = "HTTP/1.1 206 Partial Content\r\nContent-Range: " + range + "\r\n";
        if (framing == 0) head += "Content-Length: " + std::to_string(payload.size()) + "\r\n";
        if (framing == 1) head += "Transfer-Encoding: chunked\r\n";
        head += "Connection: close\r\n\r\n";
        for (char byte : head) {
            (void)co_await stream->write_all(std::string_view(&byte, 1), fixture.stop.get_token());
        }
        if (framing == 1) {
            const std::string_view chunk_size = fragmented ? "101\r\n" : "10003\r\n";
            (void)co_await stream->write_all(chunk_size, fixture.stop.get_token());
        }
        payload_written = (co_await stream->write_all(payload, fixture.stop.get_token())).result;
        if (framing == 1) {
            (void)co_await stream->write_all("\r\n0\r\nX-End: yes\r\n\r\n", fixture.stop.get_token());
        }
        (void)co_await stream->finish_write(fixture.stop.get_token());
        http::detail::abort_stream_io(*stream);
    }, [&](http::client& client) -> coro::task<void> {
        result = co_await client.with_response(http::request(http::method::GET, "/range"),
            fixture.target, {}, [&](const http::response& head, http::response_body_reader& body,
                                    coro::cancel_token token) -> coro::task<void> {
                headers_ok = head.status_code() == 206 && head.body().empty() &&
                    head.header("Content-Range") == range;
                co_await consume(body, token, observation, fragmented ? 3 : 257);
            });
    });
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started).count();
    const auto error_stage = observation.error ? static_cast<int>(observation.error->stage) : -1;
    CAPTURE(elapsed, observation.reads, observation.bytes.size(), payload_written, error_stage);
    REQUIRE(headers_ok);
    REQUIRE(result_error(result) == 0);
    REQUIRE(observation.bytes == payload);
    REQUIRE(observation.complete);
    REQUIRE(observation.empty_nonterminal);
    REQUIRE(observation.repeated_eof);
}

TEST_CASE("HTTP streaming client delivers headers before body and pauses consumption",
          "[http][client][streaming][http_client_streaming][backpressure]") {
    exchange_fixture fixture(backend_type::epoll, false);
    sync::event header_handler;
    sync::event release_body;
    sync::event handler_paused;
    sync::event resume_handler;
    std::atomic<bool> callback_seen{false};
    body_observation observation;
    http::client_result<std::monostate> result;
    fixture.run([&]() -> coro::task<void> {
        auto stream = co_await fixture.accept();
        if (!stream) throw std::runtime_error("server accept failed");
        (void)co_await request_headers(*stream, fixture.stop.get_token());
        (void)co_await stream->write_all("HTTP/1.1 200 OK\r\nContent-Length: 6\r\n\r\n",
                                        fixture.stop.get_token());
        (void)co_await header_handler.wait(fixture.stop.get_token());
        callback_seen = true;
        release_body.set();
        (void)co_await handler_paused.wait(fixture.stop.get_token());
        (void)co_await stream->write_all("abcdef", fixture.stop.get_token());
        // No client body receive may be pending while its handler is paused.
        const bool idle = fixture.sched.get_worker(0)->io_context().pending_count() == 0 &&
            fixture.sched.get_worker(1)->io_context().pending_count() == 0;
        if (!idle) throw std::runtime_error("client prefetched while handler was paused");
        resume_handler.set();
        char byte;
        (void)co_await stream->read(&byte, 1, fixture.stop.get_token());
    }, [&](http::client& client) -> coro::task<void> {
        result = co_await client.with_response(http::request(http::method::GET, "/range"),
            fixture.target, {}, [&](const http::response&, http::response_body_reader& body,
                                    coro::cancel_token token) -> coro::task<void> {
                header_handler.set();
                (void)co_await release_body.wait(token);
                handler_paused.set();
                (void)co_await resume_handler.wait(token);
                co_await consume(body, token, observation);
            });
    });
    REQUIRE(callback_seen);
    REQUIRE(result_error(result) == 0);
    REQUIRE(observation.bytes == "abcdef");
    REQUIRE(observation.complete);
}

TEST_CASE("HTTP streaming client reuses only completely consumed responses",
          "[http][client][streaming][http_client_streaming][pool]") {
    const auto backend = GENERATE(backend_type::epoll, backend_type::io_uring);
    require_backend(backend);
    const bool encrypted = GENERATE(false, true);
    // Consume, abandon, throw, buffered suffix, close-delimited, truncated.
    const auto disposition = GENERATE(0, 1, 2, 3, 4, 5);
    CAPTURE(static_cast<int>(backend), encrypted, disposition);
    const bool abandon = disposition == 1;
    const bool throws = disposition == 2;
    const bool discard = disposition != 0;
    exchange_fixture fixture(backend, encrypted);
    bool first_closed = false;
    bool second_request_seen = false;
    bool exception_preserved = false;
    body_observation first;
    body_observation second;
    int first_error = 0;
    int second_error = 0;
    size_t first_request_size = 0;
    request_read_observation first_request_read;
    int first_write_result = 0;
    bool first_result_expected = false;
    sync::event first_result_ready;
    fixture.run([&]() -> coro::task<void> {
        auto stream = co_await fixture.accept();
        if (!stream) throw std::runtime_error("server accept failed");
        first_request_size = (co_await request_headers(*stream, fixture.stop.get_token(),
                                                     &first_request_read)).size();
        const std::string first_response = disposition == 4
            ? "HTTP/1.1 200 OK\r\nConnection: close\r\n\r\nfirst"
            : "HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\n";
        std::string first_wire = first_response;
        if (disposition != 4) first_wire += disposition == 5 ? "fi" : "first";
        if (disposition == 3) first_wire += "unexpected";
        first_write_result = (co_await stream->write_all(first_wire, fixture.stop.get_token())).result;
        if (disposition >= 4) (void)co_await stream->finish_write(fixture.stop.get_token());
        auto next = co_await request_headers(*stream, fixture.stop.get_token());
        (void)co_await first_result_ready.wait(fixture.stop.get_token());
        if (!first_result_expected) co_return;
        if (discard) {
            first_closed = next.empty();
            http::detail::abort_stream_io(*stream);
            stream = co_await fixture.accept();
            if (!stream) throw std::runtime_error("second server accept failed");
            next = co_await request_headers(*stream, fixture.stop.get_token());
        }
        second_request_seen = next.starts_with("GET /range ");
        (void)co_await stream->write_all(
            "HTTP/1.1 200 OK\r\nContent-Length: 6\r\nConnection: close\r\n\r\nsecond",
            fixture.stop.get_token());
        http::detail::abort_stream_io(*stream);
    }, [&](http::client& client) -> coro::task<void> {
        try {
            auto result = co_await client.with_response(http::request(http::method::GET, "/range"),
                fixture.target, {}, [&](const http::response&, http::response_body_reader& body,
                                        coro::cancel_token token) -> coro::task<void> {
                    if (throws) throw std::runtime_error("application rejection");
                    if (!abandon) co_await consume(body, token, first);
                });
            first_error = result_error(result);
            fixture.client_diagnostic = "first_error=" + std::to_string(first_error);
            if (const auto* error = std::get_if<http::client_error>(&result)) {
                fixture.client_diagnostic += "; stage=" +
                    std::to_string(static_cast<int>(error->stage));
            }
        } catch (const std::runtime_error& error) {
            exception_preserved = std::string(error.what()) == "application rejection";
        }
        first_result_expected = throws ? exception_preserved :
            first_error == (disposition == 5 ? EBADMSG : 0);
        first_result_ready.set();
        if (!first_result_expected) co_return;
        auto result = co_await client.with_response(http::request(http::method::GET, "/range"),
            fixture.target, {}, [&](const http::response&, http::response_body_reader& body,
                                    coro::cancel_token token) -> coro::task<void> {
                co_await consume(body, token, second);
            });
        second_error = result_error(result);
    });
    CAPTURE(first_error, second_error, fixture.client_diagnostic, fixture.accepted,
            fixture.accept_error, fixture.accept_stage, first_request_size,
            first_request_read.last_result, first_request_read.reads, first_request_read.elapsed_ms,
            first_write_result, first.bytes.size(), first.reads, first.complete);
    REQUIRE(first_error == (disposition == 5 ? EBADMSG : 0));
    REQUIRE(second_error == 0);
    REQUIRE(exception_preserved == throws);
    REQUIRE(second_request_seen);
    REQUIRE(second.bytes == "second");
    REQUIRE(fixture.accepted == (discard ? 2 : 1));
    if (discard) REQUIRE(first_closed);
    if (!abandon && !throws) REQUIRE(first.bytes == (disposition == 5 ? "fi" : "first"));
}

TEST_CASE("HTTP streaming concurrent entry rejects only the competing read",
          "[http][client][streaming][http_client_streaming][concurrent][pool]") {
    const auto backend = GENERATE(backend_type::epoll, backend_type::io_uring);
    require_backend(backend);
    const bool encrypted = GENERATE(false, true);
    CAPTURE(static_cast<int>(backend), encrypted);
    exchange_fixture fixture(backend, encrypted);
    response_observer_guard observer;
    sync::event first_parked;
    sync::event competing_read_finished;
    std::atomic<bool> first_started{false};
    bool parked = false;
    int competing_error = 0;
    bool competing_buffer_untouched = false;
    bool admitted_read_succeeded = false;
    bool reader_not_poisoned = false;
    bool second_request_seen = false;
    body_observation first;
    body_observation second;
    int first_error = 0;
    int second_error = 0;
    fixture.run([&]() -> coro::task<void> {
        auto stream = co_await fixture.accept();
        if (!stream) throw std::runtime_error("server accept failed");
        (void)co_await request_headers(*stream, fixture.stop.get_token());
        (void)co_await stream->write_all("HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\n",
                                        fixture.stop.get_token());
        // Withhold all payload until the competing call has returned. The
        // admitted read cannot finish successfully before that diagnostic.
        (void)co_await competing_read_finished.wait(fixture.stop.get_token());
        (void)co_await stream->write_all("first", fixture.stop.get_token());
        auto next = co_await request_headers(*stream, fixture.stop.get_token());
        second_request_seen = next.starts_with("GET /range ");
        (void)co_await stream->write_all(
            "HTTP/1.1 200 OK\r\nContent-Length: 6\r\nConnection: close\r\n\r\nsecond",
            fixture.stop.get_token());
        http::detail::abort_stream_io(*stream);
    }, [&](http::client& client) -> coro::task<void> {
        auto result = co_await client.with_response(http::request(http::method::GET, "/range"),
            fixture.target, fixture.stop.get_token(),
            [&](const http::response&, http::response_body_reader& body,
                coro::cancel_token token) -> coro::task<void> {
                std::array<char, 5> admitted_buffer{};
                auto admitted = fixture.sched.go_joinable([&]() -> coro::task<http::body_read_result> {
                    first_started.store(true, std::memory_order_release);
                    co_return co_await body.read_into(std::span<char>(admitted_buffer), token);
                });
                std::exception_ptr failure;
                try {
                    (void)co_await first_parked.wait(token);
                    std::array<char, 5> rejected_buffer;
                    rejected_buffer.fill('r');
                    auto rejected = co_await body.read_into(std::span<char>(rejected_buffer), token);
                    if (const auto* error = std::get_if<http::client_error>(&rejected)) {
                        competing_error = error->code.value();
                    }
                    competing_buffer_untouched = std::all_of(
                        rejected_buffer.begin(), rejected_buffer.end(), [](char value) { return value == 'r'; });
                } catch (...) {
                    failure = std::current_exception();
                }
                competing_read_finished.set();
                http::body_read_result read;
                try { read = co_await admitted; }
                catch (...) { if (!failure) failure = std::current_exception(); }
                co_await admitted.wait_destroyed_async();
                if (failure) std::rethrow_exception(failure);
                if (const auto* progress = std::get_if<http::body_read_progress>(&read)) {
                    admitted_read_succeeded = progress->transferred > 0 &&
                        progress->transferred <= admitted_buffer.size() &&
                        std::string_view(admitted_buffer.data(), progress->transferred) ==
                            std::string_view("first").substr(0, progress->transferred);
                    if (admitted_read_succeeded) {
                        first.bytes.append(admitted_buffer.data(), progress->transferred);
                    }
                }
                reader_not_poisoned = !body.error(); // The admitted read has settled.
                co_await consume(body, token, first);
            });
        first_error = result_error(result);
        result = co_await client.with_response(http::request(http::method::GET, "/range"),
            fixture.target, fixture.stop.get_token(),
            [&](const http::response&, http::response_body_reader& body,
                coro::cancel_token token) -> coro::task<void> {
                co_await consume(body, token, second);
            });
        second_error = result_error(result);
    }, [&] {
        parked = observe([&] {
            return first_started.load(std::memory_order_acquire) && body_read_parked(fixture);
        });
        first_parked.set();
    });
    REQUIRE(parked);
    REQUIRE(competing_error == EALREADY);
    REQUIRE(competing_buffer_untouched);
    REQUIRE(admitted_read_succeeded);
    REQUIRE(reader_not_poisoned);
    REQUIRE(first_error == 0);
    REQUIRE(first.bytes == "first");
    REQUIRE(first.complete);
    REQUIRE(second_error == 0);
    REQUIRE(second_request_seen);
    REQUIRE(second.bytes == "second");
    REQUIRE(second.complete);
    REQUIRE(fixture.accepted == 1);
}

TEST_CASE("HTTP streaming client keeps errors sticky and honors independent limits",
          "[http][client][streaming][http_client_streaming][limits]") {
    const auto failure = GENERATE(0, 1, 2, 3);
    http::client_config config;
    if (failure == 2) config.max_header_size = 32;
    exchange_fixture fixture(backend_type::epoll, false, config);
    http::streaming_response_options options;
    if (failure == 1) options.max_body_size = 3;
    if (failure == 3) options.max_informational_bytes = 10;
    body_observation observation;
    bool invoked = false;
    bool sticky = false;
    http::client_result<std::monostate> result;
    fixture.run([&]() -> coro::task<void> {
        auto stream = co_await fixture.accept();
        if (!stream) throw std::runtime_error("server accept failed");
        (void)co_await request_headers(*stream, fixture.stop.get_token());
        std::string response;
        if (failure == 3) response = "HTTP/1.1 103 Early Hints\r\n\r\n";
        response += "HTTP/1.1 200 OK\r\nContent-Length: 5\r\n";
        if (failure == 2) response += "X-Large: " + std::string(50, 'h') + "\r\n";
        response += "Connection: close\r\n\r\n";
        response += failure == 0 ? "ab" : "abcde";
        (void)co_await stream->write_all(response, fixture.stop.get_token());
        http::detail::abort_stream_io(*stream);
    }, [&](http::client& client) -> coro::task<void> {
        result = co_await client.with_response(http::request(http::method::GET, "/range"),
            fixture.target, {}, [&](const http::response&, http::response_body_reader& body,
                                    coro::cancel_token token) -> coro::task<void> {
                invoked = true;
                co_await consume(body, token, observation);
                std::array<std::byte, 5> buffer;
                buffer.fill(std::byte{0x77});
                auto again = co_await body.read_into(std::span<std::byte>(buffer), token);
                const auto* error = std::get_if<http::client_error>(&again);
                sticky = error && observation.error && error->code == observation.error->code &&
                    error->stage == observation.error->stage && buffer.front() == std::byte{0x77};
            }, options);
    });
    REQUIRE(result_error(result) == (failure == 0 ? EBADMSG : EMSGSIZE));
    REQUIRE(invoked == (failure < 2));
    if (invoked) {
        REQUIRE(sticky);
        REQUIRE_FALSE(observation.complete);
    }
}

TEST_CASE("HTTP streaming completion precedes later read cancellation",
          "[http][client][streaming][http_client_streaming][cancel][completion][pool]") {
    const auto backend = GENERATE(backend_type::epoll, backend_type::io_uring);
    require_backend(backend);
    const bool encrypted = GENERATE(false, true);
    const bool cancel_scope = GENERATE(false, true);
    CAPTURE(static_cast<int>(backend), encrypted, cancel_scope);
    exchange_fixture fixture(backend, encrypted);
    coro::cancel_source scope_cancel;
    body_observation first;
    body_observation second;
    bool repeated_completion = false;
    bool reader_not_poisoned = false;
    bool buffer_untouched = false;
    bool first_closed = false;
    bool second_request_seen = false;
    int first_error = 0;
    int second_error = 0;
    fixture.run([&]() -> coro::task<void> {
        auto stream = co_await fixture.accept();
        if (!stream) throw std::runtime_error("server accept failed");
        (void)co_await request_headers(*stream, fixture.stop.get_token());
        (void)co_await stream->write_all("HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nfirst",
                                        fixture.stop.get_token());
        auto next = co_await request_headers(*stream, fixture.stop.get_token());
        first_closed = next.empty();
        if (first_closed) {
            http::detail::abort_stream_io(*stream);
            stream = co_await fixture.accept();
            if (!stream) throw std::runtime_error("second server accept failed");
            next = co_await request_headers(*stream, fixture.stop.get_token());
        }
        second_request_seen = next.starts_with("GET /range ");
        (void)co_await stream->write_all(
            "HTTP/1.1 200 OK\r\nContent-Length: 6\r\nConnection: close\r\n\r\nsecond",
            fixture.stop.get_token());
        http::detail::abort_stream_io(*stream);
    }, [&](http::client& client) -> coro::task<void> {
        auto result = co_await client.with_response(http::request(http::method::GET, "/range"),
            fixture.target, scope_cancel.get_token(),
            [&](const http::response&, http::response_body_reader& body,
                coro::cancel_token token) -> coro::task<void> {
                co_await consume(body, token, first);
                if (cancel_scope) scope_cancel.cancel();
                coro::cancel_source later_read;
                later_read.cancel();
                std::array<char, 5> buffer;
                buffer.fill('r');
                auto repeated = co_await body.read_into(std::span<char>(buffer), later_read.get_token());
                auto empty = co_await body.read_into(std::span<char>{}, later_read.get_token());
                const auto* progress = std::get_if<http::body_read_progress>(&repeated);
                const auto* empty_progress = std::get_if<http::body_read_progress>(&empty);
                repeated_completion = progress && empty_progress && progress->complete &&
                    empty_progress->complete && progress->transferred == 0 &&
                    empty_progress->transferred == 0 && body.complete();
                reader_not_poisoned = !body.error();
                buffer_untouched = std::all_of(buffer.begin(), buffer.end(),
                                              [](char value) { return value == 'r'; });
            });
        first_error = result_error(result);
        result = co_await client.with_response(http::request(http::method::GET, "/range"),
            fixture.target, fixture.stop.get_token(),
            [&](const http::response&, http::response_body_reader& body,
                coro::cancel_token token) -> coro::task<void> {
                co_await consume(body, token, second);
            });
        second_error = result_error(result);
    });
    REQUIRE(first.bytes == "first");
    REQUIRE(first.complete);
    REQUIRE(repeated_completion);
    REQUIRE(reader_not_poisoned);
    REQUIRE(buffer_untouched);
    REQUIRE(first_error == (cancel_scope ? ECANCELED : 0));
    REQUIRE(first_closed == cancel_scope);
    REQUIRE(second_error == 0);
    REQUIRE(second_request_seen);
    REQUIRE(second.bytes == "second");
    REQUIRE(second.complete);
    REQUIRE(fixture.accepted == (cancel_scope ? 2 : 1));
}

TEST_CASE("HTTP streaming client rejects protocol handoffs before invoking the handler",
          "[http][client][streaming][http_client_streaming][handoff]") {
    const auto backend = GENERATE(backend_type::epoll, backend_type::io_uring);
    require_backend(backend);
    const bool encrypted = GENERATE(false, true);
    const auto code = GENERATE(101, 200, 204, 299, 403);
    const bool consumes = GENERATE(false, true);
    CAPTURE(static_cast<int>(backend), encrypted, code, consumes);
    exchange_fixture fixture(backend, encrypted);
    const bool handoff = code != 403;
    bool invoked = false;
    bool closed = false;
    body_observation body_read;
    http::client_result<std::monostate> result;
    fixture.run([&]() -> coro::task<void> {
        auto stream = co_await fixture.accept();
        if (!stream) throw std::runtime_error("server accept failed");
        (void)co_await request_headers(*stream, fixture.stop.get_token());
        const auto headers = "HTTP/1.1 " + std::to_string(code) + " Response\r\n";
        const auto wire = handoff ? headers + "\r\nopaque tunnel bytes"
            : headers + "Content-Length: 6\r\nConnection: close\r\n\r\ndenied";
        (void)co_await stream->write_all(wire, fixture.stop.get_token());
        char byte;
        closed = (co_await stream->read(&byte, 1, fixture.stop.get_token())).result <= 0;
        http::detail::abort_stream_io(*stream);
    }, [&](http::client& client) -> coro::task<void> {
        auto request = http::request(code == 101 ? http::method::GET : http::method::CONNECT,
                                     code == 101 ? "/range" : "example.test:443");
        result = co_await client.with_response(std::move(request), fixture.target,
            fixture.stop.get_token(),
            [&](const http::response&, http::response_body_reader& body,
                coro::cancel_token token) -> coro::task<void> {
                invoked = true;
                if (consumes) co_await consume(body, token, body_read);
            });
    });
    REQUIRE(closed);
    REQUIRE(invoked == !handoff);
    REQUIRE(result_error(result) == (handoff ? EBADMSG : 0));
    if (handoff) {
        REQUIRE(std::get<http::client_error>(result).stage == http::client_stage::framing);
    } else if (consumes) {
        REQUIRE(body_read.bytes == "denied");
        REQUIRE(body_read.complete);
    }
}

TEST_CASE("HTTP streaming cancellation settles reads before borrowed buffer reuse",
          "[http][client][streaming][http_client_streaming][cancel]") {
    const auto backend = GENERATE(backend_type::epoll, backend_type::io_uring);
    require_backend(backend);
    const bool encrypted = GENERATE(false, true);
    const bool scope_cancel = GENERATE(false, true);
    CAPTURE(static_cast<int>(backend), encrypted, scope_cancel);
    exchange_fixture fixture(backend, encrypted);
    response_observer_guard observer;
    coro::cancel_source cancellation;
    sync::event read_returned;
    sync::event late_write_finished;
    bool parked = false;
    std::atomic<bool> body_started{false};
    bool buffer_reused = false;
    bool sticky = false;
    int read_error = 0;
    http::client_result<std::monostate> result;
    fixture.run([&]() -> coro::task<void> {
        auto stream = co_await fixture.accept();
        if (!stream) throw std::runtime_error("server accept failed");
        (void)co_await request_headers(*stream, fixture.stop.get_token());
        (void)co_await stream->write_all("HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\n",
                                        fixture.stop.get_token());
        (void)co_await read_returned.wait(fixture.stop.get_token());
        (void)co_await stream->write_all("late!", fixture.stop.get_token());
        late_write_finished.set();
        char byte;
        (void)co_await stream->read(&byte, 1, fixture.stop.get_token());
        http::detail::abort_stream_io(*stream);
    }, [&](http::client& client) -> coro::task<void> {
        // Name conditional token temporaries before co_await: GCC 12 can
        // destroy a conditional-expression shared owner twice during lowering.
        auto scope_token = scope_cancel ? cancellation.get_token() : coro::cancel_token{};
        result = co_await client.with_response(http::request(http::method::GET, "/range"),
            fixture.target, scope_token,
            [&](const http::response&, http::response_body_reader& body,
                coro::cancel_token) -> coro::task<void> {
                std::array<char, 5> buffer{};
                auto read_token = scope_cancel ? coro::cancel_token{} : cancellation.get_token();
                body_started.store(true, std::memory_order_release);
                auto read = co_await body.read_into(std::span<char>(buffer), read_token);
                const auto* error = std::get_if<http::client_error>(&read);
                read_error = error ? error->code.value() : 0;
                buffer.fill('r');
                read_returned.set();
                (void)co_await late_write_finished.wait(fixture.stop.get_token());
                buffer_reused = std::all_of(buffer.begin(), buffer.end(),
                                            [](char value) { return value == 'r'; });
                auto again = co_await body.read_into(std::span<char>(buffer));
                const auto* repeated = std::get_if<http::client_error>(&again);
                sticky = repeated && repeated->code.value() == ECANCELED &&
                    repeated->stage == http::client_stage::body && buffer.front() == 'r';
            });
    }, [&] {
        parked = observe([&] {
            return body_started.load(std::memory_order_acquire) && body_read_parked(fixture);
        });
        cancellation.cancel();
    });
    REQUIRE(parked);
    REQUIRE(read_error == ECANCELED);
    REQUIRE(result_error(result) == ECANCELED);
    REQUIRE(buffer_reused);
    REQUIRE(sticky);
}

TEST_CASE("HTTP streaming body reads retain the original response deadline",
          "[http][client][streaming][http_client_streaming][timeout]") {
    const auto backend = GENERATE(backend_type::epoll, backend_type::io_uring);
    require_backend(backend);
    const bool encrypted = GENERATE(false, true);
    CAPTURE(static_cast<int>(backend), encrypted);
    exchange_fixture fixture(backend, encrypted);
    response_observer_guard observer;
    std::vector<std::chrono::steady_clock::time_point> deadlines;
    deadline_observer_guard deadline_observer(deadlines);
    bool parked = false;
    bool buffer_reused = false;
    sync::event handler_finished;
    http::client_result<std::monostate> result;
    int read_error = 0;
    fixture.run([&]() -> coro::task<void> {
        auto stream = co_await fixture.accept();
        if (!stream) throw std::runtime_error("server accept failed");
        (void)co_await request_headers(*stream, fixture.stop.get_token());
        (void)co_await stream->write_all("HTTP/1.1 200 OK\r\nContent-Length: 6\r\n\r\na",
                                        fixture.stop.get_token());
        (void)co_await handler_finished.wait(fixture.stop.get_token());
        (void)co_await stream->write_all("later", fixture.stop.get_token());
        http::detail::abort_stream_io(*stream);
    }, [&](http::client& client) -> coro::task<void> {
        result = co_await client.with_response(http::request(http::method::GET, "/range"),
            fixture.target, {}, [&](const http::response&, http::response_body_reader& body,
                                    coro::cancel_token token) -> coro::task<void> {
                std::array<char, 5> buffer{};
                auto first = co_await body.read_into(std::span<char>(buffer), token);
                auto* progress = std::get_if<http::body_read_progress>(&first);
                if (!progress || progress->transferred != 1 || buffer[0] != 'a') {
                    throw std::runtime_error("first body fragment missing");
                }
                auto read = co_await body.read_into(std::span<char>(buffer), token);
                const auto* error = std::get_if<http::client_error>(&read);
                read_error = error ? error->code.value() : 0;
                buffer.fill('r');
                auto again = co_await body.read_into(std::span<char>(buffer), token);
                buffer_reused = std::holds_alternative<http::client_error>(again) &&
                    std::all_of(buffer.begin(), buffer.end(), [](char value) { return value == 'r'; });
                handler_finished.set();
            });
    }, [&] { parked = observe([&] { return body_read_parked(fixture); }); });
    REQUIRE(parked);
    REQUIRE(read_error == ETIMEDOUT);
    REQUIRE(result_error(result) == ETIMEDOUT);
    REQUIRE(std::get<http::client_error>(result).stage == http::client_stage::body);
    REQUIRE(buffer_reused);
    REQUIRE(deadlines.size() >= 2);
    REQUIRE(std::all_of(deadlines.begin(), deadlines.end(),
        [&](auto deadline) { return deadline == deadlines.front(); }));
}

TEST_CASE("HTTP streaming Expect uses the shared interim and final response policy",
          "[http][client][streaming][http_client_streaming][expect]") {
    const auto mode = GENERATE(0, 1, 2, 3);
    http::client_config config;
    config.expect_continue_timeout = mode == 2 ? std::chrono::milliseconds{0}
                                               : std::chrono::milliseconds{1000};
    if (mode == 3) config.expect_continue_timeout = test::scaled_ms(10);
    exchange_fixture fixture(backend_type::epoll, false, config);
    response_observer_guard observer;
    expect_expiry_guard expire_expect(mode == 1);
    sync::event release_final_body;
    std::atomic<bool> final_header_handler{false};
    bool final_body_read_parked = false;
    std::string uploaded;
    bool expect_seen = false;
    bool no_early_upload = false;
    int status = 0;
    body_observation observation;
    http::client_result<std::monostate> result;
    fixture.run([&]() -> coro::task<void> {
        auto stream = co_await fixture.accept();
        if (!stream) throw std::runtime_error("server accept failed");
        auto headers = co_await request_headers(*stream, fixture.stop.get_token());
        expect_seen = headers.find("Expect: 100-continue\r\n") != std::string::npos;
        const auto end = headers.find("\r\n\r\n") + 4;
        uploaded = headers.substr(end);
        no_early_upload = uploaded.empty();
        if (mode == 0) {
            (void)co_await stream->write_all(
                "HTTP/1.1 103 Early Hints\r\nLink: </hint>\r\n\r\nHTTP/1.1 100 Continue\r\n\r\n",
                fixture.stop.get_token());
        }
        if (mode != 1) {
            std::array<char, 16> buffer;
            while (uploaded.size() < 7) {
                auto read = co_await stream->read(buffer.data(), buffer.size(), fixture.stop.get_token());
                if (read.result <= 0) break;
                uploaded.append(buffer.data(), static_cast<size_t>(read.result));
            }
        }
        if (mode == 1) {
            (void)co_await stream->write_all(
                "HTTP/1.1 417 Expectation Failed\r\nContent-Length: 2\r\n\r\n",
                fixture.stop.get_token());
            (void)co_await release_final_body.wait(fixture.stop.get_token());
            (void)co_await stream->write_all("no", fixture.stop.get_token());
        } else {
            (void)co_await stream->write_all(
                "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok", fixture.stop.get_token());
        }
        // The callback consumes the final body before the client closes its pool.
        std::array<char, 16> extra;
        auto read = co_await stream->read(extra.data(), extra.size(), fixture.stop.get_token());
        if (read.result > 0) uploaded.append(extra.data(), static_cast<size_t>(read.result));
        http::detail::abort_stream_io(*stream);
    }, [&](http::client& client) -> coro::task<void> {
        http::request req(http::method::POST, "/range");
        req.set_body(std::string_view("payload"));
        req.set_expect_continue();
        result = co_await client.with_response(std::move(req), fixture.target, {},
            [&](const http::response& head, http::response_body_reader& body,
                coro::cancel_token token) -> coro::task<void> {
                status = head.status_code();
                final_header_handler.store(true, std::memory_order_release);
                co_await consume(body, token, observation);
            });
    }, [&] {
        if (mode != 1) return;
        final_body_read_parked = observe([&] {
            return final_header_handler.load(std::memory_order_acquire) && body_read_parked(fixture);
        });
        release_final_body.set();
    });
    REQUIRE(result_error(result) == 0);
    REQUIRE(expect_seen);
    if (mode != 2) REQUIRE(no_early_upload);
    REQUIRE(uploaded == (mode == 1 ? "" : "payload"));
    REQUIRE(status == (mode == 1 ? 417 : 200));
    REQUIRE(observation.bytes == (mode == 1 ? "no" : "ok"));
    if (mode == 1) REQUIRE(final_body_read_parked);
}

TEST_CASE("HTTP streaming operation owns lazy inputs and follows redirects before callback",
          "[http][client][streaming][http_client_streaming][redirect][lifetime]") {
    const auto code = GENERATE(302, 303, 307, 308);
    exchange_fixture fixture(backend_type::epoll, false);
    std::array<std::string, 2> requests;
    body_observation observation;
    size_t callbacks = 0;
    bool owned_handler = false;
    http::client_result<std::monostate> result;
    fixture.run([&]() -> coro::task<void> {
        for (size_t i = 0; i < 2; ++i) {
            auto stream = co_await fixture.accept();
            if (!stream) throw std::runtime_error("server accept failed");
            requests[i] = co_await request_headers(*stream, fixture.stop.get_token());
            const auto end = requests[i].find("\r\n\r\n") + 4;
            if (i == 0 || code >= 307) {
                std::array<char, 16> buffer;
                while (requests[i].size() < end + 7) {
                    auto read = co_await stream->read(buffer.data(), buffer.size(), fixture.stop.get_token());
                    if (read.result <= 0) break;
                    requests[i].append(buffer.data(), static_cast<size_t>(read.result));
                }
            }
            if (i == 0) {
                const auto response = "HTTP/1.1 " + std::to_string(code) +
                    " Redirect\r\nLocation: /final\r\nContent-Length: 999999\r\n\r\n";
                (void)co_await stream->write_all(response, fixture.stop.get_token());
                char byte;
                // A streaming redirect does not drain the advertised large body.
                (void)co_await stream->read(&byte, 1, fixture.stop.get_token());
            } else {
                (void)co_await stream->write_all(
                    "HTTP/1.1 200 OK\r\nContent-Length: 2\r\nConnection: close\r\n\r\nok",
                    fixture.stop.get_token());
            }
            http::detail::abort_stream_io(*stream);
        }
    }, [&](http::client& client) -> coro::task<void> {
        http::request req(http::method::POST, "/range");
        req.set_body(std::string_view("payload"));
        auto target = fixture.target;
        auto operation = client.with_response(req, target, {},
            [&, owned = std::make_unique<std::string>("owned handler")](const http::response&,
                http::response_body_reader& body, coro::cancel_token token) -> coro::task<void> {
                ++callbacks;
                co_await consume(body, token, observation);
                owned_handler = *owned == "owned handler";
            });
        req.set_body(std::string_view("different"));
        target.scheme = "unsupported";
        result = co_await operation;
    });
    REQUIRE(result_error(result) == 0);
    REQUIRE(fixture.accepted == 2);
    REQUIRE(callbacks == 1);
    REQUIRE(owned_handler);
    REQUIRE(requests[0].starts_with("POST /range "));
    REQUIRE(requests[0].ends_with("payload"));
    REQUIRE(requests[1].starts_with(code >= 307 ? "POST /final " : "GET /final "));
    REQUIRE(requests[1].ends_with(code >= 307 ? "payload" : "\r\n\r\n"));
    REQUIRE(observation.bytes == "ok");
}

TEST_CASE("HTTP streaming watchdog exceptions settle a pending read before propagation",
          "[http][client][streaming][http_client_streaming][exception][watchdog]") {
    const auto backend = GENERATE(backend_type::epoll, backend_type::io_uring);
    require_backend(backend);
    const bool encrypted = GENERATE(false, true);
    const bool swallow = GENERATE(false, true);
    CAPTURE(static_cast<int>(backend), encrypted, swallow);
    exchange_fixture fixture(backend, encrypted);
    response_observer_guard observer;
    sync::event fail_watchdog;
    watchdog_failure_guard fault(fail_watchdog);
    sync::event read_returned;
    sync::event late_write_finished;
    std::atomic<bool> body_started{false};
    std::atomic<bool> body_returned{false};
    bool parked = false;
    bool returned_without_root_cancel = false;
    bool exception_preserved = false;
    bool buffer_reused = false;
    bool outer_exception = false;
    http::client_result<std::monostate> result;
    fixture.run([&]() -> coro::task<void> {
        auto stream = co_await fixture.accept();
        if (!stream) throw std::runtime_error("server accept failed");
        (void)co_await request_headers(*stream, fixture.stop.get_token());
        (void)co_await stream->write_all("HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\n",
                                        fixture.stop.get_token());
        (void)co_await read_returned.wait(fixture.stop.get_token());
        (void)co_await stream->write_all("late!", fixture.stop.get_token());
        late_write_finished.set();
        char byte;
        (void)co_await stream->read(&byte, 1, fixture.stop.get_token());
        http::detail::abort_stream_io(*stream);
    }, [&](http::client& client) -> coro::task<void> {
        try {
            result = co_await client.with_response(http::request(http::method::GET, "/range"),
                fixture.target, fixture.stop.get_token(),
                [&](const http::response&, http::response_body_reader& body,
                    coro::cancel_token token) -> coro::task<void> {
                    std::array<char, 5> buffer{};
                    std::exception_ptr failure;
                    body_started.store(true, std::memory_order_release);
                    try {
                        (void)co_await body.read_into(std::span<char>(buffer), token);
                    } catch (const std::runtime_error& error) {
                        exception_preserved = std::string(error.what()) ==
                            "injected response watchdog failure";
                        failure = std::current_exception();
                    }
                    buffer.fill('r');
                    body_returned.store(true, std::memory_order_release);
                    read_returned.set();
                    (void)co_await late_write_finished.wait(fixture.stop.get_token());
                    buffer_reused = std::all_of(buffer.begin(), buffer.end(),
                                                [](char value) { return value == 'r'; });
                    if (!swallow && failure) std::rethrow_exception(failure);
                });
        } catch (const std::runtime_error& error) {
            outer_exception = std::string(error.what()) == "injected response watchdog failure";
        }
    }, [&] {
        // The controlled watchdog is waiting on an event, not a timer: only
        // the body transport contributes a pending I/O operation here.
        parked = observe([&] {
            return body_started.load(std::memory_order_acquire) &&
                http::detail::response_read_stage_for_test.load() == http::client_stage::body &&
                fixture.sched.get_worker(0)->io_context().pending_count() +
                    fixture.sched.get_worker(1)->io_context().pending_count() >= 1;
        });
        fail_watchdog.set();
        returned_without_root_cancel = observe([&] {
            return body_returned.load(std::memory_order_acquire);
        });
        if (!returned_without_root_cancel) fixture.stop.cancel();
    });
    REQUIRE(parked);
    REQUIRE(returned_without_root_cancel);
    REQUIRE(exception_preserved);
    REQUIRE(buffer_reused);
    REQUIRE(outer_exception == !swallow);
    if (swallow) {
        REQUIRE(result_error(result) == EIO);
        REQUIRE(std::get<http::client_error>(result).stage == http::client_stage::body);
    }
}
