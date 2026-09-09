#include <catch2/catch_test_macros.hpp>
#include <elio/http/http_response_sender.hpp>
#include <elio/net/tcp.hpp>
#include <elio/runtime/scheduler.hpp>
#if ELIO_HAS_TLS
#include <elio/tls/tls_stream.hpp>
#include <openssl/evp.h>
#include <openssl/rsa.h>
#include <openssl/x509.h>
#endif
#include "../test_main.cpp"

#include <arpa/inet.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <utility>

namespace {
using namespace elio;
using backend_type = io::io_context::backend_type;

class streaming_backend_guard {
public:
    explicit streaming_backend_guard(backend_type backend)
        : previous_(runtime::detail::worker_io_backend_for_test.exchange(backend)) {}
    ~streaming_backend_guard() {
        runtime::detail::worker_io_backend_for_test.store(previous_);
    }
private:
    backend_type previous_;
};

template<typename Predicate>
bool await_observation(Predicate predicate) {
    const auto deadline = std::chrono::steady_clock::now() + test::scaled_ms(5000);
    while (!predicate()) {
        if (std::chrono::steady_clock::now() >= deadline) return false;
        // This is only an observation loop; correctness depends on the
        // observed pending I/O, never an elapsed sleep before cancellation.
        std::this_thread::yield();
    }
    return true;
}

struct loopback_pair {
    int sender = -1;
    int peer = -1;
    ~loopback_pair() {
        if (sender >= 0) ::close(sender);
        if (peer >= 0) ::close(peer);
    }
    void open() {
        net::tcp_stream listener(::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0));
        REQUIRE(listener.fd() >= 0);
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        REQUIRE(::bind(listener.fd(), reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0);
        REQUIRE(::listen(listener.fd(), 1) == 0);
        socklen_t length = sizeof(address);
        REQUIRE(::getsockname(listener.fd(), reinterpret_cast<sockaddr*>(&address), &length) == 0);
        sender = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
        REQUIRE(sender >= 0);
        REQUIRE(::connect(sender, reinterpret_cast<sockaddr*>(&address), length) == 0);
        peer = ::accept4(listener.fd(), nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
        REQUIRE(peer >= 0);
        const int small = 4096;
        REQUIRE(::setsockopt(sender, SOL_SOCKET, SO_SNDBUF, &small, sizeof(small)) == 0);
        REQUIRE(::setsockopt(peer, SOL_SOCKET, SO_RCVBUF, &small, sizeof(small)) == 0);
    }
};

template<typename Stream>
struct observed_stream {
    Stream& stream;
    const char* payload;
    size_t payload_size;
    std::atomic<bool> active{false};
    std::atomic<bool> borrowed{false};
    size_t calls = 0;

    coro::task<io::io_result> writev(iovec* parts, size_t count, coro::cancel_token token) {
        ++calls;
        for (size_t i = 0; i < count; ++i) {
            // Compare integer addresses: framing buffers are unrelated objects.
            const auto address = reinterpret_cast<uintptr_t>(parts[i].iov_base);
            const auto begin = reinterpret_cast<uintptr_t>(payload);
            if (address >= begin && address < begin + payload_size) borrowed = true;
        }
        active.store(true, std::memory_order_release);
        struct cleanup {
            std::atomic<bool>& active;
            ~cleanup() { active.store(false, std::memory_order_release); }
        } guard{active};
        co_return co_await stream.writev(parts, count, std::move(token));
    }
};

template<typename Stream>
void exercise_backpressure(runtime::scheduler& sched, Stream& stream, int peer_fd,
                           bool timeout) {
    std::string payload(8 * 1024 * 1024, 'x');
    std::array<http::body_buffer, 2> buffers{{
        {payload.data(), payload.size() / 2},
        {payload.data() + payload.size() / 2, payload.size() / 2}}};
    const auto original = buffers;
    observed_stream<Stream> observed{stream, payload.data(), payload.size()};
    coro::cancel_source source;
    http::send_result body_result;
    http::response_send_result result;
    size_t calls_after_body = 0;
    std::atomic<bool> done{false};
    auto& context = sched.get_worker(0)->io_context();
    const auto baseline = context.pending_count();
    http::reply selected = http::streaming_response(http::status::ok,
        [&](http::body_writer& writer, coro::cancel_token token) -> coro::task<http::send_result> {
            body_result = co_await writer.writev(buffers, std::move(token));
            calls_after_body = observed.calls;
            // Deliberately ignore the failed write. Shared sender must still
            // refuse the successful zero chunk and connection reuse.
            co_return http::send_result{};
        });
    const auto budget = timeout ? test::scaled_ms(500) : std::chrono::milliseconds{0};
    sched.go([&]() -> coro::task<void> {
        result = co_await http::send_response(observed, selected, http::method::GET,
            "HTTP/1.1", true, source.get_token(), budget);
        done.store(true, std::memory_order_release);
    });
    // A timed logical write also owns one watchdog timer. Requiring the
    // extra pending operation distinguishes actual socket backpressure from
    // merely seeing that timer before any transport submission.
    const bool parked = await_observation([&] {
        if (done.load(std::memory_order_acquire)) return true;
        if (!observed.borrowed.load() || !observed.active.load() ||
            context.pending_count() <= baseline + (timeout ? 1u : 0u)) return false;
        // Pending can mean only that a send was submitted. Require the
        // underlying socket to have exhausted writable capacity as well.
        pollfd writable{stream.fd(), POLLOUT, 0};
        return ::poll(&writable, 1, 0) == 0;
    });
    const bool completed_before_interrupt = done.load(std::memory_order_acquire);
    if (!timeout) source.cancel();
    const bool completed_without_drain = await_observation([&] { return done.load(); });
    bool descriptors_unchanged = true;
    bool payload_unchanged = true;
    const bool active_after_return = observed.active.load();
    if (completed_without_drain) {
        for (size_t i = 0; i < buffers.size(); ++i) {
            descriptors_unchanged = descriptors_unchanged &&
                buffers[i].data == original[i].data && buffers[i].size == original[i].size;
        }
        payload_unchanged = payload == std::string(payload.size(), 'x');
        // Reuse immediately after the awaited sender returned, while the
        // scheduler is still live (not merely after scheduler teardown).
        payload.assign(payload.size(), 'r');
        buffers.fill({nullptr, 0});
    }
    if (!completed_without_drain) {
        source.cancel();
        // Failure cleanup only. Never close the sending descriptor while an
        // operation could still own the borrowed buffers.
        ::shutdown(peer_fd, SHUT_RDWR);
    }
    const bool stopped = sched.shutdown(test::scaled_ms(5000));
    REQUIRE(stopped);
    REQUIRE(parked);
    REQUIRE_FALSE(completed_before_interrupt);
    REQUIRE(completed_without_drain);
    REQUIRE(observed.borrowed.load());
    REQUIRE_FALSE(active_after_return);
    REQUIRE_FALSE(observed.active.load());
    REQUIRE(context.pending_count() == baseline);
    REQUIRE(body_result.error == (timeout ? http::send_errc::timed_out : http::send_errc::cancelled));
    REQUIRE(result.result.error == body_result.error);
    REQUIRE_FALSE(result.reusable);
    REQUIRE(body_result.confirmed_body_bytes < payload.size());
    REQUIRE(result.result.confirmed_body_bytes == body_result.confirmed_body_bytes);
    REQUIRE(observed.calls == calls_after_body);
    REQUIRE(descriptors_unchanged);
    REQUIRE(payload_unchanged);
    REQUIRE(payload.front() == 'r');
    REQUIRE(payload.back() == 'r');
}

#if ELIO_HAS_TLS
bool streaming_certificate(tls::tls_context& context) {
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
#endif

void run_transport(backend_type backend, bool encrypted, bool timeout) {
    streaming_backend_guard guard(backend);
    loopback_pair pair;
    pair.open();
    runtime::scheduler sched(1);
    sched.start();
    REQUIRE(sched.get_worker(0)->io_context().get_backend_type() == backend);
    if (!encrypted) {
        net::tcp_stream sender(std::exchange(pair.sender, -1));
        exercise_backpressure(sched, sender, pair.peer, timeout);
        return;
    }
#if ELIO_HAS_TLS
    tls::tls_context server_context(tls::tls_mode::server);
    tls::tls_context peer_context(tls::tls_mode::client);
    REQUIRE(streaming_certificate(server_context));
    peer_context.set_verify_mode(tls::verify_mode::none);
    tls::tls_stream sender{net::tcp_stream(std::exchange(pair.sender, -1)), server_context};
    const int peer_fd = pair.peer;
    tls::tls_stream peer{net::tcp_stream(std::exchange(pair.peer, -1)), peer_context};
    coro::cancel_source stop_handshake;
    std::atomic<int> completed{0};
    bool sender_ok = false;
    bool peer_ok = false;
    sched.go([&]() -> coro::task<void> {
        sender_ok = co_await sender.handshake(stop_handshake.get_token());
        completed.fetch_add(1, std::memory_order_release);
    });
    sched.go([&]() -> coro::task<void> {
        peer_ok = co_await peer.handshake(stop_handshake.get_token());
        completed.fetch_add(1, std::memory_order_release);
    });
    const bool ready = await_observation([&] { return completed.load(std::memory_order_acquire) == 2; });
    if (!ready) {
        stop_handshake.cancel();
        // Do not mutate TLS state from the observing thread while handshake
        // coroutines may still access it. Kernel shutdown only releases I/O.
        ::shutdown(sender.fd(), SHUT_RDWR);
        ::shutdown(peer.fd(), SHUT_RDWR);
        REQUIRE(sched.shutdown(test::scaled_ms(5000)));
    }
    REQUIRE(ready);
    REQUIRE(sender_ok);
    REQUIRE(peer_ok);
    REQUIRE(await_observation([&] { return sched.get_worker(0)->io_context().pending_count() == 0; }));
    // Neither peer reads nor close_notify run while the sender is blocked.
    // Socket shutdown after cleanup prevents destructor TLS writes.
    exercise_backpressure(sched, sender, peer_fd, timeout);
    sender.shutdown_socket();
    peer.shutdown_socket();
#else
    SKIP("TLS support is not compiled");
#endif
}
} // namespace

TEST_CASE("HTTP sender cleans up real backpressured borrowed TCP and TLS writes",
          "[http][streaming][transport][cancel][timeout]") {
    auto cases = [&](backend_type backend) {
        SECTION("TCP cancellation") { run_transport(backend, false, false); }
        SECTION("TCP timeout") { run_transport(backend, false, true); }
        SECTION("TLS cancellation") { run_transport(backend, true, false); }
        SECTION("TLS timeout") { run_transport(backend, true, true); }
    };
    SECTION("forced epoll") { cases(backend_type::epoll); }
    SECTION("forced io_uring") {
#if ELIO_HAS_IO_URING
        if (!io::io_uring_backend::is_available()) SKIP("io_uring unavailable on this host");
        cases(backend_type::io_uring);
#else
        SKIP("io_uring support is not compiled");
#endif
    }
}
