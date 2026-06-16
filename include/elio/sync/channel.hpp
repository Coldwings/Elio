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
#include "semaphore.hpp"

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
        , items_available_(0)
        , space_available_(capacity == 0 ? 0 : (capacity == std::numeric_limits<size_t>::max() ? 0 : capacity))
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
            if (ring_->size() < capacity_ && ring_->try_push(std::move(value))) {
                // Signal item available
                items_available_.release();
                // Wake a waiting receiver if any
                std::coroutine_handle<> receiver;
                {
                    std::lock_guard<std::mutex> guard(mutex_);
                    if (!recv_waiters_.empty()) {
                        receiver = recv_waiters_.front();
                        recv_waiters_.pop();
                    }
                }
                if (receiver) {
                    // Resume receiver through scheduler
                    elio::runtime::schedule_handle(receiver);
                }
                co_return true;
            }

            // Ring is full, need to wait
            // Add to send_waiters_ queue with the value
            {
                std::lock_guard<std::mutex> guard(mutex_);
                if (closed_) {
                    co_return false;
                }
            }

            // Create awaitable for suspension (lock NOT held — critical for
            // coroutine correctness: we must not hold a mutex across co_await)
            struct send_awaitable {
                channel& ch_;
                T value_;
                bool value_moved_ = false;

                bool await_ready() const noexcept { return false; }

                bool await_suspend(std::coroutine_handle<> h) noexcept {
                    std::lock_guard<std::mutex> guard(ch_.mutex_);
                    if (ch_.closed_) {
                        return false;  // Don't suspend, resume and return false
                    }
                    ch_.send_waiters_.push({h, std::move(value_)});
                    value_moved_ = true;
                    return true;
                }

                void await_resume() noexcept {}
            };

            send_awaitable awaitable{*this, std::move(value)};
            co_await awaitable;

            // Value was moved to send_waiters_, check if it was successfully sent
            co_return !closed_.load(std::memory_order_acquire);
        }

        if (is_unbounded()) {
            std::lock_guard<std::mutex> guard(mutex_);
            if (closed_) {
                co_return false;
            }
            queue_.push(std::move(value));
            items_available_.release();
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

        // Rendezvous
        std::lock_guard<std::mutex> guard(mutex_);
        if (closed_) {
            co_return false;
        }
        queue_.push(std::move(value));
        items_available_.release();
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

    /// Try to send without waiting
    bool try_send(T value) {
        if (closed_.load(std::memory_order_acquire)) {
            return false;
        }

        if (is_bounded()) {
            if (ring_->size() >= capacity_) {
                return false;
            }
            if (ring_->try_push(std::move(value))) {
                items_available_.release();
                return true;
            }
            return false;
        }

        if (is_rendezvous()) {
            // Rendezvous: only succeeds if a receiver is already waiting
            std::lock_guard<std::mutex> guard(mutex_);
            if (!recv_waiters_.empty()) {
                queue_.push(std::move(value));
                items_available_.release();
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
        items_available_.release();
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
                            if (ring_->try_push(std::move(send_value))) {
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

        // Unbounded or rendezvous: use queue
        while (true) {
            {
                std::lock_guard<std::mutex> guard(mutex_);
                if (!queue_.empty()) {
                    auto result = std::move(queue_.front());
                    queue_.pop();
                    co_return result;
                }
                if (closed_.load(std::memory_order_acquire)) {
                    co_return std::nullopt;
                }
            }

            // Wait for item
            co_await items_available_.acquire();
        }
    }

    /// Try to receive without waiting
    std::optional<T> try_recv() {
        if (is_bounded()) {
            // Try lockfree pop from ring first
            auto val = ring_->try_pop();
            if (val.has_value()) {
                // Signal space available and wake a blocked sender
                std::coroutine_handle<> sender;
                {
                    std::lock_guard<std::mutex> guard(mutex_);
                    if (!send_waiters_.empty()) {
                        auto& [awaiter, send_value] = send_waiters_.front();
                        if (ring_->try_push(std::move(send_value))) {
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
        if (queue_.empty()) {
            return std::nullopt;
        }
        auto result = std::move(queue_.front());
        queue_.pop();
        items_available_.try_acquire();  // Decrement count (should succeed)
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
        size_t queue_size = 0;

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

            queue_size = queue_.size();

            // Collect all waiting receivers
            while (!recv_waiters_.empty()) {
                receivers_to_wake.push_back(recv_waiters_.front());
                recv_waiters_.pop();
            }
        }

        // Update items_available_ count OUTSIDE the channel lock.
        // release() calls schedule_handle which may resume coroutines via
        // trampoline; those coroutines can call recv() which needs mutex_.
        while (items_available_.count() < static_cast<int>(queue_size)) {
            items_available_.release();
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

    /// Get items_available semaphore count (for debugging)
    int items_available_count() const noexcept {
        return items_available_.count();
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
    semaphore items_available_;  // Tracks available items
    semaphore space_available_;  // Tracks available space (bounded only)
};

} // namespace elio::sync
