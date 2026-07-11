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
#include <chrono>
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
               std::chrono::nanoseconds connect_timeout = std::chrono::nanoseconds::zero()) {

    auto addresses = co_await net::resolve_all(host, port, resolve_opts);
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
            if (deadline_enforced) {
                tcp = co_await net::tcp_connect(addr, op_cancel_src->get_token());
            } else {
                tcp = co_await net::tcp_connect(addr);
            }
            if (check_timeout()) {
                co_await stop_watchdog_preserving_errno();
                co_return std::nullopt;
            }
            if (!tcp) {
                continue;
            }

            tls::tls_stream tls_stream(std::move(*tcp), *tls_ctx);
            tls_stream.set_hostname(host);
            auto hs = deadline_enforced
                ? co_await tls_stream.handshake(op_cancel_src->get_token())
                : co_await tls_stream.handshake();
            if (check_timeout()) {
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
            if (deadline_enforced) {
                result = co_await net::tcp_connect(addr, op_cancel_src->get_token());
            } else {
                result = co_await net::tcp_connect(addr);
            }
            if (check_timeout()) {
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
