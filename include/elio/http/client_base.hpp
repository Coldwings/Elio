#pragma once

/// @file client_base.hpp
/// @brief Common configuration and utilities for HTTP-based clients
///
/// This file provides shared infrastructure for HTTP, WebSocket, and SSE clients:
/// - Base client configuration with common settings
/// - TLS context initialization utilities
/// - Connection utility functions

#include <elio/net/stream.hpp>
#include <elio/http/client_result.hpp>
#include <elio/net/resolve_wait.hpp>
#include <elio/tls/tls_context.hpp>
#include <elio/coro/cancel_token.hpp>
#include <elio/io/io_context.hpp>
#include <elio/coro/task.hpp>
#include <elio/log/macros.hpp>
#include <elio/runtime/scheduler.hpp>
#include <elio/time/timer.hpp>

#include <sys/socket.h>

#include <atomic>
#include <string>
#include <string_view>
#include <chrono>
#include <cstddef>
#include <exception>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <type_traits>
#include <unordered_map>
#include <utility>

namespace elio::http {

namespace detail {

#ifdef ELIO_RUNTIME_TEST_HOOKS
// Allows cancellation regression tests to wait until a client response recv
// has been staged. Keeping the hook here gives HTTP, WebSocket, and SSE the
// same synchronization contract.
inline std::atomic<bool> observe_client_response_read_entry_for_test{false};
inline std::atomic<bool> client_response_read_staged_for_test{false};
using fd_watchdog_wait_hook = coro::task<coro::cancel_result> (*)(
    std::chrono::nanoseconds, coro::cancel_token);
inline std::atomic<fd_watchdog_wait_hook> fd_watchdog_wait_for_test{nullptr};
inline std::atomic<size_t> fd_watchdog_shutdowns_for_test{0};
using setup_watchdog_wait_hook = coro::task<coro::cancel_result> (*)(
    std::chrono::steady_clock::time_point, coro::cancel_token);
inline std::atomic<setup_watchdog_wait_hook> setup_watchdog_wait_for_test{nullptr};
inline std::atomic<void (*)()> setup_watchdog_before_construct_for_test{nullptr};
inline std::atomic<coro::task<void> (*)()> setup_watchdog_before_start_for_test{nullptr};
inline std::atomic<coro::task<void> (*)()> setup_watchdog_after_start_for_test{nullptr};
inline std::atomic<void (*)()> setup_connect_entered_for_test{nullptr};
inline std::atomic<void(*)()> tls_setup_entered_for_test{nullptr};

inline void arm_client_response_read_observer_for_test() noexcept {
    if (observe_client_response_read_entry_for_test.load(
            std::memory_order_acquire)) {
        io::detail::arm_next_cancellable_recv_staged_for_test(
            client_response_read_staged_for_test);
    }
}
#endif

inline size_t next_rotation_offset(const std::string& host, uint16_t port, size_t count) {
    if (count == 0) {
        return 0;
    }

    static std::mutex mutex;
    static std::unordered_map<std::string, size_t> state;

    std::lock_guard<std::mutex> lock(mutex);
    std::string key = host + ":" + std::to_string(port);
    size_t& cursor = state[key];
    size_t offset = cursor % count;
    cursor = (cursor + 1) % count;
    return offset;
}

template<typename Abort = std::nullptr_t>
coro::task<void> fd_shutdown_watchdog_task(
        int fd, std::chrono::nanoseconds timeout, coro::cancel_token tok,
        std::shared_ptr<std::atomic<bool>> flag, Abort abort = nullptr) {
    static_assert(std::is_same_v<Abort, std::nullptr_t> ||
                  std::is_nothrow_invocable_v<Abort&>);
    auto interrupt = [&]() noexcept {
        if constexpr (std::is_same_v<Abort, std::nullptr_t>) {
            if (fd < 0) return;
            ::shutdown(fd, SHUT_RDWR);
        } else {
            std::invoke(abort);
        }
#ifdef ELIO_RUNTIME_TEST_HOOKS
        fd_watchdog_shutdowns_for_test.fetch_add(1, std::memory_order_relaxed);
#endif
    };
    coro::cancel_result r;
    try {
#ifdef ELIO_RUNTIME_TEST_HOOKS
        if (auto hook = fd_watchdog_wait_for_test.load(std::memory_order_acquire)) {
            r = co_await hook(timeout, tok);
        } else
#endif
        {
            r = co_await elio::time::sleep_for(timeout, tok);
        }
    } catch (...) {
        // The helper cannot join us until its sibling I/O returns.
        // A cleanup-time exception must not abort successful I/O.
        if (!tok.is_cancelled()) interrupt();
        throw;
    }
    if (r == coro::cancel_result::completed) {
        flag->store(true, std::memory_order_release);
        interrupt();
    }
    co_return;
}

/// Spawn a watchdog that shutdown(2)s `fd` after `timeout` elapses.
/// The caller cancels the token on completion and joins before releasing `fd`.
/// Timer exceptions interrupt active sibling I/O without setting `timed_out`.
/// An optional noexcept callback also records layered-stream abort state.
/// Its borrowed stream must remain unmoved/alive until the watchdog is joined;
/// callback destruction must not access that stream.
template<typename Abort = std::nullptr_t>
coro::join_handle<void>
arm_fd_shutdown_watchdog(runtime::scheduler* sched,
                         int fd,
                         std::chrono::nanoseconds timeout,
                         coro::cancel_token watchdog_token,
                         std::shared_ptr<std::atomic<bool>> timed_out,
                         Abort abort = nullptr) {
    // Construct the owning frame before admission: a lazy callable wrapper
    // could fail allocating it after the sibling I/O has already suspended.
    return sched->go_joinable(fd_shutdown_watchdog_task(
        fd, timeout, std::move(watchdog_token), std::move(timed_out), std::move(abort)));
}

// Admit the watchdog inside this frame, before invoking the factory. Creating
// the operation task can itself throw, so accepting a pre-built task is unsafe.
template<typename OperationFactory, typename Abort = std::nullptr_t>
coro::task<io::io_result> await_fd_operation_with_watchdog(
        OperationFactory operation, runtime::scheduler* scheduler, int fd,
        std::chrono::nanoseconds timeout, std::shared_ptr<std::atomic<bool>> timed_out,
        Abort abort = nullptr) {
    coro::cancel_source stop;
    auto watchdog = arm_fd_shutdown_watchdog(
        scheduler, fd, timeout, stop.get_token(), std::move(timed_out), std::move(abort));
    // Rejected admission returns an exceptional ready handle, not a throw.
    // Observe it before starting I/O that would otherwise have no watchdog.
    if (watchdog.is_ready()) watchdog.await_resume();
    io::io_result result{};
    std::exception_ptr failure;
    try {
        result = co_await std::invoke(operation);
    } catch (...) {
        failure = std::current_exception();
    }
    try {
        stop.cancel();
    } catch (...) {
        if (!failure) failure = std::current_exception();
    }
    try {
        co_await watchdog;
    } catch (...) {
        if (!failure) failure = std::current_exception();
    }
    if (failure) std::rethrow_exception(failure);
    co_return result;
}

inline void abort_stream_io(net::stream& stream) noexcept {
    int fd = stream.fd();
    if (fd >= 0) {
        ::shutdown(fd, SHUT_RDWR);
        stream.mark_externally_shut_down();
    }
}

inline size_t saturated_response_header_buffer_limit(
    size_t max_headers, size_t max_header_size) noexcept {
    constexpr size_t status_line_count = 1;
    constexpr size_t line_ending_size = 2;
    constexpr size_t terminal_header_ending_size = 2;
    constexpr size_t max_size = std::numeric_limits<size_t>::max();

    if (max_headers > max_size - status_line_count) {
        return max_size;
    }
    const size_t line_count = max_headers + status_line_count;

    if (max_header_size > max_size - line_ending_size) {
        return max_size;
    }
    const size_t max_line_with_ending = max_header_size + line_ending_size;

    if (line_count >
        (max_size - terminal_header_ending_size) / max_line_with_ending) {
        return max_size;
    }

    return line_count * max_line_with_ending + terminal_header_ending_size;
}

inline size_t buffered_response_header_line_size(
    std::string_view buffer) noexcept {
    size_t size = buffer.size();
    if (size > 0 && buffer.back() == '\r') {
        --size;
    }
    return size;
}

inline bool response_header_limits_exceeded(
    std::string_view buffer, size_t max_headers,
    size_t max_header_size) noexcept {
    if (buffer.size() >
        saturated_response_header_buffer_limit(max_headers, max_header_size)) {
        return true;
    }

    size_t line_start = 0;
    size_t line_index = 0;
    size_t header_count = 0;
    while (line_start < buffer.size()) {
        auto line_end = buffer.find("\r\n", line_start);
        if (line_end == std::string_view::npos) {
            if (line_index > 0 &&
                buffered_response_header_line_size(buffer.substr(line_start)) >
                    max_header_size) {
                return true;
            }
            return false;
        }

        const size_t line_size = line_end - line_start;
        if (line_index > 0) {
            if (line_size == 0) {
                return false;
            }
            if (line_size > max_header_size) {
                return true;
            }
            if (header_count >= max_headers) {
                return true;
            }
            ++header_count;
        }

        line_start = line_end + 2;
        ++line_index;
    }

    return false;
}

} // namespace detail

/// Base configuration shared by all HTTP-based clients
/// Can be embedded in more specific configuration structures
struct base_client_config {
    std::chrono::seconds connect_timeout{10};     ///< TCP connect + TLS handshake timeout; <=0 disables
    std::chrono::seconds read_timeout{30};        ///< Read timeout; <=0 disables
    size_t read_buffer_size = 8192;               ///< Read buffer size
    std::string user_agent;                          ///< User-Agent header (empty = no header)
    bool verify_certificate = true;               ///< Verify TLS certificates
    net::resolve_options resolve_options = net::default_cached_resolve_options();  ///< DNS resolve/cache behavior
    bool rotate_resolved_addresses = true;        ///< Rotate start index across resolved addresses

    // DoS protection limits
    size_t max_headers = 100;                     ///< Max number of response headers
    size_t max_header_size = 8192;                ///< Max size of a single header line (bytes)
    std::chrono::nanoseconds dns_timeout{0};      ///< DNS observer budget; <=0 disables, independent of TCP/TLS
    std::shared_ptr<net::resolve_domain> dns_domain{}; ///< Null selects the shared default admission domain
};

/// Initialize a TLS context for client use with default settings
/// @param ctx TLS context to initialize
/// @param verify_certificate Whether to verify server certificates
inline void init_client_tls_context(tls::tls_context& ctx, bool verify_certificate = true) {
    ctx.use_default_verify_paths();
    if (verify_certificate) {
        ctx.set_verify_mode(tls::verify_mode::peer);
    } else {
        ctx.set_verify_mode(tls::verify_mode::none);
    }
}

namespace detail {

inline coro::task<void> setup_watchdog_task(
        std::chrono::steady_clock::time_point deadline,
        std::shared_ptr<coro::cancel_source> timer_source,
        std::shared_ptr<coro::cancel_source> op_source,
        std::shared_ptr<std::atomic<bool>> flag) {
    try {
        coro::cancel_result result;
#ifdef ELIO_RUNTIME_TEST_HOOKS
        if (auto hook = setup_watchdog_wait_for_test.load(std::memory_order_acquire))
            result = co_await hook(deadline, timer_source->get_token());
        else
#endif
            result = co_await elio::time::sleep_for(
                deadline - std::chrono::steady_clock::now(), timer_source->get_token());
        if (result == coro::cancel_result::completed) {
            flag->store(true, std::memory_order_release);
            op_source->cancel();
        }
    } catch (...) {
        // A failed timer must release suspended setup before its result can be
        // observed. A cancellation callback must not replace the first failure.
        const auto failure = std::current_exception();
        try { op_source->cancel(); } catch (...) {}
        std::rethrow_exception(failure);
    }
}

inline coro::task<void> make_setup_watchdog(
        std::chrono::steady_clock::time_point deadline,
        std::shared_ptr<coro::cancel_source> timer_source,
        std::shared_ptr<coro::cancel_source> op_source,
        std::shared_ptr<std::atomic<bool>> flag) {
#ifdef ELIO_RUNTIME_TEST_HOOKS
    if (auto hook = setup_watchdog_before_construct_for_test.load(std::memory_order_acquire))
        hook();
#endif
    return setup_watchdog_task(deadline, std::move(timer_source),
                               std::move(op_source), std::move(flag));
}

// Transport-created roots have no caller alias and retire only after borrowed
// exchange operations settle. Keep this policy out of the public connector.
inline coro::task<client_result<net::stream>>
client_connect_result_impl(std::string_view host, uint16_t port, bool secure,
               tls::tls_context* tls_ctx,
               net::resolve_options resolve_opts = net::default_cached_resolve_options(),
               bool rotate_resolved_addresses = true,
               std::chrono::nanoseconds connect_timeout = std::chrono::nanoseconds::zero(),
               coro::cancel_token token = {},
               std::chrono::nanoseconds dns_timeout = std::chrono::nanoseconds::zero(),
               std::shared_ptr<net::resolve_domain> dns_domain = {},
               std::optional<std::chrono::steady_clock::time_point> acquisition_deadline = {},
               bool settled_root = false) {

    if (token.is_cancelled()) {
        co_return detail::make_client_error(ECANCELED, client_stage::resolve);
    }
    if (acquisition_deadline && *acquisition_deadline <= std::chrono::steady_clock::now()) {
        co_return detail::make_client_error(ETIMEDOUT, client_stage::resolve);
    }

    net::resolve_wait_options dns_options;
    dns_options.lookup = resolve_opts;
    dns_options.domain = std::move(dns_domain);
    if (dns_timeout.count() > 0) {
        const auto now = std::chrono::steady_clock::now();
        const auto remaining = std::chrono::steady_clock::time_point::max() - now;
        dns_options.deadline = dns_timeout >= remaining
            ? std::chrono::steady_clock::time_point::max() : now + dns_timeout;
    }
    if (acquisition_deadline && (!dns_options.deadline ||
                                *acquisition_deadline < *dns_options.deadline))
        dns_options.deadline = acquisition_deadline;
    auto resolved = co_await net::resolve_all(host, port, std::move(dns_options), token);
    if (!resolved) {
        co_return detail::make_client_error(resolved.error, client_stage::resolve);
    }
    if (token.is_cancelled()) {
        co_return detail::make_client_error(ECANCELED, client_stage::resolve);
    }
    if (acquisition_deadline && *acquisition_deadline <= std::chrono::steady_clock::now()) {
        co_return detail::make_client_error(ETIMEDOUT, client_stage::resolve);
    }
    auto addresses = std::move(resolved.addresses);

    size_t offset = rotate_resolved_addresses
        ? detail::next_rotation_offset(std::string(host), port, addresses.size())
        : 0;

    auto* sched = runtime::scheduler::current();
    auto setup_deadline = acquisition_deadline;
    if (connect_timeout.count() > 0) {
        const auto now = std::chrono::steady_clock::now();
        const auto remaining = std::chrono::steady_clock::time_point::max() - now;
        const auto cap = connect_timeout >= remaining
            ? std::chrono::steady_clock::time_point::max() : now + connect_timeout;
        if (!setup_deadline || cap < *setup_deadline) setup_deadline = cap;
    }
    const bool deadline_enforced = sched != nullptr && setup_deadline.has_value();
    auto op_cancel_src = std::make_shared<coro::cancel_source>();
    auto timer_cancel_src = std::make_shared<coro::cancel_source>();
    auto timed_out = std::make_shared<std::atomic<bool>>(false);
    std::optional<coro::join_handle<void>> watchdog;
    auto user_cancel_registration =
        token.on_cancel([op_cancel_src]() { op_cancel_src->cancel(); });

    if (deadline_enforced) {
#ifdef ELIO_RUNTIME_TEST_HOOKS
        if (auto hook = setup_watchdog_before_start_for_test.load(std::memory_order_acquire))
            co_await hook();
#endif
        // Construct the owning timer frame before independent admission and
        // observe rejection before starting any TCP/TLS sibling work.
        watchdog.emplace(sched->go_joinable(make_setup_watchdog(
            *setup_deadline, timer_cancel_src, op_cancel_src, timed_out)));
#ifdef ELIO_RUNTIME_TEST_HOOKS
        if (auto hook = setup_watchdog_after_start_for_test.load(std::memory_order_acquire))
            co_await hook();
#endif
        if (watchdog->is_ready()) {
            // Admission rejection is already destroyed, but an admitted timer
            // may publish its result before its owning detached frame retires.
            std::exception_ptr startup_failure;
            try { watchdog->await_resume(); }
            catch (...) { startup_failure = std::current_exception(); }
            try { co_await watchdog->wait_destroyed_async(); }
            catch (...) {
                if (!startup_failure) startup_failure = std::current_exception();
            }
            if (startup_failure) std::rethrow_exception(startup_failure);
        }
    }

    auto stopped_error = [&](client_stage stage) -> std::optional<client_error> {
        if (timed_out->load(std::memory_order_acquire) ||
            (setup_deadline && *setup_deadline <= std::chrono::steady_clock::now())) {
            return detail::make_client_error(ETIMEDOUT, stage);
        }
        if (token.is_cancelled()) {
            return detail::make_client_error(ECANCELED, stage);
        }
        return std::nullopt;
    };

    auto last_error = detail::make_client_error(ECONNREFUSED, client_stage::connect);

    auto connect_addresses = [&]() -> coro::task<client_result<net::stream>> {
#ifdef ELIO_RUNTIME_TEST_HOOKS
        if (auto hook = setup_connect_entered_for_test.load(std::memory_order_acquire)) hook();
#endif
        if (secure) {
            if (!tls_ctx) {
                co_return detail::make_client_error(EINVAL, client_stage::tls);
            }

            for (size_t i = 0; i < addresses.size(); ++i) {
                const auto& addr = addresses[(offset + i) % addresses.size()];
                std::optional<net::tcp_stream> tcp;
                tcp = co_await net::detail::tcp_retirement_access::connect(
                    addr, op_cancel_src->get_token(), settled_root);
                const int tcp_error = tcp ? 0 : (errno ? errno : ECONNREFUSED);
                if (auto error = stopped_error(client_stage::connect)) {
                    if (tcp) {
                        tcp->shutdown_socket();
                    }
                    co_return *error;
                }
                if (!tcp) {
                    last_error = detail::make_client_error(tcp_error, client_stage::connect);
                    continue;
                }

                tls::tls_stream tls_stream(std::move(*tcp), *tls_ctx);
                tls_stream.set_hostname(host);
#ifdef ELIO_RUNTIME_TEST_HOOKS
                if (auto hook = detail::tls_setup_entered_for_test.load(std::memory_order_acquire)) hook();
#endif
                auto hs = co_await tls_stream.handshake(op_cancel_src->get_token());
                const int tls_error = hs ? 0 : (errno ? errno : EIO);
                if (auto error = stopped_error(client_stage::tls)) {
                    tls_stream.shutdown_socket();
                    co_return *error;
                }
                if (!hs) {
                    last_error = detail::make_client_error(tls_error, client_stage::tls);
                    continue;
                }

                co_return net::stream(std::move(tls_stream));
            }

            co_return last_error;
        } else {
            for (size_t i = 0; i < addresses.size(); ++i) {
                const auto& addr = addresses[(offset + i) % addresses.size()];
                std::optional<net::tcp_stream> result;
                result = co_await net::detail::tcp_retirement_access::connect(
                    addr, op_cancel_src->get_token(), settled_root);
                const int tcp_error = result ? 0 : (errno ? errno : ECONNREFUSED);
                if (auto error = stopped_error(client_stage::connect)) {
                    if (result) {
                        result->shutdown_socket();
                    }
                    co_return *error;
                }
                if (result) {
                    co_return net::stream(std::move(*result));
                }
                last_error = detail::make_client_error(tcp_error, client_stage::connect);
            }

            co_return last_error;
        }
    };

    std::optional<client_result<net::stream>> connected;
    std::exception_ptr setup_failure;
    try {
        connected = co_await connect_addresses();
    } catch (...) {
        setup_failure = std::current_exception();
    }
    // Stop the owned timer before allocating any cleanup frame, and preserve
    // the first setup failure even if cancellation or watchdog retrieval throws.
    try {
        timer_cancel_src->cancel();
    } catch (...) {
        if (!setup_failure) setup_failure = std::current_exception();
    }
    if (watchdog) {
        auto wd = std::move(*watchdog);
        watchdog.reset();
        try {
            co_await wd;
        } catch (...) {
            if (!setup_failure) setup_failure = std::current_exception();
        }
        try {
            co_await wd.wait_destroyed_async();
        } catch (...) {
            if (!setup_failure) setup_failure = std::current_exception();
        }
    }
    if (setup_failure) std::rethrow_exception(setup_failure);
    co_return std::move(*connected);
}

} // namespace detail

/// Connect to a host with TLS context setup
/// @param host Hostname
/// @param port Port number
/// @param secure If true, use TLS
/// @param tls_ctx TLS context (required if secure)
/// @param connect_timeout TCP connect + TLS handshake timeout; <=0 disables
/// @param dns_timeout Independent DNS observer timeout; <=0 disables
/// @param dns_domain Shared DNS admission; null selects the default
/// @param acquisition_deadline Optional absolute queue-inclusive budget supplied
/// by the Transport. DNS/connect caps can shorten it, never restart or extend it.
/// @return Connected stream or owned operational error; setup exceptions may throw.
inline coro::task<client_result<net::stream>>
client_connect_result(std::string_view host, uint16_t port, bool secure,
               tls::tls_context* tls_ctx,
               net::resolve_options resolve_opts = net::default_cached_resolve_options(),
               bool rotate_resolved_addresses = true,
               std::chrono::nanoseconds connect_timeout = std::chrono::nanoseconds::zero(),
               coro::cancel_token token = {},
               std::chrono::nanoseconds dns_timeout = std::chrono::nanoseconds::zero(),
               std::shared_ptr<net::resolve_domain> dns_domain = {},
               std::optional<std::chrono::steady_clock::time_point> acquisition_deadline = {}) {
    return detail::client_connect_result_impl(host, port, secure, tls_ctx,
        resolve_opts, rotate_resolved_addresses, connect_timeout, std::move(token),
        dns_timeout, std::move(dns_domain), acquisition_deadline);
}

/// Compatibility wrapper; capture errno immediately on an empty result.
inline coro::task<std::optional<net::stream>>
client_connect(std::string_view host, uint16_t port, bool secure,
               tls::tls_context* tls_ctx,
               net::resolve_options resolve_opts = net::default_cached_resolve_options(),
               bool rotate_resolved_addresses = true,
               std::chrono::nanoseconds connect_timeout = std::chrono::nanoseconds::zero(),
               coro::cancel_token token = {},
               std::chrono::nanoseconds dns_timeout = std::chrono::nanoseconds::zero(),
               std::shared_ptr<net::resolve_domain> dns_domain = {}) {
    auto result = co_await client_connect_result(host, port, secure, tls_ctx,
        resolve_opts, rotate_resolved_addresses, connect_timeout, std::move(token),
        dns_timeout, std::move(dns_domain));
    if (const auto* error = std::get_if<client_error>(&result)) {
        errno = error->code.value();
        co_return std::nullopt;
    }
    co_return std::move(std::get<net::stream>(result));
}

} // namespace elio::http
