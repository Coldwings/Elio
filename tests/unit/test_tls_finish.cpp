#include <catch2/catch_test_macros.hpp>

#if defined(ELIO_HAS_TLS) && ELIO_HAS_TLS && defined(ELIO_RUNTIME_TEST_HOOKS)
#include <elio/tls/tls_stream.hpp>
#include <elio/net/stream.hpp>
#include <elio/runtime/scheduler.hpp>
#include <elio/sync/event.hpp>
#include <openssl/evp.h>
#include <openssl/rsa.h>
#include <openssl/x509.h>
#include "../test_main.cpp"

#include <array>
#include <atomic>
#include <chrono>
#include <memory>
#include <optional>
#include <stdexcept>
#include <thread>

namespace {
using namespace elio;
using backend_type = io::io_context::backend_type;

template<class Predicate>
bool finish_observe(Predicate predicate) {
    const auto deadline = std::chrono::steady_clock::now() + test::scaled_ms(15000);
    while (!predicate()) {
        if (std::chrono::steady_clock::now() >= deadline) return false;
        std::this_thread::yield();
    }
    return true;
}

bool finish_certificate(tls::tls_context& context) {
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
        SSL_CTX_use_PrivateKey(context.native_handle(), key.get()) == 1;
}

struct finish_backend_guard {
    backend_type previous;
    explicit finish_backend_guard(backend_type backend)
        : previous(runtime::detail::worker_io_backend_for_test.exchange(backend)) {}
    ~finish_backend_guard() { runtime::detail::worker_io_backend_for_test.store(previous); }
};

struct finish_fixture {
    finish_backend_guard backend_guard;
    tls::tls_context server_context;
    tls::tls_context client_context;
    std::optional<tls::tls_stream> server;
    std::optional<tls::tls_stream> client;
    runtime::scheduler scheduler{2};
    coro::cancel_source cancel;

    finish_fixture(backend_type backend, tls::tls_version version,
                   size_t server_budget = 1024 * 1024)
        : backend_guard(backend)
        , server_context(tls::tls_mode::server, version)
        , client_context(tls::tls_mode::client, version) {
        REQUIRE(finish_certificate(server_context));
        client_context.set_verify_mode(tls::verify_mode::none);
        std::array<int, 2> sockets{-1, -1};
        REQUIRE(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sockets.data()) == 0);
        server.emplace(net::tcp_stream(sockets[0]), server_context,
                       tls::tls_stream_options{server_budget});
        client.emplace(net::tcp_stream(sockets[1]), client_context);
        scheduler.start();
        std::array<bool, 2> handshakes{};
        std::atomic<unsigned> done{0};
        scheduler.go([&]() -> coro::task<void> {
            handshakes[0] = co_await server->handshake(cancel.get_token());
            done.fetch_add(1, std::memory_order_release);
        });
        scheduler.go([&]() -> coro::task<void> {
            handshakes[1] = co_await client->handshake(cancel.get_token());
            done.fetch_add(1, std::memory_order_release);
        });
        const bool completed = finish_observe([&] { return done.load(std::memory_order_acquire) == 2; });
        if (!completed || !handshakes[0] || !handshakes[1]) {
            abort();
            if (!scheduler.shutdown(test::scaled_ms(15000))) std::terminate();
        }
        REQUIRE(completed);
        REQUIRE(handshakes[0]);
        REQUIRE(handshakes[1]);
    }

    ~finish_fixture() {
        abort();
        if (!scheduler.shutdown(test::scaled_ms(15000))) std::terminate();
    }

    void abort() {
        cancel.cancel();
        server->shutdown_socket();
        client->shutdown_socket();
    }

    template<class Function>
    void run(Function work) {
        std::atomic<bool> done{false};
        bool threw = false;
        scheduler.go([&]() -> coro::task<void> {
            try { co_await work(); } catch (...) { threw = true; }
            done.store(true, std::memory_order_release);
        });
        const bool completed = finish_observe([&] { return done.load(std::memory_order_acquire); });
        if (!completed) {
            abort();
            if (!scheduler.shutdown(test::scaled_ms(15000))) std::terminate();
        }
        REQUIRE(completed);
        REQUIRE_FALSE(threw);
    }
};

template<class Function>
void finish_backends(Function work) {
    SECTION("forced epoll") { work(backend_type::epoll); }
    SECTION("forced io_uring") {
#if ELIO_HAS_IO_URING
        if (!io::io_uring_backend::is_available()) SKIP("io_uring unavailable on this host");
        work(backend_type::io_uring);
#else
        SKIP("io_uring support is not compiled");
#endif
    }
}

template<class Function>
void finish_versions(Function work) {
    SECTION("TLS 1.2") { work(tls::tls_version::tls_1_2); }
    SECTION("TLS 1.3") { work(tls::tls_version::tls_1_3); }
}
}

TEST_CASE("TLS 1.3 write finish retains the reverse reader without a close timer",
          "[tls][finish][issue-1217]") {
    finish_backends([](backend_type backend) {
        finish_fixture fixture(backend, tls::tls_version::tls_1_3);
        auto exercise = [&](bool client_first) {
            auto& first = client_first ? *fixture.client : *fixture.server;
            auto& other = client_first ? *fixture.server : *fixture.client;
            first.set_shutdown_timer_test_hook(+[] { throw std::runtime_error("unexpected half-close timer"); });
            net::write_finish_result finished, peer_finished, repeated;
            std::array<io::io_result, 5> results{};
            char reverse = 0;
            fixture.run([&]() -> coro::task<void> {
                std::optional<coro::join_handle<void>> reader;
                auto read_reverse = [&]() -> coro::task<void> {
                    results[0] = co_await first.read(&reverse, 1, fixture.cancel.get_token());
                };
                try {
                    reader.emplace(elio::spawn(read_reverse()));
                    // A zero session timeout and a failing timer hook must have
                    // no effect on independent TLS 1.3 write closure.
                    finished = co_await first.finish_write(fixture.cancel.get_token(), std::chrono::milliseconds(0));
                    repeated = co_await first.finish_write();
                    char byte = 0;
                    results[1] = co_await other.read(&byte, 1, fixture.cancel.get_token());
                    results[2] = co_await first.write("x", 1, fixture.cancel.get_token());
                    results[3] = co_await other.write("r", 1, fixture.cancel.get_token());
                    peer_finished = co_await other.finish_write(fixture.cancel.get_token());
                } catch (...) { fixture.abort(); }
                if (reader) co_await std::move(*reader);
                char byte = 0;
                results[4] = co_await first.read(&byte, 1, fixture.cancel.get_token());
            });
            REQUIRE(finished.scope == net::close_scope::write_direction);
            REQUIRE(finished.error == 0);
            REQUIRE(finished.local_end_flushed);
            REQUIRE_FALSE(finished.peer_end_observed);
            REQUIRE(repeated.error == 0);
            REQUIRE(repeated.local_end_flushed);
            REQUIRE(peer_finished.error == 0);
            REQUIRE(peer_finished.peer_end_observed);
            REQUIRE(results[0].result == 1);
            REQUIRE(reverse == 'r');
            REQUIRE(results[1].result == 0);
            REQUIRE(results[2].result == -ESHUTDOWN);
            REQUIRE(results[3].result == 1);
            REQUIRE(results[4].result == 0);
            REQUIRE(first.is_handshake_complete());
        };
        SECTION("client EOF first") { exercise(true); }
        SECTION("server EOF first") { exercise(false); }
    });
}

TEST_CASE("TLS 1.3 tiny reads retain the payload tail across peer finish and reverse writes",
          "[tls][finish][issue-1217]") {
    finish_backends([](backend_type backend) {
        finish_fixture fixture(backend, tls::tls_version::tls_1_3);
        std::array<char, 257> payload{};
        for (size_t i = 0; i < payload.size(); ++i)
            payload[i] = static_cast<char>(i % 127);
        std::array<char, 257> received{};
        std::array<char, 2> reverse{};
        sync::event peer_finished;
        net::write_finish_result peer_end, local_end;
        io::io_result first_read{}, local_eof{}, peer_eof{};
        std::array<io::io_result, 2> reverse_writes{};
        size_t sent = 0, consumed = 0, reverse_consumed = 0;
        bool peer_threw = false, local_threw = false;
        fixture.run([&]() -> coro::task<void> {
            std::optional<coro::join_handle<void>> peer;
            auto peer_work = [&]() -> coro::task<void> {
                try {
                    while (sent < payload.size()) {
                        const auto result = co_await fixture.client->write(
                            payload.data() + sent, payload.size() - sent,
                            fixture.cancel.get_token());
                        if (result.result <= 0) { fixture.abort(); break; }
                        sent += static_cast<size_t>(result.result);
                    }
                    peer_end = co_await fixture.client->finish_write(fixture.cancel.get_token());
                    // Both application ciphertext and the real close_notify
                    // have flushed before the receiver starts its tiny reads.
                    peer_finished.set();
                    while (reverse_consumed < reverse.size()) {
                        const auto result = co_await fixture.client->read(
                            reverse.data() + reverse_consumed, 1, fixture.cancel.get_token());
                        if (result.result <= 0) { fixture.abort(); break; }
                        reverse_consumed += static_cast<size_t>(result.result);
                    }
                    char extra = 0;
                    peer_eof = co_await fixture.client->read(&extra, 1, fixture.cancel.get_token());
                } catch (...) {
                    peer_threw = true;
                    fixture.abort();
                    peer_finished.set();
                }
            };
            try {
                peer.emplace(elio::spawn(peer_work()));
                const auto ready = co_await peer_finished.wait(fixture.cancel.get_token());
                if (ready == coro::cancel_result::cancelled) {
                    fixture.abort();
                } else {
                    first_read = co_await fixture.server->read(
                        received.data(), 1, fixture.cancel.get_token());
                    if (first_read.result > 0) consumed = static_cast<size_t>(first_read.result);
                    // Interleave a local SSL_write while the peer's payload
                    // remains only partly consumed, then write again after EOF.
                    reverse_writes[0] = co_await fixture.server->write(
                        "a", 1, fixture.cancel.get_token());
                    while (consumed < received.size()) {
                        const auto result = co_await fixture.server->read(
                            received.data() + consumed, 1, fixture.cancel.get_token());
                        if (result.result <= 0) { fixture.abort(); break; }
                        consumed += static_cast<size_t>(result.result);
                    }
                    char extra = 0;
                    local_eof = co_await fixture.server->read(&extra, 1, fixture.cancel.get_token());
                    reverse_writes[1] = co_await fixture.server->write(
                        "b", 1, fixture.cancel.get_token());
                    local_end = co_await fixture.server->finish_write(fixture.cancel.get_token());
                }
            } catch (...) {
                local_threw = true;
                fixture.abort();
            }
            if (peer) co_await std::move(*peer);
        });
        REQUIRE_FALSE(peer_threw);
        REQUIRE_FALSE(local_threw);
        REQUIRE(sent == payload.size());
        REQUIRE(peer_end.error == 0);
        REQUIRE(peer_end.scope == net::close_scope::write_direction);
        REQUIRE(peer_end.local_end_flushed);
        REQUIRE(first_read.result == 1);
        REQUIRE(consumed == payload.size());
        REQUIRE(received == payload);
        REQUIRE(local_eof.result == 0);
        REQUIRE(reverse_writes[0].result == 1);
        REQUIRE(reverse_writes[1].result == 1);
        REQUIRE(reverse_consumed == reverse.size());
        REQUIRE(reverse[0] == 'a');
        REQUIRE(reverse[1] == 'b');
        REQUIRE(local_end.error == 0);
        REQUIRE(local_end.scope == net::close_scope::write_direction);
        REQUIRE(local_end.local_end_flushed);
        REQUIRE(local_end.peer_end_observed);
        REQUIRE(peer_eof.result == 0);
    });
}

TEST_CASE("TLS 1.2 write finish and concurrent read share automatic session closure",
          "[tls][finish][issue-1217]") {
    finish_backends([](backend_type backend) {
        finish_fixture fixture(backend, tls::tls_version::tls_1_2);
        auto exercise = [&](bool client_first) {
            auto& first = client_first ? *fixture.client : *fixture.server;
            auto& other = client_first ? *fixture.server : *fixture.client;
            net::write_finish_result finished, repeated;
            std::array<io::io_result, 4> results{};
            fixture.run([&]() -> coro::task<void> {
                std::array<std::optional<coro::join_handle<void>>, 2> readers;
                auto read_first = [&]() -> coro::task<void> {
                    char byte = 0;
                    results[0] = co_await first.read(&byte, 1, fixture.cancel.get_token());
                };
                auto read_other = [&]() -> coro::task<void> {
                    char byte = 0;
                    // No explicit peer finish: read EOF must respond.
                    results[1] = co_await other.read(&byte, 1, fixture.cancel.get_token());
                };
                try {
                    readers[0].emplace(elio::spawn(read_first()));
                    readers[1].emplace(elio::spawn(read_other()));
                    finished = co_await first.finish_write(fixture.cancel.get_token());
                } catch (...) { fixture.abort(); }
                for (auto& reader : readers) if (reader) co_await std::move(*reader);
                repeated = co_await other.finish_write();
                results[2] = co_await first.write("x", 1);
                results[3] = co_await other.write("y", 1);
            });
            REQUIRE(finished.scope == net::close_scope::whole_session);
            REQUIRE(finished.error == 0);
            REQUIRE(finished.local_end_flushed);
            REQUIRE(finished.peer_end_observed);
            REQUIRE(repeated.scope == net::close_scope::whole_session);
            REQUIRE(repeated.error == 0);
            REQUIRE(repeated.local_end_flushed);
            REQUIRE(repeated.peer_end_observed);
            REQUIRE(results[0].result == 0);
            REQUIRE(results[1].result == 0);
            REQUIRE(results[2].result == -ESHUTDOWN);
            REQUIRE(results[3].result == -ESHUTDOWN);
            REQUIRE_FALSE(first.shutdown_state_for_test().pump_active);
            REQUIRE_FALSE(other.shutdown_state_for_test().pump_active);
        };
        SECTION("client EOF first") { exercise(true); }
        SECTION("server EOF first") { exercise(false); }
    });
}

TEST_CASE("TLS write finish pre-cancellation is local and raw TCP EOF is not authenticated",
          "[tls][finish][cancel][issue-1217]") {
    finish_backends([](backend_type backend) {
        finish_versions([&](tls::tls_version version) {
            finish_fixture fixture(backend, version);
            net::write_finish_result finished;
            std::array<io::io_result, 3> results{};
            char received = 0;
            fixture.run([&]() -> coro::task<void> {
                coro::cancel_source cancelled;
                cancelled.cancel();
                finished = co_await fixture.server->finish_write(cancelled.get_token());
                results[0] = co_await fixture.server->write("v", 1);
                results[1] = co_await fixture.client->read(&received, 1);
                fixture.client->shutdown_socket();
                results[2] = co_await fixture.server->read(&received, 1);
            });
            REQUIRE(finished.error == ECANCELED);
            REQUIRE_FALSE(finished.local_end_flushed);
            REQUIRE(results[0].result == 1);
            REQUIRE(results[1].result == 1);
            REQUIRE(received == 'v');
            REQUIRE(results[2].result < 0);
            REQUIRE((fixture.server->shutdown_state_for_test().ssl_shutdown_flags & SSL_RECEIVED_SHUTDOWN) == 0);
        });
    });
}

TEST_CASE("TLS 1.2 write finish reports a withheld peer close timeout",
          "[tls][finish][timeout][issue-1217]") {
    finish_backends([](backend_type backend) {
        finish_fixture fixture(backend, tls::tls_version::tls_1_2);
        net::write_finish_result finished;
        fixture.run([&]() -> coro::task<void> {
            finished = co_await fixture.server->finish_write(
                fixture.cancel.get_token(), test::scaled_ms(20));
        });
        REQUIRE(finished.scope == net::close_scope::whole_session);
        REQUIRE(finished.error == ETIMEDOUT);
        REQUIRE(finished.local_end_flushed);
        REQUIRE_FALSE(finished.peer_end_observed);
        REQUIRE_FALSE(fixture.server->shutdown_state_for_test().pump_active);
    });
}

TEST_CASE("TLS 1.2 extreme finish deadlines saturate before cancellation",
          "[tls][finish][timeout][issue-1217]") {
    finish_backends([](backend_type backend) {
        SECTION("maximum duration remains pending until explicitly cancelled") {
            finish_fixture fixture(backend, tls::tls_version::tls_1_2);
            coro::cancel_source close_cancel;
            net::write_finish_result finished;
            std::atomic<bool> close_done{false};
            std::atomic<int64_t> requested_timer_ns{0};
            fixture.server->set_shutdown_timer_duration_test_hook(&requested_timer_ns,
                [](void* context, std::chrono::steady_clock::duration interval) {
                    static_cast<std::atomic<int64_t>*>(context)->store(
                        std::chrono::duration_cast<std::chrono::nanoseconds>(interval).count(),
                        std::memory_order_release);
                });
            bool alert_observed = false;
            bool pending_before_cancel = false;
            fixture.run([&]() -> coro::task<void> {
                std::optional<coro::join_handle<void>> child;
                auto close = [&]() -> coro::task<void> {
                    finished = co_await fixture.server->finish_write(
                        close_cancel.get_token(), std::chrono::milliseconds::max());
                    close_done.store(true, std::memory_order_release);
                };
                try {
                    child.emplace(elio::spawn(close()));
                    // The peer deliberately performs no reads or close. Observe
                    // real SSL progress past the deadline check, not elapsed
                    // wall time, before issuing our separate cancellation.
                    while (!close_done.load(std::memory_order_acquire) &&
                           !fixture.cancel.is_cancelled()) {
                        const auto state = fixture.server->shutdown_state_for_test();
                        if ((state.ssl_shutdown_flags & SSL_SENT_SHUTDOWN) != 0 &&
                            requested_timer_ns.load(std::memory_order_acquire) > 0) {
                            alert_observed = true;
                            pending_before_cancel = !close_done.load(std::memory_order_acquire);
                            break;
                        }
                        co_await time::yield();
                    }
                    close_cancel.cancel();
                } catch (...) { fixture.abort(); }
                if (child) co_await std::move(*child);
            });
            REQUIRE(alert_observed);
            REQUIRE(pending_before_cancel);
            // The observer sees the actual interval passed to sleep_for. The
            // old deadline-now interval is near the clock limit, not one hour.
            REQUIRE(requested_timer_ns.load() > 0);
            REQUIRE(requested_timer_ns.load() <=
                std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::hours(1)).count());
            REQUIRE(finished.scope == net::close_scope::whole_session);
            REQUIRE(finished.error == ECANCELED);
            REQUIRE_FALSE(finished.peer_end_observed);
            REQUIRE_FALSE(fixture.cancel.is_cancelled());
            REQUIRE_FALSE(fixture.server->shutdown_state_for_test().pump_active);
        }
        SECTION("minimum negative duration normalizes to immediate expiry") {
            finish_fixture fixture(backend, tls::tls_version::tls_1_2);
            net::write_finish_result finished;
            fixture.run([&]() -> coro::task<void> {
                finished = co_await fixture.server->finish_write(
                    fixture.cancel.get_token(), std::chrono::milliseconds::min());
            });
            REQUIRE(finished.scope == net::close_scope::whole_session);
            REQUIRE(finished.error == ETIMEDOUT);
            REQUIRE_FALSE(finished.local_end_flushed);
            REQUIRE_FALSE(finished.peer_end_observed);
            const auto state = fixture.server->shutdown_state_for_test();
            REQUIRE((state.ssl_shutdown_flags & SSL_SENT_SHUTDOWN) == 0);
            REQUIRE_FALSE(state.pump_active);
        }
    });
}

TEST_CASE("TLS write finish contains close alert resource failure",
          "[tls][finish][failure][issue-1217]") {
    finish_backends([](backend_type backend) {
        finish_versions([&](tls::tls_version version) {
            int expected_error = ENOBUFS;
            SECTION("payload budget exhausted") { expected_error = ENOBUFS; }
            SECTION("payload allocation fails") { expected_error = ENOMEM; }
            finish_fixture fixture(backend, version, expected_error == ENOBUFS ? 0 : 1024 * 1024);
            fixture.server->set_output_test_hooks({nullptr,
                +[](void*, int, const void*, size_t, int) -> ssize_t { errno = EAGAIN; return -1; },
                expected_error == ENOMEM ? +[](void*, bool payload) { if (payload) throw std::bad_alloc(); } : nullptr});
            net::write_finish_result finished;
            fixture.run([&]() -> coro::task<void> {
                finished = co_await fixture.server->finish_write();
            });
            fixture.server->set_output_test_hooks({});
            REQUIRE(finished.error == expected_error);
            REQUIRE_FALSE(finished.local_end_flushed);
            REQUIRE_FALSE(fixture.server->shutdown_state_for_test().pump_active);
        });
    });
}

TEST_CASE("TLS 1.2 peer closure never replaces an unfinished SSL write retry",
          "[tls][finish][retry][issue-1217]") {
    finish_backends([](backend_type backend) {
        finish_fixture fixture(backend, tls::tls_version::tls_1_2);
        struct retry_state {
            sync::event parked;
            sync::event release;
            unsigned writes = 0;
            unsigned reads = 0;
        } script;
        tls::detail::tls_dispatch_test_hooks hooks;
        hooks.context = &script;
        hooks.dispatch = +[](void* context, tls::detail::tls_test_operation operation,
                              const void*, size_t) noexcept -> tls::detail::tls_test_call_result {
            auto& state = *static_cast<retry_state*>(context);
            if (operation == tls::detail::tls_test_operation::write) {
                ++state.writes;
                return {-1, SSL_ERROR_WANT_READ};
            }
            ++state.reads;
            return {0, SSL_ERROR_ZERO_RETURN};
        };
        hooks.readiness = +[](void* context, tls::detail::tls_test_operation, bool,
                              coro::cancel_token token) -> coro::task<io::io_result> {
            auto& state = *static_cast<retry_state*>(context);
            state.parked.set();
            co_await state.release.wait(token);
            co_return io::io_result{0, 0};
        };
        fixture.server->set_dispatch_test_hooks(&hooks);
        std::array<io::io_result, 2> results{};
        fixture.run([&]() -> coro::task<void> {
            std::optional<coro::join_handle<void>> writer;
            auto write = [&]() -> coro::task<void> {
                results[0] = co_await fixture.server->write("pending", 7, fixture.cancel.get_token());
            };
            try {
                writer.emplace(elio::spawn(write()));
                co_await script.parked.wait(fixture.cancel.get_token());
                char byte = 0;
                results[1] = co_await fixture.server->read(&byte, 1, fixture.cancel.get_token());
            } catch (...) { fixture.abort(); }
            script.release.set();
            if (writer) co_await std::move(*writer);
        });
        fixture.server->set_dispatch_test_hooks(nullptr);
        REQUIRE(script.writes == 1);
        REQUIRE(script.reads == 1);
        REQUIRE(results[0].result == -ECANCELED);
        REQUIRE(results[1].result == -ECANCELED);
        REQUIRE((fixture.server->shutdown_state_for_test().ssl_shutdown_flags & SSL_SENT_SHUTDOWN) == 0);
        REQUIRE_FALSE(fixture.server->shutdown_state_for_test().pump_active);
    });
}

TEST_CASE("TLS finish result retains a later connection failure",
          "[tls][finish][failure][issue-1217]") {
    finish_backends([](backend_type backend) {
        finish_fixture fixture(backend, tls::tls_version::tls_1_3);
        net::write_finish_result first, later;
        io::io_result read;
        fixture.run([&]() -> coro::task<void> {
            first = co_await fixture.server->finish_write();
            fixture.client->shutdown_socket();
            char byte = 0;
            read = co_await fixture.server->read(&byte, 1);
            later = co_await fixture.server->finish_write();
        });
        REQUIRE(first.error == 0);
        REQUIRE(read.result < 0);
        REQUIRE(later.error == -read.result);
        REQUIRE(later.local_end_flushed);
        REQUIRE_FALSE(later.peer_end_observed);
    });
}

TEST_CASE("TLS 1.2 close cancellation settles an independently tokened reader",
          "[tls][finish][cancel][issue-1217]") {
    finish_backends([](backend_type backend) {
        finish_fixture fixture(backend, tls::tls_version::tls_1_2);
        coro::cancel_source close_cancel;
        net::write_finish_result finished;
        io::io_result read_result;
        std::atomic<bool> close_done{false};
        bool alert_observed = false;
        fixture.run([&]() -> coro::task<void> {
            std::array<std::optional<coro::join_handle<void>>, 2> children;
            auto read = [&]() -> coro::task<void> {
                char byte = 0;
                read_result = co_await fixture.server->read(&byte, 1, fixture.cancel.get_token());
            };
            auto close = [&]() -> coro::task<void> {
                finished = co_await fixture.server->finish_write(close_cancel.get_token());
                close_done.store(true, std::memory_order_release);
            };
            try {
                children[0].emplace(elio::spawn(read()));
                children[1].emplace(elio::spawn(close()));
                while (!close_done.load(std::memory_order_acquire) && !fixture.cancel.is_cancelled()) {
                    if ((fixture.server->shutdown_state_for_test().ssl_shutdown_flags & SSL_SENT_SHUTDOWN) != 0) {
                        alert_observed = true;
                        break;
                    }
                    co_await time::yield();
                }
                close_cancel.cancel();
            } catch (...) { fixture.abort(); }
            for (auto& child : children) if (child) co_await std::move(*child);
        });
        REQUIRE(alert_observed);
        REQUIRE(finished.error == ECANCELED);
        REQUIRE(read_result.result == -ECANCELED);
        REQUIRE_FALSE(fixture.cancel.is_cancelled());
        REQUIRE_FALSE(fixture.server->shutdown_state_for_test().pump_active);
    });
}

TEST_CASE("TLS-backed common stream forwards directional finish and reverse reads",
          "[tls][finish][net][issue-1217]") {
    finish_backends([](backend_type backend) {
        finish_fixture fixture(backend, tls::tls_version::tls_1_3);
        net::stream wrapped(std::move(*fixture.server));
        net::write_finish_result finished;
        std::array<io::io_result, 3> results{};
        char byte = 0;
        fixture.run([&]() -> coro::task<void> {
            finished = co_await wrapped.finish_write(fixture.cancel.get_token(), std::chrono::milliseconds(0));
            char peer_byte = 0;
            results[0] = co_await fixture.client->read(&peer_byte, 1, fixture.cancel.get_token());
            results[1] = co_await fixture.client->write("w", 1, fixture.cancel.get_token());
            results[2] = co_await wrapped.read(&byte, 1, fixture.cancel.get_token());
        });
        REQUIRE(finished.scope == net::close_scope::write_direction);
        REQUIRE(finished.error == 0);
        REQUIRE(finished.local_end_flushed);
        REQUIRE(results[0].result == 0);
        REQUIRE(results[1].result == 1);
        REQUIRE(results[2].result == 1);
        REQUIRE(byte == 'w');
    });
}
#endif
