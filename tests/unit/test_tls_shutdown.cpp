#include <catch2/catch_test_macros.hpp>

#if defined(ELIO_HAS_TLS) && ELIO_HAS_TLS && defined(ELIO_RUNTIME_TEST_HOOKS)
#include <elio/tls/tls_stream.hpp>
#include <elio/runtime/scheduler.hpp>
#include <elio/sync/event.hpp>
#include <openssl/evp.h>
#include <openssl/rsa.h>
#include <openssl/x509.h>
#include "../test_main.cpp"

#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <exception>
#include <memory>
#include <new>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <utility>

namespace {
using namespace elio;
using backend_type = io::io_context::backend_type;

class shutdown_backend_guard {
public:
    explicit shutdown_backend_guard(backend_type backend)
        : previous_(runtime::detail::worker_io_backend_for_test.exchange(backend)) {}
    ~shutdown_backend_guard() { runtime::detail::worker_io_backend_for_test.store(previous_); }
private:
    backend_type previous_;
};

template<typename Predicate>
bool await_shutdown_observation(Predicate predicate) {
    const auto deadline = std::chrono::steady_clock::now() + test::scaled_ms(15000);
    while (!predicate()) {
        if (std::chrono::steady_clock::now() >= deadline) return false;
        std::this_thread::yield();
    }
    return true;
}

bool shutdown_certificate(tls::tls_context& context) {
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

// Force the real close_notify into the owned ciphertext queue once; its pump
// still sends the actual TLS bytes through the native socket. No SSL result is
// scripted, and this is not claimed as natural socket-capacity reproduction.
struct shutdown_output_observer {
    unsigned sends = 0;
    unsigned payload_allocations = 0;
    static ssize_t send(void* context, int fd, const void* bytes, size_t size, int flags) {
        auto& self = *static_cast<shutdown_output_observer*>(context);
        if (self.sends++ == 0) { errno = EAGAIN; return -1; }
        return ::send(fd, bytes, size, flags);
    }
    static void allocate(void* context, bool payload) noexcept {
        if (payload) ++static_cast<shutdown_output_observer*>(context)->payload_allocations;
    }
};

void exercise_shutdown(backend_type backend, tls::tls_version version, bool peer_closes,
                       int timer_failure = 0) {
    CAPTURE(backend, version, peer_closes, timer_failure);
    shutdown_backend_guard backend_scope(backend);
    std::array<int, 2> sockets{-1, -1};
    REQUIRE(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sockets.data()) == 0);
    net::tcp_stream server_tcp(sockets[0]);
    net::tcp_stream peer_tcp(sockets[1]);
    tls::tls_context server_context(tls::tls_mode::server, version);
    tls::tls_context peer_context(tls::tls_mode::client, version);
    REQUIRE(shutdown_certificate(server_context));
    peer_context.set_verify_mode(tls::verify_mode::none);
    tls::tls_stream server(std::move(server_tcp), server_context);
    tls::tls_stream peer(std::move(peer_tcp), peer_context);
    runtime::scheduler scheduler(2);
    scheduler.start();
    coro::cancel_source cancel;
    std::array<bool, 2> handshakes{};
    std::atomic<unsigned> handshake_done{0};
    scheduler.go([&]() -> coro::task<void> {
        try { handshakes[0] = co_await server.handshake(cancel.get_token()); } catch (...) {}
        handshake_done.fetch_add(1, std::memory_order_release);
    });
    scheduler.go([&]() -> coro::task<void> {
        try { handshakes[1] = co_await peer.handshake(cancel.get_token()); } catch (...) {}
        handshake_done.fetch_add(1, std::memory_order_release);
    });
    const bool handshake_completed = await_shutdown_observation([&] {
        return handshake_done.load(std::memory_order_acquire) == 2;
    });
    if (!handshake_completed || !handshakes[0] || !handshakes[1]) {
        cancel.cancel();
        server.shutdown_socket();
        peer.shutdown_socket();
        if (!scheduler.shutdown(test::scaled_ms(15000))) std::terminate();
        REQUIRE(handshake_completed);
        REQUIRE(handshakes[0]);
        REQUIRE(handshakes[1]);
        return;
    }
    const bool negotiated = std::string_view(server.version()) ==
        (version == tls::tls_version::tls_1_2 ? "TLSv1.2" : "TLSv1.3") &&
        std::string_view(peer.version()) == std::string_view(server.version());

    shutdown_output_observer output_observer;
    server.set_output_test_hooks({&output_observer, shutdown_output_observer::send,
                                  shutdown_output_observer::allocate});
    if (timer_failure == 1) {
        server.set_shutdown_timer_test_hook(+[] { throw std::bad_alloc(); });
    } else if (timer_failure == 2) {
        server.set_shutdown_timer_test_hook(+[] { throw std::runtime_error("shutdown timer setup"); });
    }
    sync::event start;
    std::atomic<bool> joined{false};
    std::array<bool, 2> close_returned{};
    bool launch_failed = false;
    bool close_threw = false;
    scheduler.go([&]() -> coro::task<void> {
        std::array<std::optional<coro::join_handle<void>>, 2> tasks;
        try {
            tasks[0].emplace(scheduler.go_joinable([&]() -> coro::task<void> {
                co_await start.wait();
                if (peer_closes) co_await server.shutdown();
                else co_await server.shutdown(test::scaled_ms(1000));
                close_returned[0] = true;
            }));
            if (peer_closes) {
                tasks[1].emplace(scheduler.go_joinable([&]() -> coro::task<void> {
                    co_await start.wait();
                    co_await peer.shutdown();
                    close_returned[1] = true;
                }));
            }
        } catch (...) {
            launch_failed = true;
            server.shutdown_socket();
            peer.shutdown_socket();
        }
        start.set();
        for (auto& task : tasks) {
            if (task) {
                try { co_await std::move(*task); } catch (...) { close_threw = true; }
            }
        }
        joined.store(true, std::memory_order_release);
    });
    // A withheld peer never runs read/shutdown after handshake: the close
    // deadline itself, not a timed peer sleep, must stop the server operation.
    const bool completed = await_shutdown_observation([&] { return joined.load(std::memory_order_acquire); });
    if (!completed) {
        server.shutdown_socket();
        peer.shutdown_socket();
    }
    if (!scheduler.shutdown(test::scaled_ms(15000))) std::terminate();
    server.set_output_test_hooks({});
    server.set_shutdown_timer_test_hook(nullptr);
    const auto server_state = server.shutdown_state_for_test();
    const auto peer_state = peer.shutdown_state_for_test();
    const bool server_handshake_reset = !server.is_handshake_complete();
    const bool peer_handshake_reset = !peer.is_handshake_complete();
    server.shutdown_socket();
    peer.shutdown_socket();

    REQUIRE(negotiated);
    REQUIRE(completed);
    REQUIRE(joined.load(std::memory_order_acquire));
    REQUIRE_FALSE(launch_failed);
    REQUIRE_FALSE(close_threw);
    REQUIRE(close_returned[0]);
    REQUIRE(server_handshake_reset);
    REQUIRE_FALSE(server_state.pump_active);
    REQUIRE_FALSE(peer_state.pump_active);
    if (!timer_failure) {
        REQUIRE(output_observer.payload_allocations > 0);
        REQUIRE((server_state.ssl_shutdown_flags & SSL_SENT_SHUTDOWN) != 0);
    }
    if (peer_closes) {
        REQUIRE(close_returned[1]);
        REQUIRE(peer_handshake_reset);
        REQUIRE(server_state.transport_error == 0);
        REQUIRE(peer_state.transport_error == 0);
        REQUIRE((server_state.ssl_shutdown_flags & SSL_RECEIVED_SHUTDOWN) != 0);
        REQUIRE((peer_state.ssl_shutdown_flags & SSL_SENT_SHUTDOWN) != 0);
        REQUIRE((peer_state.ssl_shutdown_flags & SSL_RECEIVED_SHUTDOWN) != 0);
    } else {
        const int expected_error = timer_failure == 1 ? ENOMEM : timer_failure == 2 ? EIO : ETIMEDOUT;
        REQUIRE(server_state.transport_error == expected_error);
        REQUIRE((server_state.ssl_shutdown_flags & SSL_RECEIVED_SHUTDOWN) == 0);
        REQUIRE_FALSE(close_returned[1]);
        REQUIRE_FALSE(peer_handshake_reset);
    }
}

void shutdown_versions(backend_type backend, bool peer_closes, int timer_failure = 0) {
    SECTION("TLS 1.2") { exercise_shutdown(backend, tls::tls_version::tls_1_2, peer_closes, timer_failure); }
    SECTION("TLS 1.3") { exercise_shutdown(backend, tls::tls_version::tls_1_3, peer_closes, timer_failure); }
}
}

TEST_CASE("TLS whole-session shutdown exchanges both alerts and joins output", "[tls][shutdown][issue-1215]") {
    SECTION("forced epoll") { shutdown_versions(backend_type::epoll, true); }
    SECTION("forced io_uring") {
#if ELIO_HAS_IO_URING
        if (!io::io_uring_backend::is_available()) SKIP("io_uring unavailable on this host");
        shutdown_versions(backend_type::io_uring, true);
#else
        SKIP("io_uring support is not compiled");
#endif
    }
}

TEST_CASE("TLS whole-session shutdown times out a peer withholding close", "[tls][shutdown][timeout][issue-1215]") {
    SECTION("forced epoll") { shutdown_versions(backend_type::epoll, false); }
    SECTION("forced io_uring") {
#if ELIO_HAS_IO_URING
        if (!io::io_uring_backend::is_available()) SKIP("io_uring unavailable on this host");
        shutdown_versions(backend_type::io_uring, false);
#else
        SKIP("io_uring support is not compiled");
#endif
    }
}

TEST_CASE("TLS shutdown contains timer setup failure and settles output", "[tls][shutdown][failure][issue-1215]") {
    auto failures = [](backend_type backend) {
        SECTION("allocation failure") { shutdown_versions(backend, false, 1); }
        SECTION("other timer failure") { shutdown_versions(backend, false, 2); }
    };
    SECTION("forced epoll") { failures(backend_type::epoll); }
    SECTION("forced io_uring") {
#if ELIO_HAS_IO_URING
        if (!io::io_uring_backend::is_available()) SKIP("io_uring unavailable on this host");
        failures(backend_type::io_uring);
#else
        SKIP("io_uring support is not compiled");
#endif
    }
}
#endif
