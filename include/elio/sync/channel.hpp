#pragma once

#include <coroutine>
#include <atomic>
#include <limits>
#include <mutex>
#include <queue>
#include <vector>
#include <optional>
#include <utility>
#include "../runtime/scheduler.hpp"

namespace elio::sync {

/// Multi-producer multi-consumer channel
template<typename T>
class channel {
public:
    /// Create a channel with the given capacity.
    /// @param capacity Maximum number of items that can be buffered.
    ///   - capacity == 0: rendezvous (synchronous) channel — send blocks until
    ///     a receiver is ready (Go-style `make(chan T)`)
    ///   - capacity > 0: bounded channel with back-pressure when full
    /// @note For an unbounded channel (no back-pressure), use
    ///   `channel<T>::unbounded()`.
    explicit channel(size_t capacity = 0)
        : capacity_(capacity), closed_(false) {}

    /// Create an unbounded channel (no back-pressure, may grow indefinitely).
    static channel unbounded() {
        return channel(std::numeric_limits<size_t>::max());
    }

    ~channel() {
        close();
    }

    // Non-copyable
    channel(const channel&) = delete;
    channel& operator=(const channel&) = delete;

    // Non-movable (concurrent send/recv would race with move)
    channel(channel&&) = delete;
    channel& operator=(channel&&) = delete;

    /// Send awaitable
    class send_awaitable {
    public:
        send_awaitable(channel& ch, T value)
            : channel_(ch), value_(std::move(value)) {}

        // Note: non-const so we can commit the operation under the channel
        // lock and avoid a TOCTOU window between await_ready and await_resume.
        bool await_ready() noexcept {
            std::lock_guard<std::mutex> guard(channel_.mutex_);

            if (channel_.closed_) {
                // Don't suspend; await_resume reports failure.
                return true;
            }

            if (channel_.capacity_ == 0) {
                // Rendezvous: hand off directly to a waiting receiver.
                if (!channel_.recv_waiters_.empty()) {
                    // A receiver is already waiting — push value into the queue
                    // so try_take_locked / await_resume can pick it up, then
                    // wake the receiver.
                    channel_.queue_.push(std::move(value_));
                    committed_ = true;

                    to_wake_ = channel_.recv_waiters_.front();
                    channel_.recv_waiters_.pop();
                    return true;
                }
                // No receiver ready — must suspend.
                return false;
            }

            // Bounded channel (capacity_ > 0)
            if (channel_.queue_.size() < channel_.capacity_) {
                // Commit the send while still holding the lock.  Doing this
                // here (rather than in await_resume) prevents another sender
                // from filling the queue between unlock and resume, which
                // previously could push past capacity.
                channel_.queue_.push(std::move(value_));
                committed_ = true;

                if (!channel_.recv_waiters_.empty()) {
                    to_wake_ = channel_.recv_waiters_.front();
                    channel_.recv_waiters_.pop();
                }
                return true;
            }

            return false;
        }

        bool await_suspend(std::coroutine_handle<> awaiter) noexcept {
            std::lock_guard<std::mutex> guard(channel_.mutex_);

            if (channel_.closed_) {
                return false;  // Don't suspend; await_resume returns false
            }

            if (channel_.capacity_ == 0) {
                // Rendezvous: check again for a receiver that arrived between
                // await_ready and now.
                if (!channel_.recv_waiters_.empty()) {
                    channel_.queue_.push(std::move(value_));
                    committed_ = true;

                    to_wake_ = channel_.recv_waiters_.front();
                    channel_.recv_waiters_.pop();
                    return false;  // Don't suspend; value delivered
                }
                // No receiver — park on send_waiters_.
                channel_.send_waiters_.push({awaiter, std::move(value_)});
                value_moved_ = true;
                return true;
            }

            // Bounded channel (capacity_ > 0)
            if (channel_.queue_.size() < channel_.capacity_) {
                // Space appeared between await_ready and now — commit.
                channel_.queue_.push(std::move(value_));
                committed_ = true;

                if (!channel_.recv_waiters_.empty()) {
                    to_wake_ = channel_.recv_waiters_.front();
                    channel_.recv_waiters_.pop();
                }
                return false;
            }

            // Wait for space
            channel_.send_waiters_.push({awaiter, std::move(value_)});
            value_moved_ = true;
            return true;
        }

        bool await_resume() {
            if (to_wake_) {
                runtime::schedule_handle(to_wake_);
                to_wake_ = nullptr;
            }

            if (committed_) {
                // Value was committed under the lock; the send succeeded.
                return true;
            }

            // We were either suspended on send_waiters_ (woken by a recv that
            // took the value, or by close()), or short-circuited because the
            // channel was closed at await_ready / await_suspend time.
            std::lock_guard<std::mutex> guard(channel_.mutex_);
            if (channel_.closed_) {
                return false;
            }
            return value_moved_;
        }

    private:
        channel& channel_;
        T value_;
        bool committed_ = false;       // value pushed to queue under the lock
        bool value_moved_ = false;     // value moved into send_waiters_
        std::coroutine_handle<> to_wake_;
    };

    /// Receive awaitable
    class recv_awaitable {
    public:
        explicit recv_awaitable(channel& ch) : channel_(ch) {}

        // Note: non-const so we can commit the recv (capture the value) under
        // the channel lock and avoid a TOCTOU window where another receiver
        // drains the queue between await_ready and await_resume — which would
        // otherwise produce a spurious nullopt that callers misinterpret as
        // "channel closed".
        bool await_ready() noexcept {
            std::lock_guard<std::mutex> guard(channel_.mutex_);
            return try_take_locked();
        }

        bool await_suspend(std::coroutine_handle<> awaiter) noexcept {
            std::lock_guard<std::mutex> guard(channel_.mutex_);

            if (try_take_locked()) {
                return false;  // Don't suspend; value captured
            }

            channel_.recv_waiters_.push(awaiter);
            return true;
        }

        std::optional<T> await_resume() {
            if (to_wake_) {
                runtime::schedule_handle(to_wake_);
                to_wake_ = nullptr;
            }

            if (taken_.has_value()) {
                return std::move(taken_);
            }

            // We suspended on recv_waiters_ and were woken either by a sender
            // delivering a value (in which case the queue now has it) or by
            // close().  Re-check under the lock.
            std::optional<T> result;
            {
                std::lock_guard<std::mutex> guard(channel_.mutex_);
                if (!channel_.queue_.empty()) {
                    result = std::move(channel_.queue_.front());
                    channel_.queue_.pop();
                }
            }
            return result;
        }

    private:
        // Returns true if the recv is committed (taken_ filled or channel
        // closed and empty so we can return nullopt without suspending).
        // Caller must hold channel_.mutex_.
        bool try_take_locked() noexcept {
            if (!channel_.queue_.empty()) {
                taken_ = std::move(channel_.queue_.front());
                channel_.queue_.pop();

                // Queue had space freed — pull a blocked sender's value in.
                if (!channel_.send_waiters_.empty()) {
                    auto& [waiter, value] = channel_.send_waiters_.front();
                    channel_.queue_.push(std::move(value));
                    to_wake_ = waiter;
                    channel_.send_waiters_.pop();
                }
                return true;
            }

            // Queue empty: check for a sender waiting on a rendezvous channel
            // (capacity_==0) or a sender whose value hasn't been pulled into
            // the queue yet.
            if (!channel_.send_waiters_.empty()) {
                auto& [waiter, value] = channel_.send_waiters_.front();
                taken_ = std::move(value);
                to_wake_ = waiter;
                channel_.send_waiters_.pop();
                return true;
            }

            if (channel_.closed_) {
                // Empty + closed: don't suspend; await_resume returns nullopt.
                return true;
            }

            return false;
        }

        channel& channel_;
        std::optional<T> taken_;
        std::coroutine_handle<> to_wake_;
    };

    /// Send a value to the channel
    auto send(T value) {
        return send_awaitable(*this, std::move(value));
    }

    /// Try to send without waiting
    bool try_send(T value) {
        std::coroutine_handle<> to_wake;

        {
            std::lock_guard<std::mutex> guard(mutex_);

            if (closed_) {
                return false;
            }

            if (capacity_ == 0) {
                // Rendezvous: only succeeds if a receiver is already waiting.
                if (recv_waiters_.empty()) {
                    return false;
                }
                queue_.push(std::move(value));
                to_wake = recv_waiters_.front();
                recv_waiters_.pop();
            } else {
                // Bounded channel
                if (queue_.size() >= capacity_) {
                    return false;
                }
                queue_.push(std::move(value));
                if (!recv_waiters_.empty()) {
                    to_wake = recv_waiters_.front();
                    recv_waiters_.pop();
                }
            }
        }

        // Re-schedule outside the lock
        if (to_wake) {
            runtime::schedule_handle(to_wake);
        }

        return true;
    }

    /// Receive a value from the channel
    auto recv() {
        return recv_awaitable(*this);
    }

    /// Try to receive without waiting
    std::optional<T> try_recv() {
        std::coroutine_handle<> to_wake;
        std::optional<T> result;

        {
            std::lock_guard<std::mutex> guard(mutex_);

            if (!queue_.empty()) {
                result = std::move(queue_.front());
                queue_.pop();

                // Queue had space freed — pull a blocked sender's value in.
                if (!send_waiters_.empty()) {
                    auto& [waiter, send_value] = send_waiters_.front();
                    queue_.push(std::move(send_value));
                    to_wake = waiter;
                    send_waiters_.pop();
                }
            } else if (!send_waiters_.empty()) {
                // Rendezvous path: no queued items but a sender is waiting.
                auto& [waiter, send_value] = send_waiters_.front();
                result = std::move(send_value);
                to_wake = waiter;
                send_waiters_.pop();
            } else {
                return std::nullopt;
            }
        }

        // Re-schedule outside the lock
        if (to_wake) {
            runtime::schedule_handle(to_wake);
        }

        return result;
    }

    /// Close the channel
    void close() {
        std::vector<std::coroutine_handle<>> to_resume;

        {
            std::lock_guard<std::mutex> guard(mutex_);

            if (closed_) {
                return;
            }

            closed_ = true;

            // Drain pending send values into the queue so receivers can still
            // read them after close (Go semantics: close flushes in-flight
            // sends).  Senders' await_resume will return false because
            // value_moved_ is set but committed_ is not — they learn the send
            // did not complete normally.
            while (!send_waiters_.empty()) {
                auto& [waiter, value] = send_waiters_.front();
                queue_.push(std::move(value));
                to_resume.push_back(waiter);
                send_waiters_.pop();
            }

            // Wake all recv waiters
            while (!recv_waiters_.empty()) {
                to_resume.push_back(recv_waiters_.front());
                recv_waiters_.pop();
            }
        }

        // Re-schedule all waiters through the scheduler
        for (auto& h : to_resume) {
            runtime::schedule_handle(h);
        }
    }

    /// Check if channel is closed
    bool is_closed() const noexcept {
        std::lock_guard<std::mutex> guard(mutex_);
        return closed_;
    }

    /// Get current queue size
    size_t size() const noexcept {
        std::lock_guard<std::mutex> guard(mutex_);
        return queue_.size();
    }

    /// Check if channel is empty
    bool empty() const noexcept {
        std::lock_guard<std::mutex> guard(mutex_);
        return queue_.empty();
    }

private:
    mutable std::mutex mutex_;
    std::queue<T> queue_;
    std::queue<std::coroutine_handle<>> recv_waiters_;
    std::queue<std::pair<std::coroutine_handle<>, T>> send_waiters_;
    size_t capacity_;
    bool closed_;
};

} // namespace elio::sync
