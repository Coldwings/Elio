#pragma once

#include <elio/tls/tls_context.hpp>
#include <elio/tls/detail/tls_transport.hpp>
#include <elio/net/tcp.hpp>
#include <elio/net/resolve.hpp>
#include <elio/io/io_context.hpp>
#include <elio/runtime/spawn.hpp>
#include <elio/coro/task.hpp>
#include <elio/time/timer.hpp>
#include <elio/log/macros.hpp>

#include <openssl/ssl.h>
#include <openssl/err.h>

#include <sys/socket.h>

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <string>
#include <string_view>
#include <memory>
#include <mutex>
#include <optional>
#include <span>

namespace elio::tls {

#ifdef ELIO_RUNTIME_TEST_HOOKS
namespace detail {
enum class tls_test_operation { read, write };
struct tls_test_call_result {
    int ret;
    int error;
};
struct tls_dispatch_test_hooks {
    void* context = nullptr;
    tls_test_call_result (*dispatch)(void*, tls_test_operation, const void*, size_t) noexcept = nullptr;
    coro::task<io::io_result> (*readiness)(void*, tls_test_operation, bool, coro::cancel_token) = nullptr;
    void (*after_handshake_retry)(void*) noexcept = nullptr;
};
struct tls_shutdown_test_state {
    int ssl_shutdown_flags;
    int transport_error;
    bool pump_active;
};
} // namespace detail
#endif

/// TLS handshake result
enum class handshake_result {
    success,
    want_read,
    want_write,
    error
};

struct tls_stream_options {
    // Retained custom-BIO ciphertext payload; excludes OpenSSL/control overhead.
    size_t ciphertext_budget = 1024 * 1024;
};

/// TLS stream wrapping a TCP connection with SSL/TLS encryption
///
/// **Thread safety:** after handshake, one reader and one writer may overlap.
/// SSL dispatch and pending-write retry ownership are serialized internally.
/// Callers serialize handshake, multiple readers/writers, moves and destruction
/// against public operations, and keep borrowed buffers alive until completion.
///
/// **Output lifetime:** an internal pump owns only ciphertext and transport
/// state, not this SSL object or caller plaintext. Failed operations abort and
/// await its I/O leases. Destruction requests abort, never asynchronous normal
/// finalization; transport ownership survives outstanding internal cleanup.
/// Cancellation does not authorize destruction of active public task frames.
class tls_stream {
public:
#ifdef ELIO_RUNTIME_TEST_HOOKS
    void set_dispatch_test_hooks(detail::tls_dispatch_test_hooks* hooks) noexcept {
        dispatch_test_hooks_ = hooks;
    }
    void set_output_test_hooks(detail::output_bio_state::test_hooks hooks) {
        auto lock = lock_ssl_state();
        transport_->output.set_test_hooks(hooks);
    }
    detail::tls_shutdown_test_state shutdown_state_for_test() const {
        auto lock = lock_ssl_state();
        return {SSL_get_shutdown(ssl_), transport_->output.error(),
                transport_->output_active_for_test()};
    }
    void set_shutdown_timer_test_hook(void (*hook)()) noexcept {
        shutdown_timer_test_hook_ = hook;
    }
#endif
    /// Create a TLS stream from an existing TCP stream
    /// @param tcp The underlying TCP stream (takes ownership)
    /// @param ctx TLS context to use
    tls_stream(net::tcp_stream tcp, tls_context& ctx, tls_stream_options options = {})
        : transport_(std::make_shared<detail::tls_transport>(
              std::move(tcp), options.ciphertext_budget)) {
        ssl_ = SSL_new(ctx.native_handle());
        if (!ssl_) throw std::runtime_error("Failed to create SSL object");
        if (SSL_set_fd(ssl_, transport_->tcp.fd()) != 1) {
            SSL_free(std::exchange(ssl_, nullptr));
            throw std::runtime_error("Failed to attach TLS input");
        }
        BIO* output = transport_->output.make_bio();
        if (!output) {
            SSL_free(std::exchange(ssl_, nullptr));
            throw std::bad_alloc();
        }
        SSL_set0_wbio(ssl_, output);
        SSL_set_mode(ssl_, SSL_MODE_ENABLE_PARTIAL_WRITE);
        mode_ = ctx.mode();
        if (mode_ == tls_mode::client) SSL_set_connect_state(ssl_);
        else SSL_set_accept_state(ssl_);
    }

    ~tls_stream() { release_ssl(); }
    tls_stream(const tls_stream&) = delete;
    tls_stream& operator=(const tls_stream&) = delete;

    tls_stream(tls_stream&& other) noexcept
        : transport_(std::move(other.transport_))
        , ssl_(std::exchange(other.ssl_, nullptr))
        , write_retry_exclusive_(std::exchange(other.write_retry_exclusive_, false))
        , mode_(other.mode_)
        , handshake_complete_(std::exchange(other.handshake_complete_, false))
        , shutdown_sent_(std::exchange(other.shutdown_sent_, false))
        , externally_shut_down_(other.externally_shut_down_.load(std::memory_order_acquire))
        , hostname_(std::move(other.hostname_)) {}

    tls_stream& operator=(tls_stream&& other) noexcept {
        if (this != &other) {
            release_ssl();
            transport_ = std::move(other.transport_);
            ssl_ = std::exchange(other.ssl_, nullptr);
            write_retry_exclusive_ = std::exchange(other.write_retry_exclusive_, false);
            mode_ = other.mode_;
            handshake_complete_ = std::exchange(other.handshake_complete_, false);
            shutdown_sent_ = std::exchange(other.shutdown_sent_, false);
            externally_shut_down_.store(
                other.externally_shut_down_.load(std::memory_order_acquire),
                std::memory_order_release);
            hostname_ = std::move(other.hostname_);
        }
        return *this;
    }

    /// Set SNI hostname (for client connections)
    void set_hostname(std::string_view hostname) {
        hostname_ = std::string(hostname);
        auto lock = lock_ssl_state();
        // Set SNI extension
        SSL_set_tlsext_host_name(ssl_, hostname_.c_str());

        // Configure hostname verification for OpenSSL 1.1.0+
        X509_VERIFY_PARAM* param = SSL_get0_param(ssl_);
        X509_VERIFY_PARAM_set1_host(param, hostname_.c_str(), hostname_.size());
    }
    
    /// Perform the local TLS handshake; the owned output pump also drives
    /// final handshake/control records without retaining caller buffers.
    coro::task<bool> handshake() { return handshake({}); }

    coro::task<bool> handshake(coro::cancel_token token) {
        int exception_error = EIO;
        try {
            if (token.is_cancelled()) { errno = ECANCELED; co_return false; }
            for (;;) {
                if (token.is_cancelled()) {
                    co_await fail_io(ECANCELED);
                    errno = transport_error();
                    co_return false;
                }
                const auto step = call_ssl([&] {
                    return mode_ == tls_mode::client ? SSL_connect(ssl_) : SSL_accept(ssl_);
                });
                if (const int error = transport_error()) {
                    co_await fail_io(error);
                    errno = error;
                    co_return false;
                }
                if (step.ret == 1) {
                    handshake_complete_ = true;
                    co_return true;
                }
                const auto ready = co_await retry_io(step, token);
                if (ready.result < 0) {
                    co_await fail_io(-ready.result);
                    errno = -ready.result;
                    co_return false;
                }
#ifdef ELIO_RUNTIME_TEST_HOOKS
                if (dispatch_test_hooks_ && dispatch_test_hooks_->after_handshake_retry)
                    dispatch_test_hooks_->after_handshake_retry(dispatch_test_hooks_->context);
#endif
            }
        } catch (const std::bad_alloc&) { exception_error = ENOMEM; }
        catch (...) {}
        transport_->fail(exception_error);
        co_await transport_->settle_output();
        errno = transport_error();
        co_return false;
    }

    /// Read one available plaintext slice. Successful reads do not wait for
    /// outgoing control records: the owned ciphertext pump keeps those moving.
    coro::task<io::io_result> read(void* buffer, size_t length) {
        return read(buffer, length, {});
    }

    coro::task<io::io_result> read(void* buffer, size_t length,
                                   coro::cancel_token token) {
        int exception_error = EIO;
        try {
            if (length > static_cast<size_t>(INT32_MAX))
                co_return io::io_result{-EOVERFLOW, 0};
            if (!length) co_return io::io_result{0, 0};
            if (token.is_cancelled()) co_return io::io_result{-ECANCELED, 0};
            if (!handshake_complete_ && !(co_await handshake(token)))
                co_return io::io_result{-errno, 0};
            for (;;) {
                if (const int error = transport_error()) co_return co_await fail_io(error);
                if (token.is_cancelled()) co_return co_await fail_io(ECANCELED);
                const auto step = call_read(buffer, length);
                if (const int error = transport_error()) co_return co_await fail_io(error);
                if (step.ret > 0) co_return io::io_result{step.ret, 0};
                if (step.err == SSL_ERROR_ZERO_RETURN) co_return io::io_result{0, 0};
                io::io_result ready;
#ifdef ELIO_RUNTIME_TEST_HOOKS
                if (test_retry(step.err)) {
                    ready = co_await dispatch_test_hooks_->readiness(
                        dispatch_test_hooks_->context, detail::tls_test_operation::read,
                        step.err == SSL_ERROR_WANT_READ, token);
                } else
#endif
                ready = co_await retry_io(step, token);
                if (ready.result < 0) co_return co_await fail_io(-ready.result);
            }
        } catch (const std::bad_alloc&) { exception_error = ENOMEM; }
        catch (...) {}
        transport_->fail(exception_error);
        co_await transport_->settle_output();
        co_return io::io_result{-transport_error(), 0};
    }

    /// Write at most one application-record-sized plaintext slice. A positive
    /// result means its ciphertext reached the socket, not merely the BIO queue.
    coro::task<io::io_result> write(const void* buffer, size_t length) {
        return write(buffer, length, {});
    }

    coro::task<io::io_result> write(const void* buffer, size_t length,
                                    coro::cancel_token token) {
        int exception_error = EIO;
        try {
            if (length > static_cast<size_t>(INT32_MAX))
                co_return io::io_result{-EOVERFLOW, 0};
            if (!length) co_return io::io_result{0, 0};
            if (token.is_cancelled()) co_return io::io_result{-ECANCELED, 0};
            if (!handshake_complete_ && !(co_await handshake(token)))
                co_return io::io_result{-errno, 0};
            length = std::min<size_t>(length, 16384);
            for (;;) {
                if (const int error = transport_error()) co_return co_await fail_io(error);
                if (token.is_cancelled()) co_return co_await fail_io(ECANCELED);
                const auto step = call_write(buffer, length);
                if (const int error = transport_error()) co_return co_await fail_io(error);
                if (step.ret > 0) {
                    auto flushed = co_await transport_->flush_to(step.watermark, token);
                    if (flushed.result < 0) co_return co_await fail_io(-flushed.result);
                    co_return io::io_result{step.ret, 0};
                }
                io::io_result ready;
#ifdef ELIO_RUNTIME_TEST_HOOKS
                if (test_retry(step.err)) {
                    ready = co_await dispatch_test_hooks_->readiness(
                        dispatch_test_hooks_->context, detail::tls_test_operation::write,
                        step.err == SSL_ERROR_WANT_READ, token);
                } else
#endif
                ready = co_await retry_io(step, token);
                if (ready.result < 0) co_return co_await fail_io(-ready.result);
            }
        } catch (const std::bad_alloc&) { exception_error = ENOMEM; }
        catch (...) {}
        transport_->fail(exception_error);
        co_await transport_->settle_output();
        co_return io::io_result{-transport_error(), 0};
    }

    /// Write string data
    coro::task<io::io_result> write(std::string_view data) {
        return write(data.data(), data.size());
    }

    /// Write string data, cancellable by ``token``.
    coro::task<io::io_result> write(std::string_view data,
                                    coro::cancel_token token) {
        return write(data.data(), data.size(), std::move(token));
    }

    /// Borrowed-vector fallback: write the first nonempty slice, without
    /// concatenating payload. May return short progress; no record count is
    /// promised. The caller advances the vector cursor after a positive result.
    coro::task<io::io_result> writev(struct iovec* parts, size_t count) {
        auto bounds = net::detail::validate_stream_iovecs(parts, count);
        if (bounds.result <= 0) co_return bounds;
        for (size_t i = 0; i < count; ++i) {
            if (parts[i].iov_len != 0) {
                co_return co_await write(parts[i].iov_base, parts[i].iov_len);
            }
        }
        co_return io::io_result{0, 0};
    }

    /// Same borrowed-vector semantics, with cancellation forwarded to write.
    coro::task<io::io_result> writev(struct iovec* parts, size_t count,
                                    coro::cancel_token token) {
        if (token.is_cancelled()) co_return io::io_result{-ECANCELED, 0};
        auto bounds = net::detail::validate_stream_iovecs(parts, count);
        if (bounds.result <= 0) co_return bounds;
        for (size_t i = 0; i < count; ++i) {
            if (parts[i].iov_len != 0) {
                co_return co_await write(parts[i].iov_base, parts[i].iov_len, token);
            }
        }
        co_return io::io_result{0, 0};
    }

    /// Read exactly ``length`` bytes into ``buffer``.
    ///
    /// Loops over partial reads until ``length`` bytes have been stored, a
    /// terminal error occurs, or the peer closes the connection (EOF). The
    /// underlying TLS ``read`` already waits for readiness internally, so no
    /// ``-EAGAIN`` / ``-EWOULDBLOCK`` is ever surfaced here.
    ///
    /// @return ``io_result`` whose ``result`` is ``length`` on success. If the
    ///         peer closes before ``length`` bytes arrive, returns ``-ENODATA``
    ///         (short read / unexpected EOF). Any other terminal error from the
    ///         underlying ``read`` is returned as-is.
    coro::task<io::io_result> read_exactly(void* buffer, size_t length) {
        if (length > static_cast<size_t>(INT32_MAX)) {
            co_return io::io_result{-EOVERFLOW, 0};
        }

        auto* ptr = static_cast<char*>(buffer);
        size_t remaining = length;

        while (remaining > 0) {
            auto result = co_await read(ptr, remaining);
            if (result.result > 0) {
                ptr += result.result;
                remaining -= static_cast<size_t>(result.result);
            } else if (result.result == 0) {
                // Clean EOF before the requested count was satisfied.
                co_return io::io_result{-ENODATA, 0};
            } else {
                // Terminal error (TLS read never returns -EAGAIN).
                co_return result;
            }
        }
        co_return io::io_result{static_cast<int32_t>(length), 0};
    }

    /// Read exactly ``length`` bytes into ``buffer``, cancellable by ``token``.
    coro::task<io::io_result> read_exactly(void* buffer, size_t length,
                                           coro::cancel_token token) {
        if (length > static_cast<size_t>(INT32_MAX)) {
            co_return io::io_result{-EOVERFLOW, 0};
        }

        auto* ptr = static_cast<char*>(buffer);
        size_t remaining = length;

        while (remaining > 0) {
            if (token.is_cancelled()) {
                co_return io::io_result{-ECANCELED, 0};
            }

            auto result = co_await read(ptr, remaining, token);
            if (result.result > 0) {
                ptr += result.result;
                remaining -= static_cast<size_t>(result.result);
            } else if (result.result == 0) {
                co_return io::io_result{-ENODATA, 0};
            } else {
                co_return result;
            }
        }
        co_return io::io_result{static_cast<int32_t>(length), 0};
    }

    /// Read exactly enough bytes to fill ``buffer``.
    template<typename T>
    coro::task<io::io_result> read_exactly(std::span<T> buffer) {
        return read_exactly(buffer.data(), buffer.size_bytes());
    }

    /// Read exactly enough bytes to fill ``buffer``, cancellable by ``token``.
    template<typename T>
    coro::task<io::io_result> read_exactly(std::span<T> buffer,
                                           coro::cancel_token token) {
        return read_exactly(buffer.data(), buffer.size_bytes(), std::move(token));
    }

    /// Write exactly ``length`` bytes from ``buffer``.
    ///
    /// Loops over partial writes until ``length`` bytes have been accepted or a
    /// terminal error occurs. The underlying TLS ``write`` already waits for
    /// readiness internally, so no ``-EAGAIN`` / ``-EWOULDBLOCK`` is ever
    /// surfaced here.
    ///
    /// @return ``io_result`` whose ``result`` is ``length`` on success, or the
    ///         failing ``io_result`` (``result <= 0``) on a terminal error,
    ///         preserving the real error code.
    coro::task<io::io_result> write_exactly(const void* buffer, size_t length) {
        if (length > static_cast<size_t>(INT32_MAX)) {
            co_return io::io_result{-EOVERFLOW, 0};
        }

        const auto* ptr = static_cast<const char*>(buffer);
        size_t remaining = length;

        while (remaining > 0) {
            auto result = co_await write(ptr, remaining);
            if (result.result > 0) {
                ptr += result.result;
                remaining -= static_cast<size_t>(result.result);
            } else {
                // Terminal error (TLS write never returns -EAGAIN).
                co_return result;
            }
        }
        co_return io::io_result{static_cast<int32_t>(length), 0};
    }

    /// Write exactly ``length`` bytes from ``buffer``, cancellable by ``token``.
    coro::task<io::io_result> write_exactly(const void* buffer, size_t length,
                                            coro::cancel_token token) {
        if (length > static_cast<size_t>(INT32_MAX)) {
            co_return io::io_result{-EOVERFLOW, 0};
        }

        const auto* ptr = static_cast<const char*>(buffer);
        size_t remaining = length;

        while (remaining > 0) {
            if (token.is_cancelled()) {
                co_return io::io_result{-ECANCELED, 0};
            }

            auto result = co_await write(ptr, remaining, token);
            if (result.result > 0) {
                ptr += result.result;
                remaining -= static_cast<size_t>(result.result);
            } else {
                co_return result;
            }
        }
        co_return io::io_result{static_cast<int32_t>(length), 0};
    }

    /// Write exactly all bytes from ``buffer``.
    template<typename T>
    coro::task<io::io_result> write_exactly(std::span<const T> buffer) {
        return write_exactly(buffer.data(), buffer.size_bytes());
    }

    /// Write exactly all bytes from ``buffer``, cancellable by ``token``.
    template<typename T>
    coro::task<io::io_result> write_exactly(std::span<const T> buffer,
                                            coro::cancel_token token) {
        return write_exactly(buffer.data(), buffer.size_bytes(), std::move(token));
    }

    /// Write exactly all bytes from ``str``.
    coro::task<io::io_result> write_exactly(std::string_view str) {
        return write_exactly(str.data(), str.size());
    }

    /// Write exactly all bytes from ``str``, cancellable by ``token``.
    coro::task<io::io_result> write_exactly(std::string_view str,
                                            coro::cancel_token token) {
        return write_exactly(str.data(), str.size(), std::move(token));
    }

    /// Complete whole-session TLS shutdown under one close budget. Timeout
    /// begins abort processing, not asynchronous destruction of pending I/O.
    /// This legacy void API does not report peer receipt or lossless delivery.
    coro::task<void> shutdown(std::chrono::milliseconds timeout =
                                  std::chrono::milliseconds(5000)) {
        if (!ssl_ || !handshake_complete_) co_return;
        if (is_socket_closed_or_dead() || transport_error()) {
            transport_->fail(ECANCELED);
            co_await transport_->settle_output();
            handshake_complete_ = false;
            co_return;
        }
        const auto deadline = std::chrono::steady_clock::now() + timeout;
        std::optional<coro::cancel_source> cancel;
        std::optional<coro::join_handle<void>> watchdog;
        int error = 0;
        try {
            cancel.emplace();
            auto ticket = std::make_unique<close_watchdog_ticket>(transport_);
            watchdog.emplace(elio::spawn(close_watchdog(
                std::move(ticket), deadline, *cancel
#ifdef ELIO_RUNTIME_TEST_HOOKS
                , shutdown_timer_test_hook_
#endif
                )));
            for (;;) {
                if (const int failed = transport_error()) { error = failed; break; }
                if (cancel->is_cancelled()) { error = ETIMEDOUT; break; }
                auto step = call_ssl([&] { return SSL_shutdown(ssl_); }, false);
                auto flushed = co_await transport_->flush_to(step.watermark, cancel->get_token());
                if (flushed.result < 0) {
                    error = -flushed.result;
                    break;
                }
                if (step.ret >= 0) shutdown_sent_ = true;
                if (step.ret == 1) break;
                // Zero completes only our alert. Retry SSL before polling: the
                // peer's alert can already be buffered above the socket BIO.
                if (step.ret == 0) continue;
                auto ready = co_await retry_io(step, cancel->get_token());
                if (ready.result < 0) {
                    error = -ready.result;
                    break;
                }
            }
        } catch (const std::bad_alloc&) { error = ENOMEM; }
        catch (...) { error = EIO; }
        if (error) transport_->fail(error);
        if (cancel) { try { cancel->cancel(); } catch (...) { transport_->fail(EIO); } }
        if (watchdog) {
            try { co_await std::move(*watchdog); }
            catch (...) { transport_->fail(EIO); }
        }
        transport_->retire_output();
        co_await transport_->settle_output();
        handshake_complete_ = false;
    }
    
    /// Get negotiated ALPN protocol
    std::string_view alpn_protocol() const {
        const unsigned char* proto = nullptr;
        unsigned int len = 0;
        auto lock = lock_ssl_state();
        SSL_get0_alpn_selected(ssl_, &proto, &len);
        if (proto && len > 0) {
            return std::string_view(reinterpret_cast<const char*>(proto), len);
        }
        return {};
    }
    
    /// Get TLS version string
    const char* version() const {
        auto lock = lock_ssl_state();
        return SSL_get_version(ssl_);
    }
    
    /// Get cipher name
    const char* cipher() const {
        auto lock = lock_ssl_state();
        return SSL_get_cipher_name(ssl_);
    }
    
    /// Get underlying file descriptor
    int fd() const noexcept { return transport_ ? transport_->tcp.fd() : -1; }

    /// Get underlying TCP stream (const)
    const net::tcp_stream& tcp() const noexcept { return transport_->tcp; }

    /// Check if handshake is complete
    bool is_handshake_complete() const noexcept { return handshake_complete_; }

    /// Mark this stream as having had its socket shut down externally
    /// (e.g. by a slow-loris watchdog running on a different thread).
    ///
    /// After this call the destructor will skip ``SSL_shutdown`` because the
    /// underlying socket can no longer accept the close_notify write; on
    /// OpenSSL versions / libc builds that don't set ``MSG_NOSIGNAL`` the
    /// write would deliver SIGPIPE to the process.
    ///
    /// This method only flips a flag and is safe to call from any thread;
    /// it must, however, happen-before the destructor runs (typically by
    /// joining the watchdog coroutine before letting the stream go out of
    /// scope).
    void mark_externally_shut_down() noexcept {
        externally_shut_down_.store(true, std::memory_order_release);
    }

    /// Convenience: kernel-side ``::shutdown(fd, SHUT_RDWR)`` plus
    /// ``mark_externally_shut_down()``. Intended for watchdog code that
    /// needs to interrupt a pending recv on a different thread.
    void shutdown_socket() noexcept {
        mark_externally_shut_down();
        if (int fd = transport_->tcp.fd(); fd >= 0) {
            ::shutdown(fd, SHUT_RDWR);
        }
    }
    
    /// Get peer certificate (if any)
    X509* peer_certificate() const {
        auto lock = lock_ssl_state();
        return SSL_get_peer_certificate(ssl_);
    }
    
    /// Verify peer certificate result
    long verify_result() const {
        auto lock = lock_ssl_state();
        return SSL_get_verify_result(ssl_);
    }
    
private:
    struct close_watchdog_ticket {
        explicit close_watchdog_ticket(std::shared_ptr<detail::tls_transport> value)
            : transport(std::move(value)) {}
        ~close_watchdog_ticket() { if (!entered) transport->fail(EIO); }
        std::shared_ptr<detail::tls_transport> transport;
        bool entered = false;
    };

    static coro::task<void> close_watchdog(
        std::unique_ptr<close_watchdog_ticket> ticket,
        std::chrono::steady_clock::time_point deadline, coro::cancel_source cancel
#ifdef ELIO_RUNTIME_TEST_HOOKS
        , void (*timer_hook)()
#endif
    ) {
        ticket->entered = true;
        auto transport = ticket->transport;
        ticket.reset();
        int error = 0;
        try {
#ifdef ELIO_RUNTIME_TEST_HOOKS
            if (timer_hook) timer_hook();
#endif
            const auto now = std::chrono::steady_clock::now();
            if (now >= deadline ||
                co_await time::sleep_for(deadline - now, cancel.get_token()) ==
                    coro::cancel_result::completed)
                error = ETIMEDOUT;
        } catch (const std::bad_alloc&) { error = ENOMEM; }
        catch (...) { error = EIO; }
        if (error) {
            // Failure to arm the deadline must also terminate the close wait.
            transport->fail(error);
            try { cancel.cancel(); } catch (...) {}
        }
    }

#ifdef ELIO_RUNTIME_TEST_HOOKS
    bool test_retry(int error) const noexcept {
        return (error == SSL_ERROR_WANT_READ || error == SSL_ERROR_WANT_WRITE) &&
            dispatch_test_hooks_ && dispatch_test_hooks_->readiness;
    }
#endif

    static constexpr int retry_owner_blocked = -1001;
    struct ssl_call_result {
        int ret = 0;
        int err = SSL_ERROR_NONE;
        uint64_t observed = 0;
        uint64_t watermark = 0;
    };

    struct ssl_input_snapshot {
        uint64_t bytes;
        OSSL_HANDSHAKE_STATE state;
        int plaintext;
        int buffered;
        bool operator==(const ssl_input_snapshot&) const = default;
    };

    ssl_input_snapshot input_snapshot() const noexcept {
        return {BIO_number_read(SSL_get_rbio(ssl_)), SSL_get_state(ssl_),
                SSL_pending(ssl_), SSL_has_pending(ssl_)};
    }

    int transport_error() const {
        auto lock = lock_ssl_state();
        return transport_->operation_error();
    }

    coro::task<io::io_result> fail_io(int error) {
        transport_->fail(error);
        co_await transport_->settle_output();
        co_return io::io_result{-transport_error(), 0};
    }

    coro::task<io::io_result> retry_io(ssl_call_result step, coro::cancel_token token) {
        if (step.err == retry_owner_blocked)
            co_return co_await transport_->wait_change(step.observed, token);
        if (step.err == SSL_ERROR_WANT_READ)
            co_return co_await transport_->wait_read(step.observed, token);
        if (step.err == SSL_ERROR_WANT_WRITE) {
            // The custom output BIO never reports capacity retry. Preserve the
            // OpenSSL retry contract if another supported SSL layer requests it.
            auto flushed = co_await transport_->flush_to(step.watermark, token);
            if (flushed.result < 0) co_return flushed;
            co_return co_await transport_->wait_write(token);
        }
        co_return io::io_result{-EIO, 0};
    }

    ssl_call_result call_read(void* buffer, size_t length) {
        ssl_call_result step;
        bool progress = false;
        {
            auto lock = lock_ssl_state();
            step.observed = transport_->generation;
            if (transport_->operation_error()) return {-1, SSL_ERROR_SYSCALL};
            if (write_retry_exclusive_) {
                step.ret = -1;
                step.err = retry_owner_blocked;
                return step;
            }
            const auto before = input_snapshot();
#ifdef ELIO_RUNTIME_TEST_HOOKS
            if (dispatch_test_hooks_ && dispatch_test_hooks_->dispatch) {
                const auto result = dispatch_test_hooks_->dispatch(dispatch_test_hooks_->context,
                    detail::tls_test_operation::read, buffer, length);
                step.ret = result.ret;
                step.err = result.error;
            } else
#endif
            {
                ERR_clear_error();
                step.ret = SSL_read(ssl_, buffer, static_cast<int>(length));
                step.err = step.ret > 0 ? SSL_ERROR_NONE : SSL_get_error(ssl_, step.ret);
            }
            step.watermark = transport_->output.accepted_bytes();
            progress = step.ret > 0 || step.err == SSL_ERROR_ZERO_RETURN ||
                before != input_snapshot();
        }
        transport_->start_output();
        if (progress) transport_->notify_progress();
        return step;
    }

    ssl_call_result call_write(const void* buffer, size_t length) {
        ssl_call_result step;
        bool progress;
        {
            auto lock = lock_ssl_state();
            step.observed = transport_->generation;
            if (transport_->operation_error()) return {-1, SSL_ERROR_SYSCALL};
            const auto before = input_snapshot();
#ifdef ELIO_RUNTIME_TEST_HOOKS
            if (dispatch_test_hooks_ && dispatch_test_hooks_->dispatch) {
                const auto result = dispatch_test_hooks_->dispatch(dispatch_test_hooks_->context,
                    detail::tls_test_operation::write, buffer, length);
                step.ret = result.ret;
                step.err = result.error;
            } else
#endif
            {
                ERR_clear_error();
                step.ret = SSL_write(ssl_, buffer, static_cast<int>(length));
                step.err = step.ret > 0 ? SSL_ERROR_NONE : SSL_get_error(ssl_, step.ret);
            }
            const bool was_exclusive = write_retry_exclusive_;
            write_retry_exclusive_ = step.err == SSL_ERROR_WANT_WRITE;
            // A persistent pending-plaintext level is not new progress: using
            // it as a notification would turn WANT_READ into a self-wake loop.
            progress = step.ret > 0 || (was_exclusive && !write_retry_exclusive_) ||
                before != input_snapshot();
            step.watermark = transport_->output.accepted_bytes();
        }
        transport_->start_output();
        if (progress) transport_->notify_progress();
        return step;
    }

    std::unique_lock<std::mutex> lock_ssl_state() const {
        return std::unique_lock<std::mutex>(transport_->mutex);
    }

    template<typename F>
    ssl_call_result call_ssl(F&& fn, bool zero_is_error = true) {
        ssl_call_result step;
        {
            auto lock = lock_ssl_state();
            if (transport_->operation_error()) return {-1, SSL_ERROR_SYSCALL};
            ERR_clear_error();
            step.ret = fn();
            step.err = (step.ret < 0 || (step.ret == 0 && zero_is_error))
                ? SSL_get_error(ssl_, step.ret) : SSL_ERROR_NONE;
            step.observed = transport_->generation;
            step.watermark = transport_->output.accepted_bytes();
        }
        transport_->start_output();
        if (step.ret > 0) transport_->notify_progress();
        return step;
    }

    void release_ssl() noexcept {
        if (!ssl_) {
            return;
        }

        auto lock = lock_ssl_state();
        // If the user forgot to ``co_await stream.shutdown()``, at least
        // queue our close_notify alert so the peer can distinguish a
        // clean close from a MITM truncation. The underlying fd is
        // non-blocking, so a single ``SSL_shutdown`` call is bounded:
        // it serializes the alert and tries to write it; if the kernel
        // buffer is full we accept the loss rather than block here.
        // We never wait for the peer's close_notify in the destructor,
        // since that would require async I/O.
        //
        // Never re-enter SSL after a fatal result or an unfinished exclusive
        // retry. Known-dead sockets also cannot benefit from a closing alert;
        // the custom output BIO separately uses MSG_NOSIGNAL for every send.
        if (handshake_complete_ && !shutdown_sent_ && !write_retry_exclusive_ &&
            !transport_->output.error() &&
            !is_socket_closed_or_dead()) {
            ERR_clear_error();
            (void)SSL_shutdown(ssl_);
        }
        SSL_free(ssl_);
        ssl_ = nullptr;
        lock.unlock();
        transport_->fail(ECANCELED);
    }

    /// Cheap probe that returns true when the underlying socket is either
    /// already known (via the externally_shut_down_ flag) to be unusable,
    /// or when the kernel reports a pending error such as ECONNRESET/EPIPE.
    /// Used by the destructor to decide whether SSL_shutdown's close_notify
    /// write would be wasted (and potentially fatal via SIGPIPE).
    bool is_socket_closed_or_dead() const noexcept {
        if (externally_shut_down_.load(std::memory_order_acquire)) {
            return true;
        }
        int fd = transport_->tcp.fd();
        if (fd < 0) {
            return true;
        }
        int sock_err = 0;
        socklen_t len = sizeof(sock_err);
        if (::getsockopt(fd, SOL_SOCKET, SO_ERROR, &sock_err, &len) == 0
            && sock_err != 0) {
            return true;
        }
        return false;
    }

    static std::string get_ssl_error_string(int err) {
        switch (err) {
            case SSL_ERROR_NONE: return "none";
            case SSL_ERROR_SSL: {
                char buf[256];
                ERR_error_string_n(ERR_get_error(), buf, sizeof(buf));
                return buf;
            }
            case SSL_ERROR_WANT_READ: return "want_read";
            case SSL_ERROR_WANT_WRITE: return "want_write";
            case SSL_ERROR_WANT_X509_LOOKUP: return "want_x509_lookup";
            case SSL_ERROR_SYSCALL: return "syscall error: " + std::string(strerror(errno));
            case SSL_ERROR_ZERO_RETURN: return "zero_return";
            case SSL_ERROR_WANT_CONNECT: return "want_connect";
            case SSL_ERROR_WANT_ACCEPT: return "want_accept";
            default: return "unknown(" + std::to_string(err) + ")";
        }
    }
    
    std::shared_ptr<detail::tls_transport> transport_;
#ifdef ELIO_RUNTIME_TEST_HOOKS
    detail::tls_dispatch_test_hooks* dispatch_test_hooks_ = nullptr;
    void (*shutdown_timer_test_hook_)() = nullptr;
#endif
    SSL* ssl_ = nullptr;
    bool write_retry_exclusive_ = false;
    tls_mode mode_ = tls_mode::client;
    bool handshake_complete_ = false;
    bool shutdown_sent_ = false;  ///< True once our close_notify has been queued.
    /// Set by ``mark_externally_shut_down()`` / ``shutdown_socket()`` when a
    /// foreign actor (typically a watchdog on another thread) has already
    /// shut down the underlying TCP socket. The destructor reads this to
    /// avoid an SSL_shutdown() that would write to a half-closed socket.
    std::atomic<bool> externally_shut_down_{false};
    std::string hostname_;  // Store hostname for SNI and verification
};

/// Connect to a TLS server
/// @param ctx TLS context (client mode)
/// @param host Hostname to connect to
/// @param port Port to connect to
/// @return TLS stream on success, std::nullopt on error (check errno)
inline coro::task<std::optional<tls_stream>> 
tls_connect(tls_context& ctx,
            std::string_view host,
            uint16_t port,
            net::resolve_options resolve_opts = net::default_cached_resolve_options()) {
    auto resolved = co_await net::resolve_all(host, port, resolve_opts);
    if (resolved.empty()) {
        co_return std::nullopt;
    }

    for (const auto& addr : resolved) {
        auto tcp_result = co_await net::tcp_connect(addr);
        if (!tcp_result) {
            continue;
        }

        tls_stream stream(std::move(*tcp_result), ctx);
        stream.set_hostname(host);

        auto hs_result = co_await stream.handshake();
        if (!hs_result) {
            continue;
        }

        co_return std::move(stream);
    }

    co_return std::nullopt;
}

/// TLS listener for accepting secure connections
class tls_listener {
public:
    /// Create a TLS listener from a TCP listener and TLS context
    tls_listener(net::tcp_listener tcp, tls_context& ctx)
        : tcp_(std::move(tcp)), ctx_(&ctx) {}
    
    /// Accept a new TLS connection
    /// @return TLS stream on success, std::nullopt on error (check errno)
    coro::task<std::optional<tls_stream>> accept() {
        auto tcp_result = co_await tcp_.accept();
        if (!tcp_result) {
            co_return std::nullopt;
        }
        
        tls_stream stream(std::move(*tcp_result), *ctx_);
        
        // Perform handshake
        auto hs_result = co_await stream.handshake();
        if (!hs_result) {
            co_return std::nullopt;
        }
        
        co_return std::move(stream);
    }
    
    /// Get underlying file descriptor
    int fd() const noexcept { return tcp_.fd(); }
    
    /// Bind and create a TLS listener (IPv4)
    /// @return TLS listener on success, std::nullopt on error (check errno)
    static std::optional<tls_listener>
    bind(const net::ipv4_address& addr, tls_context& ctx) {
        auto tcp_result = net::tcp_listener::bind(addr);
        if (!tcp_result) {
            return std::nullopt;
        }
        return tls_listener(std::move(*tcp_result), ctx);
    }

    /// Bind and create a TLS listener (IPv6)
    static std::optional<tls_listener>
    bind(const net::ipv6_address& addr, tls_context& ctx) {
        auto tcp_result = net::tcp_listener::bind(addr);
        if (!tcp_result) {
            return std::nullopt;
        }
        return tls_listener(std::move(*tcp_result), ctx);
    }

    /// Bind and create a TLS listener (generic address)
    static std::optional<tls_listener>
    bind(const net::socket_address& addr, tls_context& ctx) {
        auto tcp_result = net::tcp_listener::bind(addr);
        if (!tcp_result) {
            return std::nullopt;
        }
        return tls_listener(std::move(*tcp_result), ctx);
    }
    
private:
    net::tcp_listener tcp_;
    tls_context* ctx_;
};

} // namespace elio::tls
