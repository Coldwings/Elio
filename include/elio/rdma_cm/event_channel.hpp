#pragma once

/// @file event_channel.hpp
/// @brief RAII wrapper for `rdma_event_channel*` with an
///        io_context-friendly `next_event()` coroutine.
///
/// `librdmacm` exposes async connection events through a single
/// `rdma_event_channel` per process (typically). The channel has an
/// fd that becomes readable when an event is pending; the
/// application then calls `rdma_get_cm_event()` to read it. This
/// wrapper sets the fd to non-blocking, polls it via Elio's
/// `io::async_poll_read`, and consumes one event per iteration.
///
/// **Lifetime**: the channel and its fd live as long as this object;
/// destruction calls `rdma_destroy_event_channel`. Any
/// `cm_id` objects derived from this channel MUST be destroyed before
/// the channel — librdmacm requires it.

#include <elio/coro/cancel_token.hpp>
#include <elio/coro/task.hpp>
#include <elio/io/io_awaitables.hpp>

#include <rdma/rdma_cma.h>

#include <fcntl.h>
#include <unistd.h>

#include <cerrno>
#include <cstring>
#include <stdexcept>
#include <string>
#include <utility>

namespace elio::rdma_cm {

class event_channel {
public:
    /// Create a new `rdma_event_channel` and set its fd non-blocking.
    /// Throws `std::runtime_error` on creation failure (errno set).
    event_channel()
        : channel_(::rdma_create_event_channel()) {
        if (!channel_) {
            const int e = errno;
            throw std::runtime_error(
                std::string("rdma_create_event_channel failed: ")
                + std::strerror(e));
        }
        // Non-blocking so rdma_get_cm_event returns EAGAIN instead of
        // blocking the worker. Errors here are non-fatal; we still
        // have a usable channel, just with worse poll behaviour, so
        // log via the exception path only on outright failure.
        const int flags = ::fcntl(channel_->fd, F_GETFL);
        if (flags >= 0) {
            (void)::fcntl(channel_->fd, F_SETFL, flags | O_NONBLOCK);
        }
    }

    event_channel(const event_channel&) = delete;
    event_channel& operator=(const event_channel&) = delete;

    event_channel(event_channel&& other) noexcept
        : channel_(other.channel_) {
        other.channel_ = nullptr;
    }

    event_channel& operator=(event_channel&& other) noexcept {
        if (this != &other) {
            destroy_();
            channel_ = other.channel_;
            other.channel_ = nullptr;
        }
        return *this;
    }

    ~event_channel() noexcept { destroy_(); }

    [[nodiscard]] rdma_event_channel* native() const noexcept {
        return channel_;
    }
    [[nodiscard]] int fd() const noexcept {
        return channel_ ? channel_->fd : -1;
    }

    /// Wait for the next CM event on this channel. Returns the
    /// event on success (the caller MUST ack it via
    /// `ack(event)` before the next call). Returns `nullptr` on
    /// cancellation or unrecoverable poll error. On success the
    /// caller owns the `rdma_cm_event*` until it acks.
    [[nodiscard]] coro::task<rdma_cm_event*>
    next_event(coro::cancel_token token = {}) {
        while (!token.is_cancelled()) {
            rdma_cm_event* event = nullptr;
            // Try a non-blocking read first: an event may already be
            // queued from a previous poll wake-up.
            const int rc = ::rdma_get_cm_event(channel_, &event);
            if (rc == 0) {
                co_return event;
            }
            if (errno != EAGAIN && errno != EWOULDBLOCK) {
                co_return nullptr;  // hard error
            }
            // No event ready; wait for the fd to become readable.
            auto result = co_await io::async_poll_read(channel_->fd);
            if (token.is_cancelled()) co_return nullptr;
            if (!result.success()) {
                const int err = result.error_code();
                if (err != EINTR && err != EAGAIN && err != ECANCELED) {
                    co_return nullptr;
                }
                // benign — retry
            }
        }
        co_return nullptr;
    }

    /// Acknowledge a CM event. The caller MUST call this exactly
    /// once per successful `next_event()` return, including for
    /// events the application doesn't otherwise act on.
    void ack(rdma_cm_event* event) noexcept {
        if (event) {
            (void)::rdma_ack_cm_event(event);
        }
    }

private:
    void destroy_() noexcept {
        if (channel_) {
            ::rdma_destroy_event_channel(channel_);
            channel_ = nullptr;
        }
    }

    rdma_event_channel* channel_ = nullptr;
};

}  // namespace elio::rdma_cm
