#pragma once

#include <coroutine>
#include <atomic>
#include <limits>
#include <mutex>
#include <queue>
#include <vector>
#include <optional>
#include <utility>
#include <memory>
#include "lockfree_ring.hpp"
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
        : capacity_(capacity)
        , closed_(false)
    {
        if (is_bounded()) {
            ring_ = std::make_unique<LockfreeMPMCRing<T>>(capacity);
        }
    }

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

    /// Send a value to the channel
    auto send(T value) -> coro::task<bool> {
        if (closed_.load(std::memory_order_acquire)) {
            co_return false;
        }

        if (is_bounded()) {
            // Try fast path: lockfree push (only if under user-requested capacity)
            // Note: we must check closed_ under lock to avoid racing with close()
            if (ring_->size() < capacity_) {
                // Check closed_ and push under lock to prevent post-close sends
                std::coroutine_handle<> receiver;
                bool pushed = false;
                {
                    std::lock_guard<std::mutex> guard(mutex_);
                    if (!closed_) {
                        if (ring_->try_push(value)) {
                            pushed = true;
                            // Wake a waiting receiver if any
                            if (!recv_waiters_.empty()) {
                                receiver = recv_waiters_.front();
                                recv_waiters_.pop();
                            }
                        }
                    }
                }
                if (pushed) {
                    if (receiver) {
                        elio::runtime::schedule_handle(receiver);
                    }
                    co_return true;
                }
                if (closed_.load(std::memory_order_acquire)) {
                    co_return false;
                }
                // Push failed due to contention, fall through to slow path
            }

            // Ring is full, need to wait
            // Create awaitable for suspension (lock NOT held — critical for
            // coroutine correctness: we must not hold a mutex across co_await)
            struct send_awaitable {
                channel& ch_;
                T value_;
                bool pushed_ = false;

                bool await_ready() const noexcept { return false; }

                bool await_suspend(std::coroutine_handle<> h) noexcept {
                    std::lock_guard<std::mutex> guard(ch_.mutex_);

                    // Double-check: closed, ring space, or waiting receivers
                    if (ch_.closed_) {
                        return false;
                    }

                    // Check if ring has space now (consumer may have popped)
                    if (ch_.ring_->size() < ch_.capacity_) {
                        if (ch_.ring_->try_push(value_)) {
                            pushed_ = true;
                            // Wake a waiting receiver if any
                            std::coroutine_handle<> receiver;
                            if (!ch_.recv_waiters_.empty()) {
                                receiver = ch_.recv_waiters_.front();
                                ch_.recv_waiters_.pop();
                            }
                            if (receiver) {
                                elio::runtime::schedule_handle(receiver);
                            }
                            return false;  // Don't suspend, we pushed successfully
                        }
                    }

                    // Still full, add to send_waiters_
                    ch_.send_waiters_.push({h, std::move(value_)});
                    return true;
                }

                bool await_resume() const noexcept { return pushed_; }
            };

            send_awaitable awaitable{*this, std::move(value)};
            bool pushed = co_await awaitable;

            if (pushed) {
                co_return true;
            }
            co_return !closed_.load(std::memory_order_acquire);
        }

        if (is_unbounded()) {
            std::lock_guard<std::mutex> guard(mutex_);
            if (closed_) {
                co_return false;
            }
            queue_.push(std::move(value));
            // Wake a waiting receiver if any
            std::coroutine_handle<> receiver;
            if (!recv_waiters_.empty()) {
                receiver = recv_waiters_.front();
                recv_waiters_.pop();
            }
            if (receiver) {
                elio::runtime::schedule_handle(receiver);
            }
            co_return true;
        }

        // Rendezvous: must suspend if no receiver is waiting
        {
            struct rendezvous_send_awaitable {
                channel& ch_;
                T value_;
                bool handed_off_ = false;

                bool await_ready() const noexcept { return false; }

                bool await_suspend(std::coroutine_handle<> h) noexcept {
                    std::lock_guard<std::mutex> guard(ch_.mutex_);

                    if (ch_.closed_) {
                        return false;
                    }

                    // If a receiver is waiting, hand off directly
                    if (!ch_.recv_waiters_.empty()) {
                        // Push to queue_ for the receiver to pick up
                        ch_.queue_.push(std::move(value_));
                        handed_off_ = true;
                        auto receiver = ch_.recv_waiters_.front();
                        ch_.recv_waiters_.pop();
                        elio::runtime::schedule_handle(receiver);
                        return false;  // Don't suspend
                    }

                    // No receiver waiting — suspend on send_waiters_
                    ch_.send_waiters_.push({h, std::move(value_)});
                    return true;
                }

                bool await_resume() const noexcept { return handed_off_; }
            };

            rendezvous_send_awaitable awaitable{*this, std::move(value)};
            bool result = co_await awaitable;
            if (result) {
                co_return true;
            }
            co_return !closed_.load(std::memory_order_acquire);
        }
    }

    /// Try to send without waiting
    bool try_send(T value) {
        if (closed_.load(std::memory_order_acquire)) {
            return false;
        }

        if (is_bounded()) {
            std::lock_guard<std::mutex> guard(mutex_);
            if (closed_) {
                return false;
            }
            if (ring_->size() >= capacity_) {
                return false;
            }
            if (ring_->try_push(value)) {
                // Wake a waiting receiver if any
                std::coroutine_handle<> receiver;
                if (!recv_waiters_.empty()) {
                    receiver = recv_waiters_.front();
                    recv_waiters_.pop();
                }
                if (receiver) {
                    elio::runtime::schedule_handle(receiver);
                }
                return true;
            }
            return false;
        }

        if (is_rendezvous()) {
            // Rendezvous: only succeeds if a receiver is already waiting
            std::lock_guard<std::mutex> guard(mutex_);
            if (closed_) {
                return false;
            }
            if (!recv_waiters_.empty()) {
                queue_.push(std::move(value));
                std::coroutine_handle<> receiver = recv_waiters_.front();
                recv_waiters_.pop();
                elio::runtime::schedule_handle(receiver);
                return true;
            }
            return false;
        }

        // Unbounded
        std::lock_guard<std::mutex> guard(mutex_);
        if (closed_) {
            return false;
        }
        queue_.push(std::move(value));
        // Wake a waiting receiver if any
        std::coroutine_handle<> receiver;
        if (!recv_waiters_.empty()) {
            receiver = recv_waiters_.front();
            recv_waiters_.pop();
        }
        if (receiver) {
            elio::runtime::schedule_handle(receiver);
        }
        return true;
    }

    /// Receive a value from the channel
    coro::task<std::optional<T>> recv() {
        if (is_bounded()) {
            while (true) {
                // Try fast path: lockfree pop from ring
                auto val = ring_->try_pop();
                if (val.has_value()) {
                    // Wake a blocked sender if any (transfer their value into ring)
                    std::coroutine_handle<> sender;
                    {
                        std::lock_guard<std::mutex> guard(mutex_);
                        if (!send_waiters_.empty()) {
                            auto& [awaiter, send_value] = send_waiters_.front();
                            // Only transfer if we can guarantee the push will succeed.
                            // We just popped, so there should be space. But another
                            // producer could fill it concurrently. If try_push fails,
                            // leave the sender in the queue to be woken later when
                            // there's actually space for its value.
                            if (ring_->try_push(send_value)) {
                                sender = awaiter;
                                send_waiters_.pop();
                            }
                        }
                    }
                    if (sender) {
                        elio::runtime::schedule_handle(sender);
                    }
                    co_return val;
                }

                // Ring is empty — check send_waiters_ and closed state under lock
                {
                    std::lock_guard<std::mutex> guard(mutex_);
                    if (!send_waiters_.empty()) {
                        auto& [awaiter, send_value] = send_waiters_.front();
                        auto result = std::optional<T>(std::move(send_value));
                        auto sender = awaiter;
                        send_waiters_.pop();
                        elio::runtime::schedule_handle(sender);
                        co_return result;
                    }
                    if (closed_.load(std::memory_order_acquire)) {
                        // close() drains ring + send_waiters_ into queue_;
                        // drain remaining values before returning nullopt.
                        if (!queue_.empty()) {
                            auto result = std::move(queue_.front());
                            queue_.pop();
                            co_return result;
                        }
                        co_return std::nullopt;
                    }
                }

                // No value available — suspend until woken by sender or close
                struct recv_suspend_awaitable {
                    channel& ch_;
                    bool await_ready() const noexcept { return false; }
                    bool await_suspend(std::coroutine_handle<> h) noexcept {
                        std::lock_guard<std::mutex> guard(ch_.mutex_);
                        // Re-check: a send or close may have happened since our check
                        if (!ch_.ring_->empty() || !ch_.send_waiters_.empty() ||
                            ch_.closed_.load(std::memory_order_acquire)) {
                            return false;  // Don't suspend, retry immediately
                        }
                        ch_.recv_waiters_.push(h);
                        return true;
                    }
                    void await_resume() noexcept {}
                };

                co_await recv_suspend_awaitable{*this};
                // Woken up — loop back and retry
            }
        }

        // Unbounded or rendezvous: use queue with explicit waiters
        while (true) {
            {
                std::lock_guard<std::mutex> guard(mutex_);
                // For rendezvous channels, also check send_waiters_ for direct handoff
                if (!queue_.empty()) {
                    auto result = std::move(queue_.front());
                    queue_.pop();
                    co_return result;
                }
                if (is_rendezvous() && !send_waiters_.empty()) {
                    auto& [awaiter, send_value] = send_waiters_.front();
                    auto result = std::optional<T>(std::move(send_value));
                    auto sender = awaiter;
                    send_waiters_.pop();
                    elio::runtime::schedule_handle(sender);
                    co_return result;
                }
                if (closed_.load(std::memory_order_acquire)) {
                    co_return std::nullopt;
                }
            }

            // Wait for item or close
            struct recv_suspend_awaitable {
                channel& ch_;
                bool await_ready() const noexcept { return false; }
                bool await_suspend(std::coroutine_handle<> h) noexcept {
                    std::lock_guard<std::mutex> guard(ch_.mutex_);
                    // Double-check: item available, sender waiting (rendezvous), or closed?
                    if (!ch_.queue_.empty() || ch_.closed_.load(std::memory_order_acquire)) {
                        return false;  // Don't suspend, retry immediately
                    }
                    if (ch_.is_rendezvous() && !ch_.send_waiters_.empty()) {
                        return false;  // Sender waiting for rendezvous
                    }
                    ch_.recv_waiters_.push(h);
                    return true;
                }
                void await_resume() noexcept {}
            };

            co_await recv_suspend_awaitable{*this};
        }
    }

    /// Try to receive without waiting
    std::optional<T> try_recv() {
        if (is_bounded()) {
            // Try lockfree pop from ring first
            auto val = ring_->try_pop();
            if (val.has_value()) {
                // Wake a blocked sender if any
                std::coroutine_handle<> sender;
                {
                    std::lock_guard<std::mutex> guard(mutex_);
                    if (!send_waiters_.empty()) {
                        auto& [awaiter, send_value] = send_waiters_.front();
                        if (ring_->try_push(send_value)) {
                            sender = awaiter;
                            send_waiters_.pop();
                        } else {
                            // Can't transfer — wake sender to retry
                            sender = awaiter;
                            send_waiters_.pop();
                        }
                    }
                }
                if (sender) {
                    elio::runtime::schedule_handle(sender);
                }
                return val;
            }

            // Ring is empty, check if closed and has values in queue
            if (closed_.load(std::memory_order_acquire)) {
                std::lock_guard<std::mutex> guard(mutex_);
                if (!queue_.empty()) {
                    auto result = std::move(queue_.front());
                    queue_.pop();
                    return result;
                }
            }

            // Ring is empty
            return std::nullopt;
        }

        std::lock_guard<std::mutex> guard(mutex_);
        // For rendezvous, check send_waiters_ too
        if (queue_.empty()) {
            if (is_rendezvous() && !send_waiters_.empty()) {
                auto& [awaiter, send_value] = send_waiters_.front();
                auto result = std::optional<T>(std::move(send_value));
                auto sender = awaiter;
                send_waiters_.pop();
                elio::runtime::schedule_handle(sender);
                return result;
            }
            return std::nullopt;
        }
        auto result = std::move(queue_.front());
        queue_.pop();
        return result;
    }

    /// Close the channel
    void close() {
        bool expected = false;
        if (!closed_.compare_exchange_strong(expected, true)) {
            return;
        }

        std::vector<std::coroutine_handle<>> senders_to_wake;
        std::vector<std::coroutine_handle<>> receivers_to_wake;

        {
            std::lock_guard<std::mutex> guard(mutex_);

            // For bounded channels, drain any remaining values from the ring
            // and from send_waiters_ into the queue (Go semantics)
            if (is_bounded()) {
                // Drain ring
                while (true) {
                    auto val = ring_->try_pop();
                    if (!val.has_value()) break;
                    queue_.push(std::move(*val));
                }
                // Drain send_waiters_
                while (!send_waiters_.empty()) {
                    auto& [awaiter, value] = send_waiters_.front();
                    queue_.push(std::move(value));
                    senders_to_wake.push_back(awaiter);
                    send_waiters_.pop();
                }
            }

            // For rendezvous channels, drain send_waiters_ into queue_
            if (is_rendezvous()) {
                while (!send_waiters_.empty()) {
                    auto& [awaiter, value] = send_waiters_.front();
                    queue_.push(std::move(value));
                    senders_to_wake.push_back(awaiter);
                    send_waiters_.pop();
                }
            }

            // Collect all waiting receivers
            while (!recv_waiters_.empty()) {
                receivers_to_wake.push_back(recv_waiters_.front());
                recv_waiters_.pop();
            }
        }

        // Wake up all blocked senders and receivers after releasing lock
        for (auto& sender : senders_to_wake) {
            elio::runtime::schedule_handle(sender);
        }
        for (auto& receiver : receivers_to_wake) {
            elio::runtime::schedule_handle(receiver);
        }
    }

    /// Check if channel is closed
    bool is_closed() const noexcept {
        return closed_.load(std::memory_order_acquire);
    }

    /// Get current queue size
    size_t size() const noexcept {
        if (is_bounded()) {
            return ring_->size();
        }
        std::lock_guard<std::mutex> guard(mutex_);
        return queue_.size();
    }

    /// Check if channel is empty
    bool empty() const noexcept {
        if (is_bounded()) {
            return ring_->empty();
        }
        std::lock_guard<std::mutex> guard(mutex_);
        return queue_.empty();
    }

private:
    bool is_bounded() const noexcept {
        return capacity_ > 0 && capacity_ != std::numeric_limits<size_t>::max();
    }

    bool is_unbounded() const noexcept {
        return capacity_ == std::numeric_limits<size_t>::max();
    }

    bool is_rendezvous() const noexcept {
        return capacity_ == 0;
    }

    mutable std::mutex mutex_;
    std::unique_ptr<LockfreeMPMCRing<T>> ring_;  // Only for bounded channels
    std::queue<T> queue_;  // Only for unbounded and rendezvous channels
    std::queue<std::coroutine_handle<>> recv_waiters_;  // Blocked receivers
    std::queue<std::pair<std::coroutine_handle<>, T>> send_waiters_;  // Blocked senders with their values
    size_t capacity_;
    std::atomic<bool> closed_;
};

} // namespace elio::sync
