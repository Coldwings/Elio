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
#include <cassert>
#include "lockfree_ring.hpp"
#include "../detail/intrusive_list.hpp"
#include "../runtime/scheduler.hpp"

namespace elio::sync {

/// Multi-producer multi-consumer channel
template<typename T>
class channel {
public:
    // Forward declarations for intrusive_list
    class send_awaitable;
    class recv_awaitable;

    /// Create a channel with the given capacity.
    ///
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

    /// Create an unbounded channel
    static channel unbounded() {
        return channel(std::numeric_limits<size_t>::max());
    }

    ~channel() {
        close();
        assert(send_waiters_.empty() && recv_waiters_.empty()
               && "channel destroyed with pending waiters");
    }

    // Non-copyable, non-movable
    channel(const channel&) = delete;
    channel& operator=(const channel&) = delete;
    channel(channel&&) = delete;
    channel& operator=(channel&&) = delete;

    /// Send awaitable — handles both bounded and rendezvous channels.
    /// Stores handle + value. Inherits intrusive_list_node for safe unlinking.
    class send_awaitable : public elio::detail::intrusive_list_node<send_awaitable> {
    public:
        send_awaitable(channel& ch, T value) : ch_(ch), value_(std::move(value)) {}

        ~send_awaitable() {
            // Fast path: if we never suspended, we were never enqueued
            if (!suspended_) return;

            // Slow path: acquire mutex to prevent race with close/recv
            std::lock_guard<std::mutex> guard(ch_.mutex_);
            if (this->is_linked()) {
                ch_.send_waiters_.remove(this);
            }
        }

        bool await_ready() const noexcept { return false; }

        bool await_suspend(std::coroutine_handle<> h) noexcept {
            std::coroutine_handle<> to_schedule = nullptr;
            bool should_suspend = true;

            {
                std::lock_guard<std::mutex> guard(ch_.mutex_);

                if (ch_.closed_) {
                    should_suspend = false;
                } else if (ch_.is_rendezvous()) {
                    // Rendezvous: direct handoff to a waiting receiver
                    if (!ch_.recv_waiters_.empty()) {
                        ch_.queue_.push(std::move(value_));
                        success_ = true;
                        auto* receiver = ch_.recv_waiters_.pop_front();
                        to_schedule = receiver->handle_;
                        should_suspend = false;
                    }
                } else {
                    // Bounded: try to push into ring
                    if (ch_.ring_->size() < ch_.capacity_) {
                        if (ch_.ring_->try_push(value_)) {
                            success_ = true;
                            if (!ch_.recv_waiters_.empty()) {
                                auto* receiver = ch_.recv_waiters_.pop_front();
                                to_schedule = receiver->handle_;
                            }
                            should_suspend = false;
                        }
                    }
                }

                if (should_suspend) {
                    // Cannot send now — suspend
                    handle_ = h;
                    ch_.send_waiters_.push_back(this);
                    suspended_ = true;  // Mark as enqueued
                }
            }

            // Schedule outside lock to avoid deadlock
            if (to_schedule) {
                elio::runtime::schedule_handle(to_schedule);
            }

            return should_suspend;
        }

        bool await_resume() const noexcept { return success_; }

    private:
        channel& ch_;
        T value_;
        std::coroutine_handle<> handle_;
        bool success_ = false;
        bool suspended_ = false;  // True if enqueued in send_waiters_

        friend class channel;
    };

    /// Recv awaitable — handles bounded, unbounded, and rendezvous channels.
    class recv_awaitable : public elio::detail::intrusive_list_node<recv_awaitable> {
    public:
        explicit recv_awaitable(channel& ch) : ch_(ch) {}

        ~recv_awaitable() {
            // Fast path: if we never suspended, we were never enqueued
            if (!suspended_) return;

            // Slow path: acquire mutex to prevent race with close/send
            std::lock_guard<std::mutex> guard(ch_.mutex_);
            if (this->is_linked()) {
                ch_.recv_waiters_.remove(this);
            }
        }

        bool await_ready() const noexcept { return false; }

        bool await_suspend(std::coroutine_handle<> h) noexcept {
            std::lock_guard<std::mutex> guard(ch_.mutex_);

            if (ch_.is_bounded()) {
                if (!ch_.ring_->empty() || !ch_.send_waiters_.empty() ||
                    ch_.closed_.load(std::memory_order_acquire)) {
                    return false;
                }
            } else {
                if (!ch_.queue_.empty() || ch_.closed_.load(std::memory_order_acquire)) {
                    return false;
                }
                if (ch_.is_rendezvous() && !ch_.send_waiters_.empty()) {
                    return false;
                }
            }

            handle_ = h;
            ch_.recv_waiters_.push_back(this);
            suspended_ = true;  // Mark as enqueued
            return true;
        }

        void await_resume() noexcept {}

    private:
        channel& ch_;
        std::coroutine_handle<> handle_;
        bool suspended_ = false;  // True if enqueued in recv_waiters_

        friend class channel;
    };

    /// Send a value to the channel
    auto send(T value) -> coro::task<bool> {
        if (closed_.load(std::memory_order_acquire)) {
            co_return false;
        }

        if (is_unbounded()) {
            std::coroutine_handle<> receiver_handle = nullptr;
            {
                std::lock_guard<std::mutex> guard(mutex_);
                if (closed_) {
                    co_return false;
                }
                queue_.push(std::move(value));
                if (!recv_waiters_.empty()) {
                    auto* receiver = recv_waiters_.pop_front();
                    receiver_handle = receiver->handle_;
                }
            }
            if (receiver_handle) {
                elio::runtime::schedule_handle(receiver_handle);
            }
            co_return true;
        }

        // Bounded or rendezvous: try fast path, then suspend
        if (is_bounded() && ring_->size() < capacity_) {
            bool pushed = false;
            std::coroutine_handle<> receiver_handle = nullptr;
            {
                std::lock_guard<std::mutex> guard(mutex_);
                if (!closed_ && ring_->size() < capacity_) {
                    if (ring_->try_push(value)) {
                        pushed = true;
                        if (!recv_waiters_.empty()) {
                            auto* receiver = recv_waiters_.pop_front();
                            receiver_handle = receiver->handle_;
                        }
                    }
                }
            }
            if (receiver_handle) {
                elio::runtime::schedule_handle(receiver_handle);
            }
            if (pushed) {
                co_return true;
            }
            if (closed_.load(std::memory_order_acquire)) {
                co_return false;
            }
        }

        // Suspend via send_awaitable
        send_awaitable awaitable{*this, std::move(value)};
        bool pushed = co_await awaitable;

        if (pushed) {
            co_return true;
        }
        co_return !closed_.load(std::memory_order_acquire);
    }

    /// Try to send without waiting
    bool try_send(T value) {
        if (closed_.load(std::memory_order_acquire)) {
            return false;
        }

        if (is_bounded()) {
            std::coroutine_handle<> receiver_handle = nullptr;
            {
                std::lock_guard<std::mutex> guard(mutex_);
                if (closed_) return false;
                if (ring_->size() >= capacity_) return false;
                if (ring_->try_push(value)) {
                    if (!recv_waiters_.empty()) {
                        auto* receiver = recv_waiters_.pop_front();
                        receiver_handle = receiver->handle_;
                    }
                } else {
                    return false;
                }
            }
            if (receiver_handle) {
                elio::runtime::schedule_handle(receiver_handle);
            }
            return true;
        }

        if (is_rendezvous()) {
            std::coroutine_handle<> receiver_handle = nullptr;
            {
                std::lock_guard<std::mutex> guard(mutex_);
                if (closed_) return false;
                if (!recv_waiters_.empty()) {
                    queue_.push(std::move(value));
                    auto* receiver = recv_waiters_.pop_front();
                    receiver_handle = receiver->handle_;
                } else {
                    return false;
                }
            }
            elio::runtime::schedule_handle(receiver_handle);
            return true;
        }

        // Unbounded
        std::coroutine_handle<> receiver_handle = nullptr;
        {
            std::lock_guard<std::mutex> guard(mutex_);
            if (closed_) return false;
            queue_.push(std::move(value));
            if (!recv_waiters_.empty()) {
                auto* receiver = recv_waiters_.pop_front();
                receiver_handle = receiver->handle_;
            }
        }
        if (receiver_handle) {
            elio::runtime::schedule_handle(receiver_handle);
        }
        return true;
    }

    /// Receive a value from the channel
    coro::task<std::optional<T>> recv() {
        if (is_bounded()) {
            while (true) {
                auto val = ring_->try_pop();
                if (val.has_value()) {
                    std::coroutine_handle<> sender_handle = nullptr;
                    {
                        std::lock_guard<std::mutex> guard(mutex_);
                        if (!send_waiters_.empty()) {
                            auto* sender = send_waiters_.front();
                            if (ring_->try_push(sender->value_)) {
                                sender_handle = sender->handle_;
                                send_waiters_.pop_front();
                            }
                        }
                    }
                    if (sender_handle) {
                        elio::runtime::schedule_handle(sender_handle);
                    }
                    co_return val;
                }

                std::coroutine_handle<> sender_handle = nullptr;
                std::optional<T> result;
                bool should_wait = false;
                {
                    std::lock_guard<std::mutex> guard(mutex_);
                    if (!send_waiters_.empty()) {
                        auto* sender = send_waiters_.pop_front();
                        result = std::optional<T>(std::move(sender->value_));
                        sender_handle = sender->handle_;
                    } else if (closed_.load(std::memory_order_acquire)) {
                        if (!queue_.empty()) {
                            result = std::move(queue_.front());
                            queue_.pop();
                        } else {
                            result = std::nullopt;
                        }
                    } else {
                        should_wait = true;
                    }
                }
                if (sender_handle) {
                    elio::runtime::schedule_handle(sender_handle);
                }
                if (should_wait) {
                    recv_awaitable awaitable{*this};
                    co_await awaitable;
                    continue;
                }
                co_return result;
            }
        }

        // Unbounded or rendezvous
        while (true) {
            std::coroutine_handle<> sender_handle = nullptr;
            std::optional<T> result;
            bool should_wait = false;
            {
                std::lock_guard<std::mutex> guard(mutex_);
                if (!queue_.empty()) {
                    result = std::move(queue_.front());
                    queue_.pop();
                } else if (is_rendezvous() && !send_waiters_.empty()) {
                    auto* sender = send_waiters_.pop_front();
                    result = std::optional<T>(std::move(sender->value_));
                    sender_handle = sender->handle_;
                } else if (closed_.load(std::memory_order_acquire)) {
                    result = std::nullopt;
                } else {
                    should_wait = true;
                }
            }
            if (should_wait) {
                recv_awaitable awaitable{*this};
                co_await awaitable;
                continue;
            }
            if (sender_handle) {
                elio::runtime::schedule_handle(sender_handle);
            }
            co_return result;
        }
    }

    /// Try to receive without waiting
    std::optional<T> try_recv() {
        if (is_bounded()) {
            auto val = ring_->try_pop();
            if (val.has_value()) {
                std::coroutine_handle<> sender_handle = nullptr;
                {
                    std::lock_guard<std::mutex> guard(mutex_);
                    if (!send_waiters_.empty()) {
                        auto* sender = send_waiters_.front();
                        if (ring_->try_push(sender->value_)) {
                            sender_handle = sender->handle_;
                            send_waiters_.pop_front();
                        }
                    }
                }
                if (sender_handle) {
                    elio::runtime::schedule_handle(sender_handle);
                }
                return val;
            }

            if (closed_.load(std::memory_order_acquire)) {
                std::lock_guard<std::mutex> guard(mutex_);
                if (!queue_.empty()) {
                    auto result = std::move(queue_.front());
                    queue_.pop();
                    return result;
                }
            }
            return std::nullopt;
        }

        std::coroutine_handle<> sender_handle = nullptr;
        std::optional<T> result;
        {
            std::lock_guard<std::mutex> guard(mutex_);
            if (queue_.empty()) {
                if (is_rendezvous() && !send_waiters_.empty()) {
                    auto* sender = send_waiters_.pop_front();
                    result = std::optional<T>(std::move(sender->value_));
                    sender_handle = sender->handle_;
                } else {
                    return std::nullopt;
                }
            } else {
                result = std::move(queue_.front());
                queue_.pop();
            }
        }
        if (sender_handle) {
            elio::runtime::schedule_handle(sender_handle);
        }
        return result;
    }

    /// Close the channel
    void close() {
        bool expected = false;
        if (!closed_.compare_exchange_strong(expected, true)) {
            return;
        }

        std::vector<std::coroutine_handle<>> to_schedule;
        {
            std::lock_guard<std::mutex> guard(mutex_);

            // Drain ring and send_waiters_ into queue_ for bounded channels
            if (is_bounded()) {
                while (true) {
                    auto val = ring_->try_pop();
                    if (!val.has_value()) break;
                    queue_.push(std::move(*val));
                }
                while (!send_waiters_.empty()) {
                    auto* sender = send_waiters_.pop_front();
                    queue_.push(std::move(sender->value_));
                    to_schedule.push_back(sender->handle_);
                }
            }

            // Drain rendezvous send_waiters_
            if (is_rendezvous()) {
                while (!send_waiters_.empty()) {
                    auto* sender = send_waiters_.pop_front();
                    queue_.push(std::move(sender->value_));
                    to_schedule.push_back(sender->handle_);
                }
            }

            // Wake all waiting receivers
            while (!recv_waiters_.empty()) {
                auto* receiver = recv_waiters_.pop_front();
                to_schedule.push_back(receiver->handle_);
            }
        }

        // Schedule outside lock to avoid deadlock
        for (auto h : to_schedule) {
            elio::runtime::schedule_handle(h);
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
    std::unique_ptr<LockfreeMPMCRing<T>> ring_;
    std::queue<T> queue_;
    elio::detail::intrusive_list<recv_awaitable> recv_waiters_;
    elio::detail::intrusive_list<send_awaitable> send_waiters_;
    size_t capacity_;
    std::atomic<bool> closed_;
};

} // namespace elio::sync
