#pragma once

/// @file client_base.hpp
/// @brief Common configuration and utilities for HTTP-based clients
///
/// This file provides shared infrastructure for HTTP, WebSocket, and SSE clients:
/// - Base client configuration with common settings
/// - TLS context initialization utilities
/// - Connection utility functions

#include <elio/net/stream.hpp>
#include <elio/net/resolve.hpp>
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
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <utility>

namespace elio::http {

namespace detail {

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

/// Spawn a watchdog that shutdown(2)s `fd` after `timeout` elapses.
///
/// The returned join_handle must be awaited after the I/O operation completes;
/// the caller cancels `watchdog_token` to wake the watchdog early on success.
/// `timed_out` is set only when the deadline fired before cancellation.
inline coro::join_handle<void>
arm_fd_shutdown_watchdog(runtime::scheduler* sched,
                         int fd,
                         std::chrono::nanoseconds timeout,
                         coro::cancel_token watchdog_token,
                         std::shared_ptr<std::atomic<bool>> timed_out) {
    return sched->go_joinable(
        [fd, timeout, tok = std::move(watchdog_token),
         flag = std::move(timed_out)]() -> coro::task<void> {
            auto r = co_await elio::time::sleep_for(timeout, tok);
            if (r == coro::cancel_result::completed) {
                flag->store(true, std::memory_order_release);
                if (fd >= 0) {
                    ::shutdown(fd, SHUT_RDWR);
                }
            }
            co_return;
        });
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

/// Connect to a host with TLS context setup
/// @param host Hostname
/// @param port Port number
/// @param secure If true, use TLS
/// @param tls_ctx TLS context (required if secure)
/// @param connect_timeout TCP connect + TLS handshake timeout; <=0 disables
/// @return Connected stream or std::nullopt on error
inline coro::task<std::optional<net::stream>>
client_connect(std::string_view host, uint16_t port, bool secure,
               tls::tls_context* tls_ctx,
               net::resolve_options resolve_opts = net::default_cached_resolve_options(),
               bool rotate_resolved_addresses = true,
               std::chrono::nanoseconds connect_timeout = std::chrono::nanoseconds::zero(),
               coro::cancel_token token = {}) {

    if (token.is_cancelled()) {
        errno = ECANCELED;
        co_return std::nullopt;
    }

    auto addresses = co_await net::resolve_all(host, port, resolve_opts);
    if (token.is_cancelled()) {
        errno = ECANCELED;
        co_return std::nullopt;
    }
    if (addresses.empty()) {
        ELIO_LOG_ERROR("Failed to resolve {}:{}: {}", host, port, strerror(errno));
        co_return std::nullopt;
    }

    size_t offset = rotate_resolved_addresses
        ? detail::next_rotation_offset(std::string(host), port, addresses.size())
        : 0;

    auto* sched = runtime::scheduler::current();
    const bool deadline_enforced = sched != nullptr && connect_timeout.count() > 0;
    auto op_cancel_src = std::make_shared<coro::cancel_source>();
    auto timer_cancel_src = std::make_shared<coro::cancel_source>();
    auto timed_out = std::make_shared<std::atomic<bool>>(false);
    std::optional<coro::join_handle<void>> watchdog;
    auto user_cancel_registration =
        token.on_cancel([op_cancel_src]() { op_cancel_src->cancel(); });

    if (deadline_enforced) {
        watchdog.emplace(sched->go_joinable(
            [timeout = connect_timeout,
             timer_source = timer_cancel_src,
             op_source = op_cancel_src,
             flag = timed_out]() -> coro::task<void> {
                auto r = co_await elio::time::sleep_for(
                    timeout, timer_source->get_token());
                if (r == coro::cancel_result::completed) {
                    flag->store(true, std::memory_order_release);
                    op_source->cancel();
                }
                co_return;
            }));
    }

    auto stop_watchdog = [&]() -> coro::task<void> {
        timer_cancel_src->cancel();
        if (watchdog) {
            auto wd = std::move(*watchdog);
            watchdog.reset();
            co_await std::move(wd);
        }
        co_return;
    };

    auto stop_watchdog_preserving_errno = [&]() -> coro::task<void> {
        int saved_errno = errno;
        co_await stop_watchdog();
        errno = saved_errno;
        co_return;
    };

    auto check_timeout = [&]() -> bool {
        if (timed_out->load(std::memory_order_acquire)) {
            errno = ETIMEDOUT;
            return true;
        }
        return false;
    };

    auto check_cancelled = [&]() -> bool {
        if (token.is_cancelled()) {
            errno = ECANCELED;
            return true;
        }
        return false;
    };

    if (secure) {
        if (!tls_ctx) {
            errno = EINVAL;
            co_await stop_watchdog_preserving_errno();
            ELIO_LOG_ERROR("TLS context required for secure connection to {}:{}", host, port);
            co_return std::nullopt;
        }

        for (size_t i = 0; i < addresses.size(); ++i) {
            const auto& addr = addresses[(offset + i) % addresses.size()];
            std::optional<net::tcp_stream> tcp;
            tcp = co_await net::tcp_connect(addr, op_cancel_src->get_token());
            if (check_timeout()) {
                co_await stop_watchdog_preserving_errno();
                co_return std::nullopt;
            }
            if (check_cancelled()) {
                if (tcp) {
                    tcp->shutdown_socket();
                }
                co_await stop_watchdog_preserving_errno();
                co_return std::nullopt;
            }
            if (!tcp) {
                continue;
            }

            tls::tls_stream tls_stream(std::move(*tcp), *tls_ctx);
            tls_stream.set_hostname(host);
            auto hs = co_await tls_stream.handshake(op_cancel_src->get_token());
            if (check_timeout()) {
                co_await stop_watchdog_preserving_errno();
                co_return std::nullopt;
            }
            if (check_cancelled()) {
                tls_stream.shutdown_socket();
                co_await stop_watchdog_preserving_errno();
                co_return std::nullopt;
            }
            if (!hs) {
                continue;
            }

            co_await stop_watchdog();
            co_return net::stream(std::move(tls_stream));
        }

        int connect_errno = errno ? errno : ECONNREFUSED;
        co_await stop_watchdog();
        errno = connect_errno;
        ELIO_LOG_ERROR("Failed to connect to {}:{}: {}", host, port, strerror(errno));
        co_return std::nullopt;
    } else {
        for (size_t i = 0; i < addresses.size(); ++i) {
            const auto& addr = addresses[(offset + i) % addresses.size()];
            std::optional<net::tcp_stream> result;
            result = co_await net::tcp_connect(addr, op_cancel_src->get_token());
            if (check_timeout()) {
                co_await stop_watchdog_preserving_errno();
                co_return std::nullopt;
            }
            if (check_cancelled()) {
                if (result) {
                    result->shutdown_socket();
                }
                co_await stop_watchdog_preserving_errno();
                co_return std::nullopt;
            }
            if (result) {
                co_await stop_watchdog();
                co_return net::stream(std::move(*result));
            }
        }

        int connect_errno = errno ? errno : ECONNREFUSED;
        co_await stop_watchdog();
        errno = connect_errno;
        ELIO_LOG_ERROR("Failed to connect to {}:{}: {}", host, port, strerror(errno));
        co_return std::nullopt;
    }
}

} // namespace elio::http
