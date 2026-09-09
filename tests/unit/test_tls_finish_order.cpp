#include <catch2/catch_test_macros.hpp>

#if defined(ELIO_HAS_TLS) && ELIO_HAS_TLS && defined(ELIO_RUNTIME_TEST_HOOKS)
#include <elio/tls/tls_stream.hpp>
#include <elio/runtime/scheduler.hpp>
#include <openssl/evp.h>
#include <openssl/rsa.h>
#include <openssl/x509.h>
#include "../test_main.cpp"
#include <array>
#include <atomic>
#include <chrono>
#include <memory>
#include <optional>
#include <string>
#include <thread>

namespace {
using namespace elio;
using backend_type = io::io_context::backend_type;

bool order_certificate(tls::tls_context& context) {
    std::unique_ptr<EVP_PKEY_CTX, decltype(&EVP_PKEY_CTX_free)> gen(
        EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, nullptr), EVP_PKEY_CTX_free);
    if (!gen || EVP_PKEY_keygen_init(gen.get()) <= 0 ||
        EVP_PKEY_CTX_set_rsa_keygen_bits(gen.get(), 2048) <= 0) return false;
    EVP_PKEY* raw = nullptr;
    if (EVP_PKEY_keygen(gen.get(), &raw) <= 0) return false;
    std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)> key(raw, EVP_PKEY_free);
    std::unique_ptr<X509, decltype(&X509_free)> cert(X509_new(), X509_free);
    if (!cert || X509_set_version(cert.get(), 2) != 1 ||
        ASN1_INTEGER_set(X509_get_serialNumber(cert.get()), 1) != 1 ||
        !X509_gmtime_adj(X509_getm_notBefore(cert.get()), 0) ||
        !X509_gmtime_adj(X509_getm_notAfter(cert.get()), 3600) ||
        X509_set_pubkey(cert.get(), key.get()) != 1) return false;
    auto* name = X509_get_subject_name(cert.get());
    return name && X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC,
        reinterpret_cast<const unsigned char*>("localhost"), -1, -1, 0) == 1 &&
        X509_set_issuer_name(cert.get(), name) == 1 &&
        X509_sign(cert.get(), key.get(), EVP_sha256()) > 0 &&
        SSL_CTX_use_certificate(context.native_handle(), cert.get()) == 1 &&
        SSL_CTX_use_PrivateKey(context.native_handle(), key.get()) == 1;
}

template<class Predicate>
bool order_wait(Predicate predicate) {
    const auto end = std::chrono::steady_clock::now() + test::scaled_ms(15000);
    while (!predicate()) {
        if (std::chrono::steady_clock::now() >= end) return false;
        std::this_thread::yield();
    }
    return true;
}

struct order_fixture {
    backend_type previous;
    tls::tls_context context{tls::tls_mode::server, tls::tls_version::tls_1_2};
    std::optional<tls::tls_stream> server;
    std::unique_ptr<SSL_CTX, decltype(&SSL_CTX_free)> peer_context{nullptr, SSL_CTX_free};
    std::unique_ptr<SSL, decltype(&SSL_free)> peer{nullptr, SSL_free};
    int peer_fd = -1;
    runtime::scheduler scheduler{2};
    coro::cancel_source cancel;
    std::string payload = std::string(16384, 'q');
    std::atomic<bool> handshake_done{false}, write_done{false}, read_done{false};
    std::atomic<bool> allocated{false}, threw{false};
    bool handshake_ok = false;
    io::io_result written{}, read_result{};
    bool joined = false;

    explicit order_fixture(backend_type backend)
        : previous(runtime::detail::worker_io_backend_for_test.exchange(backend)) {}
    ~order_fixture() {
        cleanup();
        peer.reset();
        if (peer_fd >= 0) ::close(peer_fd);
        runtime::detail::worker_io_backend_for_test.store(previous);
    }
    void cleanup() {
        if (joined) return;
        cancel.cancel();
        if (server) server->shutdown_socket();
        if (peer_fd >= 0) ::shutdown(peer_fd, SHUT_RDWR);
        if (!scheduler.shutdown(test::scaled_ms(15000))) std::terminate();
        joined = true;
    }
    coro::task<void> handshake() {
        try { handshake_ok = co_await server->handshake(cancel.get_token()); }
        catch (...) { threw.store(true, std::memory_order_release); }
        handshake_done.store(true, std::memory_order_release);
    }
    coro::task<void> write() {
        try { written = co_await server->write(payload.data(), payload.size(), cancel.get_token()); }
        catch (...) { threw.store(true, std::memory_order_release); }
        write_done.store(true, std::memory_order_release);
    }
    coro::task<void> read() {
        char byte = 0;
        try { read_result = co_await server->read(&byte, 1, cancel.get_token()); }
        catch (...) { threw.store(true, std::memory_order_release); }
        read_done.store(true, std::memory_order_release);
    }
};

void order_case(backend_type backend) {
    order_fixture f(backend);
    REQUIRE(order_certificate(f.context));
    std::array<int, 2> sockets{-1, -1};
    REQUIRE(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sockets.data()) == 0);
    f.peer_fd = sockets[1];
    net::tcp_stream server_socket(sockets[0]);
    int send_buffer = 4096;
    REQUIRE(::setsockopt(server_socket.fd(), SOL_SOCKET, SO_SNDBUF, &send_buffer, sizeof(send_buffer)) == 0);
    const timeval socket_timeout{15, 0};
    REQUIRE(::setsockopt(f.peer_fd, SOL_SOCKET, SO_RCVTIMEO, &socket_timeout, sizeof(socket_timeout)) == 0);
    REQUIRE(::setsockopt(f.peer_fd, SOL_SOCKET, SO_SNDTIMEO, &socket_timeout, sizeof(socket_timeout)) == 0);
    f.server.emplace(std::move(server_socket), f.context);
    f.peer_context.reset(SSL_CTX_new(TLS_client_method()));
    REQUIRE(f.peer_context);
    REQUIRE(SSL_CTX_set_min_proto_version(f.peer_context.get(), TLS1_2_VERSION) == 1);
    REQUIRE(SSL_CTX_set_max_proto_version(f.peer_context.get(), TLS1_2_VERSION) == 1);
    SSL_CTX_set_verify(f.peer_context.get(), SSL_VERIFY_NONE, nullptr);
    f.peer.reset(SSL_new(f.peer_context.get()));
    REQUIRE(f.peer);
    REQUIRE(SSL_set_fd(f.peer.get(), f.peer_fd) == 1);
    f.scheduler.start();
    f.scheduler.go(f.handshake());
    const int connected = SSL_connect(f.peer.get());
    const bool handshake_done = order_wait([&] { return f.handshake_done.load(std::memory_order_acquire); });
    if (connected != 1 || !handshake_done || !f.handshake_ok) {
        f.cleanup();
        REQUIRE(connected == 1);
        REQUIRE(handshake_done);
        REQUIRE(f.handshake_ok);
    }
    f.server->set_output_test_hooks({&f.allocated, nullptr,
        +[](void* context, bool payload) {
            if (payload) static_cast<std::atomic<bool>*>(context)->store(true, std::memory_order_release);
        }});
    f.scheduler.go(f.write());
    const bool allocated = order_wait([&] { return f.allocated.load(std::memory_order_acquire); });
    // Snapshot locks the BIO mutex, so the allocation callback has completed
    // and its payload is actually committed before examining the watermark.
    const auto committed = f.server->finish_state_for_test();
    const bool writer_parked = !f.write_done.load(std::memory_order_acquire);
    int close_sent = -1;
    bool alert_queued = false;
    std::string received;
    int final_error = SSL_ERROR_NONE;
    if (allocated && writer_parked) {
        close_sent = SSL_shutdown(f.peer.get());
        f.scheduler.go(f.read());
        alert_queued = order_wait([&] {
            return (f.server->shutdown_state_for_test().ssl_shutdown_flags & SSL_SENT_SHUTDOWN) != 0;
        });
        if (alert_queued) {
            std::array<char, 16384> bytes{};
            // At most one payload and one close alert; extra bytes fail too.
            for (unsigned step = 0; step < 4; ++step) {
                ERR_clear_error();
                const int count = SSL_read(f.peer.get(), bytes.data(), static_cast<int>(bytes.size()));
                if (count > 0) received.append(bytes.data(), static_cast<size_t>(count));
                else { final_error = SSL_get_error(f.peer.get(), count); break; }
            }
        }
    }
    const bool completed = order_wait([&] {
        return f.write_done.load(std::memory_order_acquire) && f.read_done.load(std::memory_order_acquire);
    });
    const auto state = f.server->shutdown_state_for_test();
    f.cleanup(); // Join public tasks and drain before assertions/freeing storage.
    REQUIRE(allocated);
    REQUIRE(writer_parked);
    CHECK(committed.accepted_ciphertext > committed.drained_ciphertext);
    REQUIRE(close_sent == 0);
    REQUIRE(alert_queued);
    REQUIRE(completed);
    CHECK_FALSE(f.threw.load(std::memory_order_acquire));
    CHECK(received == f.payload);
    CHECK(final_error == SSL_ERROR_ZERO_RETURN);
    CHECK(f.written.result == 16384);
    CHECK(f.read_result.result == 0);
    CHECK(state.transport_error == 0);
    CHECK_FALSE(state.pump_active);
}
}

TEST_CASE("TLS 1.2 peer closure preserves committed ciphertext before its alert",
          "[tls][finish][order][issue-1217]") {
    SECTION("epoll") { order_case(backend_type::epoll); }
    SECTION("io_uring") {
#if ELIO_HAS_IO_URING
        if (!io::io_uring_backend::is_available()) SKIP("io_uring unavailable on this host");
        order_case(backend_type::io_uring);
#else
        SKIP("io_uring support is not compiled");
#endif
    }
}
#endif
