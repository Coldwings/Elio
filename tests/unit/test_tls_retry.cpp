#include <catch2/catch_test_macros.hpp>
#include <elio/tls/tls_stream.hpp>
#include <elio/runtime/scheduler.hpp>
#include <openssl/evp.h>
#include <openssl/rsa.h>
#include <openssl/x509.h>
#include "../test_main.cpp"

#include <array>
#include <atomic>
#include <chrono>
#include <cerrno>
#include <coroutine>
#include <cstring>
#include <exception>
#include <memory>
#include <optional>
#include <string_view>
#include <thread>
#include <utility>

namespace {
using namespace elio;
using operation = tls::detail::tls_test_operation;

bool retry_certificate(tls::tls_context& context) {
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

void complete_retry_handshake(tls::tls_stream& server, tls::tls_stream& peer) {
    runtime::scheduler scheduler(1);
    scheduler.start();
    coro::cancel_source cancel;
    std::atomic<unsigned> finished{0};
    bool server_ok = false;
    bool peer_ok = false;
    scheduler.go([&]() -> coro::task<void> {
        server_ok = co_await server.handshake(cancel.get_token());
        finished.fetch_add(1, std::memory_order_release);
    });
    scheduler.go([&]() -> coro::task<void> {
        peer_ok = co_await peer.handshake(cancel.get_token());
        finished.fetch_add(1, std::memory_order_release);
    });
    const auto deadline = std::chrono::steady_clock::now() + test::scaled_ms(5000);
    while (finished.load(std::memory_order_acquire) != 2 &&
           std::chrono::steady_clock::now() < deadline) std::this_thread::yield();
    const bool ready = finished.load(std::memory_order_acquire) == 2;
    if (!ready) {
        cancel.cancel();
        server.shutdown_socket();
        peer.shutdown_socket();
    }
    REQUIRE(scheduler.shutdown(test::scaled_ms(5000)));
    REQUIRE(ready);
    REQUIRE(server_ok);
    REQUIRE(peer_ok);
}

struct retry_script {
    retry_script(operation pending, int error) : pending_operation(pending), pending_error(error) {}

    operation pending_operation;
    int pending_error;
    unsigned pending_calls = 0;
    unsigned other_calls = 0;
    bool interleaved = false;
    bool arguments_stable = true;
    bool direction_correct = true;
    bool fail_wait = false;
    const void* original_buffer = nullptr;
    size_t original_length = 0;
    std::string_view payload;
    std::coroutine_handle<> continuation;

    static tls::detail::tls_test_call_result dispatch(
        void* context, operation kind, const void* buffer, size_t length) noexcept {
        auto& script = *static_cast<retry_script*>(context);
        if (kind == script.pending_operation) {
            ++script.pending_calls;
            if (script.pending_calls == 1) {
                script.original_buffer = buffer;
                script.original_length = length;
                return {-1, script.pending_error};
            }
            script.arguments_stable &= buffer == script.original_buffer &&
                length == script.original_length;
            if (kind == operation::write) {
                script.arguments_stable &= length == script.payload.size() &&
                    std::memcmp(buffer, script.payload.data(), length) == 0;
            }
        } else {
            ++script.other_calls;
            script.interleaved |= script.pending_calls == 1;
        }
        if (kind == operation::read && length != 0) {
            *static_cast<char*>(const_cast<void*>(buffer)) = 'r';
            return {1, SSL_ERROR_NONE};
        }
        return {static_cast<int>(length), SSL_ERROR_NONE};
    }

    struct gate {
        retry_script& script;
        bool await_ready() const noexcept { return false; }
        void await_suspend(std::coroutine_handle<> handle) noexcept {
            script.continuation = handle;
        }
        void await_resume() const noexcept {}
    };

    static coro::task<io::io_result> readiness(
        void* context, operation kind, bool for_read, coro::cancel_token token) {
        auto& script = *static_cast<retry_script*>(context);
        script.direction_correct &= kind == script.pending_operation &&
            for_read == (script.pending_error == SSL_ERROR_WANT_READ);
        co_await gate{script};
        if (script.fail_wait) throw std::bad_alloc();
        if (token.is_cancelled()) co_return io::io_result{-ECANCELED, 0};
        co_return io::io_result{1, 0};
    }
};

void exercise_retry(operation pending, int error, bool allow_interleaving, bool cancellable,
                    bool fail_wait = false, unsigned cancel_mode = 0) {
    CAPTURE(pending, error, cancellable, cancel_mode);
    std::array<int, 2> sockets{-1, -1};
    REQUIRE(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sockets.data()) == 0);
    net::tcp_stream server_tcp(sockets[0]);
    net::tcp_stream peer_tcp(sockets[1]);
    tls::tls_context server_context(tls::tls_mode::server);
    tls::tls_context peer_context(tls::tls_mode::client);
    REQUIRE(retry_certificate(server_context));
    peer_context.set_verify_mode(tls::verify_mode::none);
    tls::tls_stream server(std::move(server_tcp), server_context);
    tls::tls_stream peer(std::move(peer_tcp), peer_context);
    complete_retry_handshake(server, peer);

    constexpr std::string_view payload = "stable write payload";
    std::array<char, 16> input{};
    retry_script script{pending, error};
    script.payload = payload;
    script.fail_wait = fail_wait;
    tls::detail::tls_dispatch_test_hooks hooks{
        &script, retry_script::dispatch, retry_script::readiness};
    server.set_dispatch_test_hooks(&hooks);
    coro::cancel_source cancel;
    coro::cancel_source independent_reader_cancel;
    if (cancel_mode == 2) cancel.cancel();
    const auto reader_token = cancel_mode ? independent_reader_cancel.get_token() : cancel.get_token();
    auto writer = cancellable ? server.write(payload, cancel.get_token()) : server.write(payload);
    auto reader = cancellable ? server.read(input.data(), input.size(), reader_token)
                              : server.read(input.data(), input.size());
    auto writer_handle = coro::detail::task_access::handle(writer);
    auto reader_handle = coro::detail::task_access::handle(reader);
    auto first = pending == operation::write ? writer_handle : reader_handle;
    auto second = pending == operation::write ? reader_handle : writer_handle;
    first.resume();
    const bool parked = static_cast<bool>(script.continuation) && !first.done();
    second.resume();
    // Advance the submitted reader/writer, not an observation waiting for an
    // SSL_read call: correctly deferred SSL dispatch must not hang this test.
    const bool interleaved_before_retry = script.interleaved;
    const bool sibling_deferred = !reader_handle.done();
    if (cancel_mode == 1) cancel.cancel();
    if (script.continuation) {
        auto resume = std::exchange(script.continuation, {});
        resume.resume();
    }
    const bool writer_done = writer_handle.done();
    const bool reader_done = reader_handle.done();
    const auto transport_state = server.shutdown_state_for_test();
    server.set_dispatch_test_hooks(nullptr);
    server.shutdown_socket();
    peer.shutdown_socket();

    REQUIRE(writer_done);
    REQUIRE(reader_done);
    if (cancel_mode) {
        CHECK(writer.await_resume().result == -ECANCELED);
        CHECK_FALSE(independent_reader_cancel.is_cancelled());
        CHECK_FALSE(interleaved_before_retry);
        CHECK_FALSE(transport_state.pump_active);
        if (cancel_mode == 1) {
            CHECK(parked);
            CHECK(sibling_deferred);
            CHECK(reader.await_resume().result == -ECANCELED);
            CHECK(transport_state.transport_error == ECANCELED);
            CHECK(script.pending_calls == 1);
            CHECK(script.other_calls == 0);
            CHECK(input[0] == '\0');
        } else {
            CHECK_FALSE(parked);
            CHECK(reader.await_resume().result == 1);
            CHECK(transport_state.transport_error == 0);
            CHECK(script.pending_calls == 0);
            CHECK(script.other_calls == 1);
            CHECK(input[0] == 'r');
        }
        return;
    }
    if (fail_wait) {
        CHECK(writer.await_resume().result == -ENOMEM);
        CHECK(reader.await_resume().result == -ENOMEM);
        CHECK(script.pending_calls == 1);
        CHECK(script.other_calls == 0);
        CHECK_FALSE(interleaved_before_retry);
        return;
    }
    CHECK(writer.await_resume().result == static_cast<int>(payload.size()));
    CHECK(reader.await_resume().result == 1);
    CHECK(parked);
    CHECK(script.pending_calls == 2);
    CHECK(script.other_calls == 1);
    CHECK(script.arguments_stable);
    CHECK(script.direction_correct);
    CHECK(input[0] == 'r');
    CHECK(interleaved_before_retry == allow_interleaving);
}
} // namespace

TEST_CASE("TLS pending WANT_WRITE excludes intervening SSL reads", "[tls][retry][issue-1215]") {
    for (bool cancellable : {false, true})
        exercise_retry(operation::write, SSL_ERROR_WANT_WRITE, false, cancellable);
}

TEST_CASE("TLS cancellation after dispatch terminates an independently tokened sibling",
          "[tls][retry][cancel][issue-1215]") {
    exercise_retry(operation::write, SSL_ERROR_WANT_WRITE, false, true, false, 1);
}

TEST_CASE("TLS precancelled write leaves independent read and connection healthy",
          "[tls][retry][cancel][issue-1215]") {
    exercise_retry(operation::write, SSL_ERROR_WANT_WRITE, false, true, false, 2);
}

TEST_CASE("TLS pending WANT_READ write allows eligible SSL reads", "[tls][retry][issue-1215]") {
    for (bool cancellable : {false, true})
        exercise_retry(operation::write, SSL_ERROR_WANT_READ, true, cancellable);
}

TEST_CASE("TLS pending read does not exclude SSL writes", "[tls][retry][issue-1215]") {
    for (bool cancellable : {false, true})
        exercise_retry(operation::read, SSL_ERROR_WANT_READ, true, cancellable);
}

namespace {
struct handshake_cancel_progress {
    coro::cancel_source cancel;
    std::atomic<bool> queued{false};
    std::atomic<bool> cancelled_after_progress{false};
    unsigned sends = 0;

    static ssize_t send(void* opaque, int fd, const void* data, size_t size, int flags) {
        auto& state = *static_cast<handshake_cancel_progress*>(opaque);
        // Queue a real encrypted handshake flight; its pump uses native I/O.
        if (state.sends++ == 0) { errno = EAGAIN; return -1; }
        return ::send(fd, data, size, flags);
    }
    static void allocate(void* opaque, bool payload) noexcept {
        if (payload) static_cast<handshake_cancel_progress*>(opaque)->queued.store(true, std::memory_order_release);
    }
    static void after_retry(void* opaque) noexcept {
        auto& state = *static_cast<handshake_cancel_progress*>(opaque);
        // Ignore initial readability before SSL_accept generates its flight.
        // Cancel only after a successful production retry and queued output.
        if (state.queued.load(std::memory_order_acquire)) {
            state.cancelled_after_progress.store(true, std::memory_order_release);
            state.cancel.cancel();
        }
    }
};

void exercise_handshake_progress_cancel(io::io_context::backend_type backend,
                                        tls::tls_version version) {
    CAPTURE(backend, version);
    struct backend_scope {
        io::io_context::backend_type previous;
        ~backend_scope() { runtime::detail::worker_io_backend_for_test.store(previous); }
    } restore{runtime::detail::worker_io_backend_for_test.exchange(backend)};
    std::array<int, 2> sockets{-1, -1};
    REQUIRE(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sockets.data()) == 0);
    net::tcp_stream server_tcp(sockets[0]);
    net::tcp_stream peer_tcp(sockets[1]);
    tls::tls_context server_context(tls::tls_mode::server, version);
    tls::tls_context peer_context(tls::tls_mode::client, version);
    REQUIRE(retry_certificate(server_context));
    peer_context.set_verify_mode(tls::verify_mode::none);
    tls::tls_stream server(std::move(server_tcp), server_context);
    tls::tls_stream peer(std::move(peer_tcp), peer_context);
    handshake_cancel_progress progress;
    tls::detail::tls_dispatch_test_hooks hooks{
        &progress, nullptr, nullptr, handshake_cancel_progress::after_retry};
    server.set_dispatch_test_hooks(&hooks);
    server.set_output_test_hooks({&progress, handshake_cancel_progress::send,
                                  handshake_cancel_progress::allocate});
    runtime::scheduler scheduler(2);
    scheduler.start();
    std::atomic<bool> joined{false};
    bool server_result = true;
    bool launch_failed = false;
    bool operation_threw = false;
    scheduler.go([&]() -> coro::task<void> {
        std::array<std::optional<coro::join_handle<void>>, 2> tasks;
        try {
            tasks[0].emplace(scheduler.go_joinable([&]() -> coro::task<void> {
                server_result = co_await server.handshake(progress.cancel.get_token());
            }));
            tasks[1].emplace(scheduler.go_joinable([&]() -> coro::task<void> {
                (void)co_await peer.handshake(progress.cancel.get_token());
            }));
        } catch (...) {
            launch_failed = true;
            progress.cancel.cancel();
        }
        for (auto& task : tasks) {
            if (task) {
                try { co_await std::move(*task); } catch (...) { operation_threw = true; }
            }
        }
        joined.store(true, std::memory_order_release);
    });
    const auto deadline = std::chrono::steady_clock::now() + test::scaled_ms(15000);
    while (!joined.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < deadline)
        std::this_thread::yield();
    const bool completed = joined.load(std::memory_order_acquire);
    if (!completed) {
        progress.cancel.cancel();
        server.shutdown_socket();
        peer.shutdown_socket();
    }
    // Never destroy the hook, cancellation source or streams before cleanup.
    if (!scheduler.shutdown(test::scaled_ms(15000))) std::terminate();
    server.set_dispatch_test_hooks(nullptr);
    server.set_output_test_hooks({});
    const auto result = server.shutdown_state_for_test();
    const auto peer_state = peer.shutdown_state_for_test();
    server.shutdown_socket();
    peer.shutdown_socket();
    REQUIRE(completed);
    REQUIRE(joined.load(std::memory_order_acquire));
    REQUIRE_FALSE(launch_failed);
    REQUIRE_FALSE(operation_threw);
    REQUIRE(progress.queued.load(std::memory_order_acquire));
    REQUIRE(progress.cancelled_after_progress.load(std::memory_order_acquire));
    REQUIRE_FALSE(server_result);
    REQUIRE_FALSE(server.is_handshake_complete());
    REQUIRE(result.transport_error == ECANCELED);
    REQUIRE_FALSE(result.pump_active);
    REQUIRE_FALSE(peer_state.pump_active);
}
}

TEST_CASE("TLS handshake cancellation after retry progress settles ciphertext output",
          "[tls][handshake][cancel][issue-1215]") {
    auto versions = [](io::io_context::backend_type backend) {
        SECTION("TLS 1.2") { exercise_handshake_progress_cancel(backend, tls::tls_version::tls_1_2); }
        SECTION("TLS 1.3") { exercise_handshake_progress_cancel(backend, tls::tls_version::tls_1_3); }
    };
    SECTION("forced epoll") { versions(io::io_context::backend_type::epoll); }
    SECTION("forced io_uring") {
#if ELIO_HAS_IO_URING
        if (!io::io_uring_backend::is_available()) SKIP("io_uring unavailable on this host");
        versions(io::io_context::backend_type::io_uring);
#else
        SKIP("io_uring support is not compiled");
#endif
    }
}

TEST_CASE("TLS retry wait exception terminates the pending owner and deferred reader", "[tls][retry][issue-1215]") {
    for (bool cancellable : {false, true})
        exercise_retry(operation::write, SSL_ERROR_WANT_WRITE, false, cancellable, true);
}
