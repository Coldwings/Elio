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
#include "mutex.hpp"

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
            // Try fast path: lockfree push
            if (ring_->try_push(std::move(value))) {
                // Signal item available
                items_available_.release();
                // Wake a waiting receiver if any
                std::lock_guard<std::mutex> guard(mutex_);
                if (!recv_waiters_.empty()) {
                    auto receiver = recv_waiters_.front();
                    recv_waiters_.pop();
                    receiver.resume();
                }
                co_return true;
            }

            // Ring is full, need to wait
            // Add to send_waiters_ queue with the value
            std::unique_lock<std::mutex> lock(mutex_);
            if (closed_) {
                co_return false;
            }

            // Create awaitable for suspension
            struct send_awaitable {
                channel& ch_;
                T value_;
                bool value_moved_ = false;

                bool await_ready() const noexcept { return false; }

                bool await_suspend(std::coroutine_handle<> h) noexcept {
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
            if (!recv_waiters_.empty()) {
                auto receiver = recv_waiters_.front();
                recv_waiters_.pop();
                receiver.resume();
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
        if (!recv_waiters_.empty()) {
            auto receiver = recv_waiters_.front();
            recv_waiters_.pop();
            receiver.resume();
        }
        co_return true;
    }

    /// Try to send without waiting
    bool try_send(T value) {
        if (closed_.load(std::memory_order_acquire)) {
            return false;
        }

        if (is_bounded()) {
            // Check if ring is full
            if (ring_->size() >= capacity_) {
                return false;
            }
            if (ring_->try_push(std::move(value))) {
                items_available_.release();
                return true;
            }
            // Ring is full, try_send fails (no blocking)
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
            // Try fast path: lockfree pop from ring
            auto val = ring_->try_pop();
            if (val.has_value()) {
                // Signal space available and wake a blocked sender
                std::lock_guard<std::mutex> guard(mutex_);
                if (!send_waiters_.empty()) {
                    auto& [awaiter, send_value] = send_waiters_.front();
                    if (ring_->try_push(std::move(send_value))) {
                        awaiter.resume();
                        send_waiters_.pop();
                    }
                }
                co_return val;
            }

            // Ring is empty, need to wait
            std::optional<T> result;

            // Create awaitable for suspension
            struct recv_awaitable {
                channel& ch_;
                std::optional<T>& result_;

                bool await_ready() const noexcept {
                    // Check if there's a value available
                    return !ch_.ring_->empty();
                }

                bool await_suspend(std::coroutine_handle<> h) noexcept {
                    std::lock_guard<std::mutex> guard(ch_.mutex_);
                    // Check again if there's a value (may have been sent while we were waiting for lock)
                    auto val = ch_.ring_->try_pop();
                    if (val.has_value()) {
                        // Value available, don't suspend
                        result_ = std::move(val);
                        // Wake a blocked sender if any
                        if (!ch_.send_waiters_.empty()) {
                            auto& [awaiter, send_value] = ch_.send_waiters_.front();
                            if (ch_.ring_->try_push(std::move(send_value))) {
                                awaiter.resume();
                                ch_.send_waiters_.pop();
                            }
                        }
                        return false;  // Don't suspend
                    }
                    // No value, add to waiters and suspend
                    ch_.recv_waiters_.push(h);
                    return true;
                }

                void await_resume() noexcept {}
            };

            recv_awaitable awaitable{*this, result};
            co_await awaitable;

            if (result.has_value()) {
                co_return result;
            }

            // Woken up, try to get value from ring
            val = ring_->try_pop();
            if (val.has_value()) {
                // Wake a blocked sender if any
                std::lock_guard<std::mutex> guard(mutex_);
                if (!send_waiters_.empty()) {
                    auto& [awaiter, send_value] = send_waiters_.front();
                    if (ring_->try_push(std::move(send_value))) {
                        awaiter.resume();
                        send_waiters_.pop();
                    }
                }
                co_return val;
            }

            // Ring is empty, check if closed and has values in queue
            if (closed_.load(std::memory_order_acquire)) {
                std::lock_guard<std::mutex> guard(mutex_);
                if (!queue_.empty()) {
                    auto result = std::move(queue_.front());
                    queue_.pop();
                    co_return result;
                }
            }

            // Still empty (channel closed)
            co_return std::nullopt;
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
                std::lock_guard<std::mutex> guard(mutex_);
                if (!send_waiters_.empty()) {
                    auto& [awaiter, send_value] = send_waiters_.front();
                    if (ring_->try_push(std::move(send_value))) {
                        awaiter.resume();
                        send_waiters_.pop();
                    }
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
                awaiter.resume();  // Wake up blocked sender
                send_waiters_.pop();
            }
        }

        // Update items_available_ count to reflect queue size
        // (for closed channels, we use queue instead of semaphore)
        while (items_available_.count() < static_cast<int>(queue_.size())) {
            items_available_.release();
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
