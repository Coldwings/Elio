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
/// cancellable `io::async_poll_read`, and consumes one event per iteration.
///
/// **Lifetime**: the channel and its fd live as long as this object;
/// destruction calls `rdma_destroy_event_channel`. Any
/// `cm_id` objects derived from this channel MUST be destroyed before
/// the channel — librdmacm requires it.

#include <elio/coro/cancel_token.hpp>
#include <elio/coro/task.hpp>
#include <elio/io/io_awaitables.hpp>
#include <elio/sync/mutex.hpp>

#include <rdma/rdma_cma.h>

#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <cerrno>
#include <deque>
#include <cstring>
#include <mutex>
#include <memory>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace elio::rdma_cm {

namespace detail {

class event_backlog {
public:
    event_backlog() = default;

    event_backlog(const event_backlog&) = delete;
    event_backlog& operator=(const event_backlog&) = delete;
    event_backlog(event_backlog&&) noexcept = default;
    event_backlog& operator=(event_backlog&&) noexcept = default;

    void stash(rdma_cm_event* event) {
        if (event) {
            events_.push_back(event);
        }
    }

    template<typename Predicate>
    [[nodiscard]] rdma_cm_event* take_if(Predicate&& pred) {
        for (auto it = events_.begin(); it != events_.end(); ++it) {
            if (pred(*it)) {
                rdma_cm_event* event = *it;
                events_.erase(it);
                return event;
            }
        }
        return nullptr;
    }

    [[nodiscard]] rdma_cm_event* pop_any() noexcept {
        if (events_.empty()) {
            return nullptr;
        }
        rdma_cm_event* event = events_.front();
        events_.pop_front();
        return event;
    }

    [[nodiscard]] bool empty() const noexcept { return events_.empty(); }
    [[nodiscard]] std::size_t size() const noexcept { return events_.size(); }

private:
    std::deque<rdma_cm_event*> events_;
};

class backlog_wait_registry {
public:
    class subscription {
    public:
        subscription() = default;
        subscription(backlog_wait_registry& registry,
                     std::shared_ptr<coro::cancel_source> source)
            : registry_(&registry),
              source_(std::move(source)) {}

        subscription(const subscription&) = delete;
        subscription& operator=(const subscription&) = delete;

        subscription(subscription&& other) noexcept
            : registry_(std::exchange(other.registry_, nullptr)),
              source_(std::move(other.source_)) {}

        subscription& operator=(subscription&& other) noexcept {
            if (this != &other) {
                reset();
                registry_ = std::exchange(other.registry_, nullptr);
                source_ = std::move(other.source_);
            }
            return *this;
        }

        ~subscription() { reset(); }

        void reset() noexcept {
            if (registry_ && source_) {
                registry_->unsubscribe(source_);
            }
            registry_ = nullptr;
            source_.reset();
        }

    private:
        backlog_wait_registry* registry_ = nullptr;
        std::shared_ptr<coro::cancel_source> source_{};
    };

    backlog_wait_registry() = default;

    backlog_wait_registry(const backlog_wait_registry&) = delete;
    backlog_wait_registry& operator=(const backlog_wait_registry&) = delete;
    backlog_wait_registry(backlog_wait_registry&&) = delete;
    backlog_wait_registry& operator=(backlog_wait_registry&&) = delete;

    [[nodiscard]] subscription subscribe(
        std::shared_ptr<coro::cancel_source> source) {
        if (!source) {
            return {};
        }
        {
            std::lock_guard<std::mutex> guard(mutex_);
            compact_locked_();
            waiters_.push_back(source);
        }
        return subscription(*this, std::move(source));
    }

    void cancel_all() {
        std::vector<std::shared_ptr<coro::cancel_source>> to_cancel;
        {
            std::lock_guard<std::mutex> guard(mutex_);
            for (auto it = waiters_.begin(); it != waiters_.end();) {
                if (auto source = it->lock()) {
                    to_cancel.push_back(std::move(source));
                    ++it;
                } else {
                    it = waiters_.erase(it);
                }
            }
        }
        for (auto& source : to_cancel) {
            source->cancel();
        }
    }

private:
    void unsubscribe(const std::shared_ptr<coro::cancel_source>& source) noexcept {
        std::lock_guard<std::mutex> guard(mutex_);
        waiters_.erase(
            std::remove_if(waiters_.begin(), waiters_.end(),
                [&](const std::weak_ptr<coro::cancel_source>& waiter) {
                    auto locked = waiter.lock();
                    return !locked || locked.get() == source.get();
                }),
            waiters_.end());
    }

    void compact_locked_() {
        waiters_.erase(
            std::remove_if(waiters_.begin(), waiters_.end(),
                [](const std::weak_ptr<coro::cancel_source>& waiter) {
                    return waiter.expired();
                }),
            waiters_.end());
    }

    std::mutex mutex_{};
    std::vector<std::weak_ptr<coro::cancel_source>> waiters_{};
};

}  // namespace detail

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
        : channel_(other.channel_),
          backlog_(std::move(other.backlog_)) {
        other.channel_ = nullptr;
    }

    event_channel& operator=(event_channel&& other) noexcept {
        if (this != &other) {
            destroy_();
            channel_ = other.channel_;
            backlog_ = std::move(other.backlog_);
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
        co_return co_await next_event_if(
            [](rdma_cm_event*) noexcept { return true; }, token);
    }

    /// Wait for the next CM event belonging to `id`. Events for other
    /// non-null ids are retained in this channel and can be consumed by
    /// their matching waiters later. Global/unowned events are returned
    /// to the current waiter so it can treat them as errors.
    [[nodiscard]] coro::task<rdma_cm_event*>
    next_event_for(rdma_cm_id* id,
                   coro::cancel_token token = {}) {
        if (!id) {
            co_return nullptr;
        }
        co_return co_await next_event_matching_(
            [id](rdma_cm_event* event) noexcept {
                return event && event->id == id;
            },
            token);
    }

    /// Wait for the next event accepted by `pred`. Non-matching events
    /// that carry a non-null cm_id are retained for another waiter instead
    /// of being acknowledged by the wrong helper.
    template<typename Predicate>
    [[nodiscard]] coro::task<rdma_cm_event*>
    next_event_if(Predicate pred,
                  coro::cancel_token token = {}) {
        co_return co_await next_event_matching_(std::move(pred), token);
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
    struct event_lock_guard {
        explicit event_lock_guard(sync::mutex& m) noexcept : mutex(&m) {}
        event_lock_guard(const event_lock_guard&) = delete;
        event_lock_guard& operator=(const event_lock_guard&) = delete;
        ~event_lock_guard() { unlock(); }

        void unlock() noexcept {
            if (owns) {
                mutex->unlock();
                owns = false;
            }
        }

        sync::mutex* mutex;
        bool owns = true;
    };

    template<typename Predicate>
    [[nodiscard]] coro::task<rdma_cm_event*>
    next_event_matching_(Predicate pred,
                         coro::cancel_token token) {
        while (!token.is_cancelled()) {
            std::shared_ptr<coro::cancel_source> backlog_cancel;
            detail::backlog_wait_registry::subscription backlog_subscription;
            {
                co_await event_mutex_.lock();
                event_lock_guard lock{event_mutex_};

                if (auto* pending = backlog_.take_if(pred)) {
                    lock.unlock();
                    co_return pending;
                }

                rdma_cm_event* event = nullptr;
                // Try a non-blocking read first: an event may already be
                // queued from a previous poll wake-up. The mutex serializes
                // all readers of the shared CM fd.
                const int rc = ::rdma_get_cm_event(channel_, &event);
                if (rc == 0) {
                    if (pred(event) || !event || !event->id) {
                        lock.unlock();
                        co_return event;
                    }
                    backlog_.stash(event);

                    // Let any waiter for this event's cm_id acquire the
                    // channel before we continue waiting for our own event.
                    // The fd-readable event has been consumed, so wake any
                    // waiter already suspended in async_poll_read().
                    lock.unlock();
                    backlog_waiters_.cancel_all();
                    continue;
                }
                if (errno != EAGAIN && errno != EWOULDBLOCK) {
                    lock.unlock();
                    co_return nullptr;  // hard error
                }

                backlog_cancel = std::make_shared<coro::cancel_source>();
                backlog_subscription =
                    backlog_waiters_.subscribe(backlog_cancel);
                lock.unlock();
            }

            auto token_registration = token.on_cancel([backlog_cancel]() {
                backlog_cancel->cancel();
            });
            if (token.is_cancelled()) {
                backlog_cancel->cancel();
            }

            // No matching event ready; wait without holding event_mutex_
            // so other helpers can consume already-backlogged events and
            // their cancel tokens are not blocked behind this waiter.
            auto result = co_await io::async_poll_read(
                channel_->fd, backlog_cancel->get_token());
            backlog_subscription.reset();
            if (token.is_cancelled()) {
                co_return nullptr;
            }
            if (result.was_cancelled()) {
                continue;  // local backlog wake-up; retry and re-check backlog
            }
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

    void destroy_() noexcept {
        while (auto* event = backlog_.pop_any()) {
            (void)::rdma_ack_cm_event(event);
        }
        if (channel_) {
            ::rdma_destroy_event_channel(channel_);
            channel_ = nullptr;
        }
    }

    rdma_event_channel* channel_ = nullptr;
    sync::mutex         event_mutex_{};
    detail::event_backlog backlog_{};
    detail::backlog_wait_registry backlog_waiters_{};
};

}  // namespace elio::rdma_cm
