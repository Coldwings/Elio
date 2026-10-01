#include <catch2/catch_test_macros.hpp>

#if defined(ELIO_HAS_TLS) && ELIO_HAS_TLS && defined(ELIO_RUNTIME_TEST_HOOKS)
#include <elio/tls/tls_stream.hpp>
#include <elio/runtime/scheduler.hpp>
#include <elio/sync/event.hpp>
#include <openssl/evp.h>
#include <openssl/rsa.h>
#include <openssl/x509.h>
#include "../test_main.cpp"

#include <arpa/inet.h>
#include <unistd.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <exception>
#include <limits>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string_view>
#include <thread>
#include <type_traits>
#include <utility>

namespace {
using namespace elio;
using backend_type = io::io_context::backend_type;
constexpr size_t duplex_bytes = 2 * 1024 * 1024;
constexpr size_t duplex_chunk = 64 * 1024;

class duplex_backend_guard {
public:
    explicit duplex_backend_guard(backend_type backend)
        : previous_(runtime::detail::worker_io_backend_for_test.exchange(backend)) {}
    ~duplex_backend_guard() { runtime::detail::worker_io_backend_for_test.store(previous_); }
private:
    backend_type previous_;
};

template<typename Predicate>
bool duplex_observe(Predicate predicate) {
    const auto end = std::chrono::steady_clock::now() + test::scaled_ms(15000);
    while (!predicate()) {
        if (std::chrono::steady_clock::now() >= end) return false;
        std::this_thread::yield();
    }
    return true;
}

bool duplex_certificate(tls::tls_context& context) {
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

struct duplex_sockets {
    int client = -1;
    int server = -1;
    ~duplex_sockets() {
        if (client >= 0) ::close(client);
        if (server >= 0) ::close(server);
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
        client = ::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
        REQUIRE(client >= 0);
        REQUIRE(::connect(client, reinterpret_cast<sockaddr*>(&address), length) == 0);
        server = ::accept4(listener.fd(), nullptr, nullptr, SOCK_CLOEXEC | SOCK_NONBLOCK);
        REQUIRE(server >= 0);
        const int small = 4096;
        for (const auto fd : {client, server}) {
            REQUIRE(::setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &small, sizeof(small)) == 0);
        }
    }
};

struct duplex_observations {
    std::array<std::atomic<bool>, 2> queued{};
    std::atomic<unsigned> cleaned{0};
    std::atomic<bool> joined{false};
    std::array<size_t, 4> counts{};
    std::array<int, 4> errors{};
    std::array<bool, 4> valid{true, true, true, true};
    bool launch_failed = false;
    static void allocated(void* context, bool payload) noexcept {
        if (payload) static_cast<std::atomic<bool>*>(context)->store(true, std::memory_order_release);
    }
};

unsigned char duplex_pattern(size_t offset, unsigned direction) {
    return static_cast<unsigned char>((offset * 17 + offset / 251 + direction * 103) % 256);
}

coro::task<void> duplex_worker(tls::tls_stream& stream, unsigned index,
                              sync::event& readers, coro::cancel_token token,
                              duplex_observations& observations) {
    std::array<unsigned char, duplex_chunk> buffer{};
    struct cleanup {
        std::atomic<unsigned>& count;
        ~cleanup() { count.fetch_add(1, std::memory_order_release); }
    } cleanup_on_return{observations.cleaned};
    const bool writing = index < 2;
    const unsigned direction = writing ? index : index - 2;
    try {
        if (!writing) co_await readers.wait(token);
        while (observations.counts[index] < duplex_bytes) {
            if (token.is_cancelled()) { observations.errors[index] = ECANCELED; break; }
            const auto offset = observations.counts[index];
            const auto count = std::min(buffer.size(), duplex_bytes - offset);
            io::io_result result;
            if (writing) {
                for (size_t i = 0; i < count; ++i) buffer[i] = duplex_pattern(offset + i, direction);
                result = co_await stream.write_exactly(buffer.data(), count, token);
            } else {
                result = co_await stream.read(buffer.data(), count, token);
            }
            if (result.result <= 0) {
                observations.errors[index] = result.result < 0 ? result.error_code() : EPIPE;
                break;
            }
            const auto progress = static_cast<size_t>(result.result);
            if (progress > count) { observations.errors[index] = EOVERFLOW; break; }
            if (!writing) {
                for (size_t i = 0; i < progress; ++i) {
                    observations.valid[index] &= buffer[i] == duplex_pattern(offset + i, direction);
                }
            }
            observations.counts[index] += progress;
        }
    } catch (...) { observations.errors[index] = EIO; }
}

void run_duplex(backend_type backend, tls::tls_version version, bool cancel_pending) {
    CAPTURE(backend, version, cancel_pending);
    duplex_backend_guard backend_scope(backend);
    duplex_sockets sockets;
    sockets.open();
    tls::tls_context server_context(tls::tls_mode::server, version);
    tls::tls_context client_context(tls::tls_mode::client, version);
    REQUIRE(duplex_certificate(server_context));
    client_context.set_verify_mode(tls::verify_mode::none);
    tls::tls_stream server(net::tcp_stream(std::exchange(sockets.server, -1)), server_context);
    tls::tls_stream client(net::tcp_stream(std::exchange(sockets.client, -1)), client_context);
    runtime::scheduler scheduler(2);
    scheduler.start();
    coro::cancel_source cancel;
    std::array<bool, 2> handshakes{};
    std::atomic<unsigned> handshakes_done{0};
    scheduler.go([&]() -> coro::task<void> {
        try { handshakes[0] = co_await server.handshake(cancel.get_token()); } catch (...) {}
        handshakes_done.fetch_add(1, std::memory_order_release);
    });
    scheduler.go([&]() -> coro::task<void> {
        try { handshakes[1] = co_await client.handshake(cancel.get_token()); } catch (...) {}
        handshakes_done.fetch_add(1, std::memory_order_release);
    });
    const bool handshake_completed = duplex_observe([&] {
        return handshakes_done.load(std::memory_order_acquire) == 2;
    });
    if (!handshake_completed || !handshakes[0] || !handshakes[1]) {
        cancel.cancel();
        server.shutdown_socket();
        client.shutdown_socket();
        // Never unwind stream storage while operations may still reference it.
        if (!scheduler.shutdown(test::scaled_ms(15000))) std::terminate();
        REQUIRE(handshake_completed);
        REQUIRE(handshakes[0]);
        REQUIRE(handshakes[1]);
        return;
    }
    const bool negotiated = std::string_view(server.version()) ==
        (version == tls::tls_version::tls_1_2 ? "TLSv1.2" : "TLSv1.3") &&
        std::string_view(client.version()) == std::string_view(server.version());

    duplex_observations observations;
    sync::event readers;
    // Only observe actual payload allocation; all SSL and socket calls remain real.
    server.set_output_test_hooks({&observations.queued[0], nullptr, duplex_observations::allocated});
    client.set_output_test_hooks({&observations.queued[1], nullptr, duplex_observations::allocated});
    scheduler.go([&]() -> coro::task<void> {
        std::array<std::optional<coro::join_handle<void>>, 4> tasks;
        try {
            tasks[0].emplace(scheduler.go_joinable([&] { return duplex_worker(server, 0, readers, cancel.get_token(), observations); }));
            tasks[1].emplace(scheduler.go_joinable([&] { return duplex_worker(client, 1, readers, cancel.get_token(), observations); }));
            tasks[2].emplace(scheduler.go_joinable([&] { return duplex_worker(client, 2, readers, cancel.get_token(), observations); }));
            tasks[3].emplace(scheduler.go_joinable([&] { return duplex_worker(server, 3, readers, cancel.get_token(), observations); }));
        } catch (...) {
            observations.launch_failed = true;
            cancel.cancel();
            readers.set();
        }
        for (auto& task : tasks) {
            if (task) {
                try { co_await std::move(*task); } catch (...) { observations.launch_failed = true; }
            }
        }
        observations.joined.store(true, std::memory_order_release);
    });
    const bool both_queued = duplex_observe([&] {
        return (observations.queued[0].load(std::memory_order_acquire) &&
                observations.queued[1].load(std::memory_order_acquire)) ||
                observations.joined.load(std::memory_order_acquire);
    }) && observations.queued[0].load(std::memory_order_acquire) &&
          observations.queued[1].load(std::memory_order_acquire);
    // No elapsed delay proves readiness: release readers/cancel only after
    // observing real queued ciphertext in both write directions.
    if (!both_queued || cancel_pending) cancel.cancel();
    readers.set();
    const bool completed_in_budget = duplex_observe([&] {
        return observations.joined.load(std::memory_order_acquire);
    });
    if (!completed_in_budget) {
        cancel.cancel();
        server.shutdown_socket();
        client.shutdown_socket();
    }
    if (!scheduler.shutdown(test::scaled_ms(15000))) std::terminate();
    server.set_output_test_hooks({});
    client.set_output_test_hooks({});
    server.shutdown_socket();
    client.shutdown_socket();
    REQUIRE(negotiated);
    REQUIRE(both_queued);
    REQUIRE(completed_in_budget);
    REQUIRE(observations.joined.load(std::memory_order_acquire));
    REQUIRE_FALSE(observations.launch_failed);
    REQUIRE(observations.cleaned.load(std::memory_order_acquire) == 4);
    for (unsigned index = 0; index < 4; ++index) {
        CAPTURE(index);
        if (cancel_pending) {
            REQUIRE(observations.errors[index] == ECANCELED);
            REQUIRE(observations.counts[index] < duplex_bytes);
        } else {
            REQUIRE(observations.errors[index] == 0);
            REQUIRE(observations.counts[index] == duplex_bytes);
            REQUIRE(observations.valid[index]);
        }
    }
}

void duplex_versions(backend_type backend, bool cancel_pending) {
    SECTION("TLS 1.2") { run_duplex(backend, tls::tls_version::tls_1_2, cancel_pending); }
    SECTION("TLS 1.3") { run_duplex(backend, tls::tls_version::tls_1_3, cancel_pending); }
}

class fdless_tcp_stream {
public:
    struct stats {
        std::atomic<unsigned> clamped_reads{0};
        std::atomic<unsigned> clamped_writes{0};
        std::atomic<unsigned> reads{0};
        std::atomic<unsigned> writes{0};
        std::atomic<unsigned> finishes{0};
        std::atomic<unsigned> aborts{0};
        std::atomic<unsigned> shutdowns{0};
    };

    using byte_stream_contract = net::publishing_byte_stream_contract;
    static constexpr bool tls_progress_interrupts_read = true;

    explicit fdless_tcp_stream(
        net::tcp_stream stream,
        size_t max_read = std::numeric_limits<size_t>::max(),
        size_t max_write = std::numeric_limits<size_t>::max(),
        std::shared_ptr<stats> counters = {}) noexcept
        : stream_(std::move(stream))
        , max_read_(max_read)
        , max_write_(max_write)
        , counters_(std::move(counters)) {}
    fdless_tcp_stream(fdless_tcp_stream&&) noexcept = default;
    fdless_tcp_stream& operator=(fdless_tcp_stream&&) noexcept = default;
    fdless_tcp_stream(const fdless_tcp_stream&) = delete;
    fdless_tcp_stream& operator=(const fdless_tcp_stream&) = delete;

    coro::task<io::io_result> read(void* buffer, size_t length,
                                   coro::cancel_token token) {
        if (counters_)
            counters_->reads.fetch_add(1, std::memory_order_relaxed);
        const auto requested = length;
        length = std::min(length, max_read_);
        if (counters_ && length < requested)
            counters_->clamped_reads.fetch_add(1, std::memory_order_relaxed);
        return stream_.read(buffer, length, std::move(token));
    }

    coro::task<io::io_result> write(const void* buffer, size_t length,
                                    coro::cancel_token token) {
        if (counters_)
            counters_->writes.fetch_add(1, std::memory_order_relaxed);
        const auto requested = length;
        length = std::min(length, max_write_);
        if (counters_ && length < requested)
            counters_->clamped_writes.fetch_add(1, std::memory_order_relaxed);
        return stream_.write(buffer, length, std::move(token));
    }

    coro::task<net::write_finish_result> finish_write(
        coro::cancel_token token, std::chrono::milliseconds timeout) {
        if (counters_)
            counters_->finishes.fetch_add(1, std::memory_order_relaxed);
        co_return co_await stream_.finish_write(std::move(token), timeout);
    }

    net::close_scope read_end_scope() const noexcept {
        return stream_.read_end_scope();
    }

    coro::task<void> abort_and_settle() {
        if (counters_)
            counters_->aborts.fetch_add(1, std::memory_order_relaxed);
        stream_.shutdown_socket();
        co_return;
    }

    void shutdown_socket() noexcept {
        if (counters_)
            counters_->shutdowns.fetch_add(1, std::memory_order_relaxed);
        stream_.shutdown_socket();
    }

private:
    net::tcp_stream stream_;
    size_t max_read_;
    size_t max_write_;
    std::shared_ptr<stats> counters_;
};

class throwing_fd_stream {
public:
    using byte_stream_contract = net::publishing_byte_stream_contract;

    throwing_fd_stream() = default;
    throwing_fd_stream(throwing_fd_stream&&) noexcept = default;
    throwing_fd_stream& operator=(throwing_fd_stream&&) noexcept = default;
    throwing_fd_stream(const throwing_fd_stream&) = delete;
    throwing_fd_stream& operator=(const throwing_fd_stream&) = delete;

    coro::task<io::io_result> read(void*, size_t, coro::cancel_token) {
        co_return io::io_result{-ENOTCONN, 0};
    }

    coro::task<io::io_result> write(const void*, size_t, coro::cancel_token) {
        co_return io::io_result{-ENOTCONN, 0};
    }

    coro::task<net::write_finish_result> finish_write(
        coro::cancel_token, std::chrono::milliseconds) {
        co_return net::write_finish_result{net::close_scope::whole_session, ENOTCONN};
    }

    net::close_scope read_end_scope() const noexcept {
        return net::close_scope::whole_session;
    }

    coro::task<void> abort_and_settle() { co_return; }

    int fd() const { throw std::runtime_error("diagnostic fd only"); }
};

struct non_descriptor_fd_probe {
    const char* fd() const noexcept { return "not a descriptor"; }
};

template<typename Stream>
concept exposes_fd = requires(const Stream& stream) {
    stream.fd();
};

template<typename ServerStream, typename ClientStream>
void complete_tls_pair_handshake(ServerStream& server, ClientStream& client) {
    runtime::scheduler scheduler(2);
    scheduler.start();
    coro::cancel_source cancel;
    std::array<bool, 2> handshakes{};
    std::atomic<unsigned> finished{0};
    scheduler.go([&]() -> coro::task<void> {
        try { handshakes[0] = co_await server.handshake(cancel.get_token()); } catch (...) {}
        finished.fetch_add(1, std::memory_order_release);
    });
    scheduler.go([&]() -> coro::task<void> {
        try { handshakes[1] = co_await client.handshake(cancel.get_token()); } catch (...) {}
        finished.fetch_add(1, std::memory_order_release);
    });
    const bool ready = duplex_observe([&] {
        return finished.load(std::memory_order_acquire) == 2;
    });
    if (!ready || !handshakes[0] || !handshakes[1]) {
        cancel.cancel();
        server.shutdown_socket();
        client.shutdown_socket();
    }
    REQUIRE(scheduler.shutdown(test::scaled_ms(15000)));
    REQUIRE(ready);
    REQUIRE(handshakes[0]);
    REQUIRE(handshakes[1]);
}

template<typename SenderStream, typename ReceiverStream>
void exchange_tls_payload(SenderStream& sender, ReceiverStream& receiver,
                          std::string_view payload) {
    runtime::scheduler scheduler(2);
    scheduler.start();
    coro::cancel_source cancel;
    std::string received(payload.size(), '\0');
    io::io_result write_result{};
    io::io_result read_result{};
    std::atomic<unsigned> finished{0};
    scheduler.go([&]() -> coro::task<void> {
        try { write_result = co_await sender.write_exactly(payload, cancel.get_token()); }
        catch (...) { write_result = {-EIO, 0}; }
        finished.fetch_add(1, std::memory_order_release);
    });
    scheduler.go([&]() -> coro::task<void> {
        try {
            read_result = co_await receiver.read_exactly(
                received.data(), received.size(), cancel.get_token());
        } catch (...) { read_result = {-EIO, 0}; }
        finished.fetch_add(1, std::memory_order_release);
    });
    const bool ready = duplex_observe([&] {
        return finished.load(std::memory_order_acquire) == 2;
    });
    if (!ready) {
        cancel.cancel();
        sender.shutdown_socket();
        receiver.shutdown_socket();
    }
    REQUIRE(scheduler.shutdown(test::scaled_ms(15000)));
    REQUIRE(ready);
    REQUIRE(write_result.result == static_cast<int>(payload.size()));
    REQUIRE(read_result.result == static_cast<int>(payload.size()));
    REQUIRE(received == payload);
}

template<typename Stream>
net::write_finish_result finish_tls_write(Stream& stream) {
    runtime::scheduler scheduler(2);
    scheduler.start();
    net::write_finish_result finished{
        net::close_scope::whole_session, EIO, false, false};
    std::atomic<bool> done{false};
    scheduler.go([&]() -> coro::task<void> {
        try {
            finished = co_await stream.finish_write(
                {}, std::chrono::milliseconds(5000));
        } catch (...) {
            finished.error = EIO;
        }
        done.store(true, std::memory_order_release);
    });
    const bool ready = duplex_observe([&] {
        return done.load(std::memory_order_acquire);
    });
    if (!ready) stream.shutdown_socket();
    REQUIRE(scheduler.shutdown(test::scaled_ms(15000)));
    REQUIRE(ready);
    return finished;
}

template<typename Stream>
void abort_tls_stream(Stream& stream) {
    runtime::scheduler scheduler(1);
    scheduler.start();
    std::atomic<bool> done{false};
    bool threw = false;
    scheduler.go([&]() -> coro::task<void> {
        try {
            co_await stream.abort_and_settle();
        } catch (...) {
            threw = true;
        }
        done.store(true, std::memory_order_release);
    });
    const bool ready = duplex_observe([&] {
        return done.load(std::memory_order_acquire);
    });
    if (!ready) stream.shutdown_socket();
    REQUIRE(scheduler.shutdown(test::scaled_ms(15000)));
    REQUIRE(ready);
    REQUIRE_FALSE(threw);
}

void generic_tls_contexts(tls::tls_context& server, tls::tls_context& client) {
    REQUIRE(duplex_certificate(server));
    client.set_verify_mode(tls::verify_mode::none);
}
}

TEST_CASE("TLS real duplex keeps bounded writes and opposite reads progressing", "[tls][duplex][issue-1215]") {
    SECTION("forced epoll") { duplex_versions(backend_type::epoll, false); }
    SECTION("forced io_uring") {
#if ELIO_HAS_IO_URING
        if (!io::io_uring_backend::is_available()) SKIP("io_uring unavailable on this host");
        duplex_versions(backend_type::io_uring, false);
#else
        SKIP("io_uring support is not compiled");
#endif
    }
}

TEST_CASE("TLS real pending duplex writes cancel before borrowed frames retire", "[tls][duplex][cancel][issue-1215]") {
    SECTION("forced epoll") { duplex_versions(backend_type::epoll, true); }
    SECTION("forced io_uring") {
#if ELIO_HAS_IO_URING
        if (!io::io_uring_backend::is_available()) SKIP("io_uring unavailable on this host");
        duplex_versions(backend_type::io_uring, true);
#else
        SKIP("io_uring support is not compiled");
#endif
    }
}

TEST_CASE("TLS stream ignores unsafe diagnostic lower fd hooks", "[tls][generic][issue-1244]") {
    STATIC_REQUIRE(net::publishing_byte_stream<throwing_fd_stream>);
    STATIC_REQUIRE(!tls::detail::noexcept_int_fd<throwing_fd_stream>);
    STATIC_REQUIRE(!tls::detail::noexcept_int_fd<non_descriptor_fd_probe>);
    tls::tls_context context(tls::tls_mode::client);
    tls::basic_tls_stream<throwing_fd_stream> stream(throwing_fd_stream{}, context);
    CHECK(stream.fd() == -1);
}

TEST_CASE("TLS stream can wrap fd-less async byte streams", "[tls][generic][issue-1244]") {
    STATIC_REQUIRE(!exposes_fd<fdless_tcp_stream>);
    STATIC_REQUIRE(net::publishing_byte_stream<fdless_tcp_stream>);
    STATIC_REQUIRE(net::publishing_byte_stream<tls::tls_stream>);
    STATIC_REQUIRE(net::publishing_byte_stream<tls::basic_tls_stream<fdless_tcp_stream>>);
    STATIC_REQUIRE(std::is_move_constructible_v<tls::basic_tls_stream<fdless_tcp_stream>>);
    int sockets[2];
    REQUIRE(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sockets) == 0);
    tls::tls_context server_context(tls::tls_mode::server);
    tls::tls_context client_context(tls::tls_mode::client);
    generic_tls_contexts(server_context, client_context);
    tls::basic_tls_stream<fdless_tcp_stream> server(
        fdless_tcp_stream{net::tcp_stream{sockets[0]}}, server_context);
    tls::basic_tls_stream<fdless_tcp_stream> client(
        fdless_tcp_stream{net::tcp_stream{sockets[1]}}, client_context);

    complete_tls_pair_handshake(server, client);
    exchange_tls_payload(client, server, "generic lower stream payload");
    server.shutdown_socket();
    client.shutdown_socket();
}

TEST_CASE("TLS stream retries short positive generic lower reads and writes",
          "[tls][generic][short][issue-1244]") {
    int sockets[2];
    REQUIRE(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sockets) == 0);
    tls::tls_context server_context(tls::tls_mode::server);
    tls::tls_context client_context(tls::tls_mode::client);
    generic_tls_contexts(server_context, client_context);
    auto server_stats = std::make_shared<fdless_tcp_stream::stats>();
    auto client_stats = std::make_shared<fdless_tcp_stream::stats>();
    tls::basic_tls_stream<fdless_tcp_stream> server(
        fdless_tcp_stream{net::tcp_stream{sockets[0]}, 5, 7, server_stats},
        server_context);
    tls::basic_tls_stream<fdless_tcp_stream> client(
        fdless_tcp_stream{net::tcp_stream{sockets[1]}, 5, 7, client_stats},
        client_context);

    complete_tls_pair_handshake(server, client);
    exchange_tls_payload(client, server,
        "generic lower stream payload split across many ciphertext chunks");
    server.shutdown_socket();
    client.shutdown_socket();

    CHECK(server_stats->clamped_reads.load(std::memory_order_relaxed) > 0);
    CHECK(server_stats->clamped_writes.load(std::memory_order_relaxed) > 0);
    CHECK(client_stats->clamped_reads.load(std::memory_order_relaxed) > 0);
    CHECK(client_stats->clamped_writes.load(std::memory_order_relaxed) > 0);
}

TEST_CASE("TLS stream finish_write drains close notify through fd-less lowers",
          "[tls][generic][finish][issue-1244]") {
    int sockets[2];
    REQUIRE(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sockets) == 0);
    tls::tls_context server_context(tls::tls_mode::server, tls::tls_version::tls_1_3);
    tls::tls_context client_context(tls::tls_mode::client, tls::tls_version::tls_1_3);
    generic_tls_contexts(server_context, client_context);
    auto server_stats = std::make_shared<fdless_tcp_stream::stats>();
    auto client_stats = std::make_shared<fdless_tcp_stream::stats>();
    tls::basic_tls_stream<fdless_tcp_stream> server(
        fdless_tcp_stream{net::tcp_stream{sockets[0]}, 7, 5, server_stats},
        server_context);
    tls::basic_tls_stream<fdless_tcp_stream> client(
        fdless_tcp_stream{net::tcp_stream{sockets[1]}, 7, 5, client_stats},
        client_context);

    complete_tls_pair_handshake(server, client);
    exchange_tls_payload(client, server, "generic finish payload");
    const auto writes_before_finish =
        client_stats->writes.load(std::memory_order_relaxed);

    auto finished = finish_tls_write(client);

    CHECK(finished.error == 0);
    CHECK(finished.scope == net::close_scope::write_direction);
    CHECK(finished.local_end_flushed);
    CHECK(client_stats->writes.load(std::memory_order_relaxed) > writes_before_finish);
    server.shutdown_socket();
    client.shutdown_socket();
}

TEST_CASE("TLS stream abort_and_settle forwards through fd-less lowers",
          "[tls][generic][abort][issue-1244]") {
    int sockets[2];
    REQUIRE(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sockets) == 0);
    tls::tls_context context(tls::tls_mode::client);
    auto stats = std::make_shared<fdless_tcp_stream::stats>();
    tls::basic_tls_stream<fdless_tcp_stream> stream(
        fdless_tcp_stream{net::tcp_stream{sockets[0]}, 7, 5, stats}, context);

    abort_tls_stream(stream);

    CHECK(stats->aborts.load(std::memory_order_relaxed) == 1);
    CHECK(stream.shutdown_state_for_test().transport_error == ECANCELED);
    CHECK_FALSE(stream.shutdown_state_for_test().pump_active);
    ::close(sockets[1]);
}

TEST_CASE("TLS stream can layer over another TLS stream without fd bypass", "[tls][generic][nested][issue-1244]") {
    int sockets[2];
    REQUIRE(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sockets) == 0);
    tls::tls_context inner_server_context(tls::tls_mode::server);
    tls::tls_context inner_client_context(tls::tls_mode::client);
    tls::tls_context outer_server_context(tls::tls_mode::server);
    tls::tls_context outer_client_context(tls::tls_mode::client);
    generic_tls_contexts(inner_server_context, inner_client_context);
    generic_tls_contexts(outer_server_context, outer_client_context);

    tls::tls_stream inner_server{net::tcp_stream{sockets[0]}, inner_server_context};
    tls::tls_stream inner_client{net::tcp_stream{sockets[1]}, inner_client_context};
    complete_tls_pair_handshake(inner_server, inner_client);

    tls::basic_tls_stream<tls::tls_stream> outer_server(
        std::move(inner_server), outer_server_context);
    tls::basic_tls_stream<tls::tls_stream> outer_client(
        std::move(inner_client), outer_client_context);
    complete_tls_pair_handshake(outer_server, outer_client);
    exchange_tls_payload(outer_client, outer_server, "nested TLS lower stream payload");
    outer_server.shutdown_socket();
    outer_client.shutdown_socket();
}
#endif
