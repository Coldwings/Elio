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
#include <coroutine>
#include <cstring>
#include <memory>
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
        void* context, operation kind, bool for_read, coro::cancel_token) {
        auto& script = *static_cast<retry_script*>(context);
        script.direction_correct &= kind == script.pending_operation &&
            for_read == (script.pending_error == SSL_ERROR_WANT_READ);
        co_await gate{script};
        co_return io::io_result{1, 0};
    }
};

void exercise_retry(operation pending, int error, bool allow_interleaving, bool cancellable) {
    CAPTURE(pending, error, cancellable);
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
    tls::detail::tls_dispatch_test_hooks hooks{
        &script, retry_script::dispatch, retry_script::readiness};
    server.set_dispatch_test_hooks(&hooks);
    coro::cancel_source cancel;
    auto writer = cancellable ? server.write(payload, cancel.get_token()) : server.write(payload);
    auto reader = cancellable ? server.read(input.data(), input.size(), cancel.get_token())
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
    if (script.continuation) {
        auto resume = std::exchange(script.continuation, {});
        resume.resume();
    }
    const bool writer_done = writer_handle.done();
    const bool reader_done = reader_handle.done();
    server.set_dispatch_test_hooks(nullptr);
    server.shutdown_socket();
    peer.shutdown_socket();

    REQUIRE(writer_done);
    REQUIRE(reader_done);
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

TEST_CASE("TLS pending WANT_READ write allows eligible SSL reads", "[tls][retry][issue-1215]") {
    for (bool cancellable : {false, true})
        exercise_retry(operation::write, SSL_ERROR_WANT_READ, true, cancellable);
}

TEST_CASE("TLS pending read does not exclude SSL writes", "[tls][retry][issue-1215]") {
    for (bool cancellable : {false, true})
        exercise_retry(operation::read, SSL_ERROR_WANT_READ, true, cancellable);
}
