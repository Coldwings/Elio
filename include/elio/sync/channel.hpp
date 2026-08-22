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
#include "../coro/cancel_token.hpp"
#include "../detail/intrusive_list.hpp"
#include "../runtime/scheduler.hpp"
#include "detail/wake_state.hpp"

namespace elio::sync {

#ifdef ELIO_RUNTIME_TEST_HOOKS
namespace detail {
inline std::atomic<size_t> bounded_send_publish_waits_for_test{0};
inline std::atomic<size_t> bounded_recv_waits_for_test{0};
inline std::atomic<bool> pause_bounded_recv_after_claim_for_test{false};
inline std::atomic<bool> bounded_recv_paused_after_claim_for_test{false};
inline std::atomic<bool> pause_bounded_recv_after_failed_pop_for_test{false};
inline std::atomic<bool> bounded_recv_paused_after_failed_pop_for_test{false};
inline std::atomic<bool> pause_bounded_close_after_publish_for_test{false};
inline std::atomic<bool> bounded_close_paused_after_publish_for_test{false};
inline std::atomic<size_t> bounded_refill_snapshot_locks_for_test{0};
inline std::atomic<size_t> bounded_refill_credit_locks_for_test{0};
inline std::atomic<size_t> bounded_refill_empty_returns_for_test{0};
inline std::atomic<size_t> bounded_refill_token_skips_for_test{0};
inline std::atomic<size_t> bounded_try_send_lock_attempts_for_test{0};
inline std::atomic<bool> pause_bounded_refill_empty_return_for_test{false};
inline std::atomic<bool> bounded_refill_paused_on_empty_for_test{false};
inline std::atomic<bool> pause_bounded_refill_after_dequeue_for_test{false};
inline std::atomic<bool> bounded_refill_paused_after_dequeue_for_test{false};

inline void pause_bounded_refill_on_empty_for_test() noexcept {
    if (!pause_bounded_refill_empty_return_for_test.load(
            std::memory_order_acquire)) {
        return;
    }
    bounded_refill_paused_on_empty_for_test.store(
        true, std::memory_order_release);
    bounded_refill_paused_on_empty_for_test.notify_all();
    while (pause_bounded_refill_empty_return_for_test.load(
        std::memory_order_acquire)) {
        pause_bounded_refill_empty_return_for_test.wait(
            true, std::memory_order_acquire);
    }
    bounded_refill_paused_on_empty_for_test.store(
        false, std::memory_order_release);
}

inline void maybe_pause_bounded_refill_after_dequeue_for_test() noexcept {
    if (!pause_bounded_refill_after_dequeue_for_test.load(
            std::memory_order_acquire)) {
        return;
    }
    bounded_refill_paused_after_dequeue_for_test.store(
        true, std::memory_order_release);
    bounded_refill_paused_after_dequeue_for_test.notify_all();
    while (pause_bounded_refill_after_dequeue_for_test.load(
        std::memory_order_acquire)) {
        pause_bounded_refill_after_dequeue_for_test.wait(
            true, std::memory_order_acquire);
    }
    bounded_refill_paused_after_dequeue_for_test.store(
        false, std::memory_order_release);
}

inline void maybe_pause_bounded_recv_after_failed_pop_for_test() noexcept {
    if (!pause_bounded_recv_after_failed_pop_for_test.load(
            std::memory_order_acquire)) {
        return;
    }
    bounded_recv_paused_after_failed_pop_for_test.store(
        true, std::memory_order_release);
    bounded_recv_paused_after_failed_pop_for_test.notify_all();
    while (pause_bounded_recv_after_failed_pop_for_test.load(
        std::memory_order_acquire)) {
        pause_bounded_recv_after_failed_pop_for_test.wait(
            true, std::memory_order_acquire);
    }
    bounded_recv_paused_after_failed_pop_for_test.store(
        false, std::memory_order_release);
}

inline void maybe_pause_bounded_close_after_publish_for_test() noexcept {
    if (!pause_bounded_close_after_publish_for_test.load(
            std::memory_order_acquire)) {
        return;
    }
    bounded_close_paused_after_publish_for_test.store(
        true, std::memory_order_release);
    bounded_close_paused_after_publish_for_test.notify_all();
    while (pause_bounded_close_after_publish_for_test.load(
        std::memory_order_acquire)) {
        pause_bounded_close_after_publish_for_test.wait(
            true, std::memory_order_acquire);
    }
    bounded_close_paused_after_publish_for_test.store(
        false, std::memory_order_release);
}
}  // namespace detail
#endif

/// Multi-producer multi-consumer channel
template<typename T>
class channel {
public:
    // Public awaitable declarations
    class send_awaitable;
    class cancellable_send_awaitable;
    class recv_awaitable;
    class cancellable_recv_awaitable;

    /// Result of a cancellation-aware send operation.
    struct cancellable_send_result {
        bool sent;
        coro::cancel_result cancel;

        bool success() const noexcept { return sent; }
        bool was_cancelled() const noexcept {
            return cancel == coro::cancel_result::cancelled;
        }
        bool was_closed() const noexcept {
            return !sent && !was_cancelled();
        }
    };

    /// Result of a cancellation-aware receive operation.
    struct cancellable_recv_result {
        std::optional<T> value;
        coro::cancel_result cancel;

        bool success() const noexcept { return value.has_value(); }
        bool was_cancelled() const noexcept {
            return cancel == coro::cancel_result::cancelled;
        }
        bool was_closed() const noexcept {
            return !value.has_value() && !was_cancelled();
        }
    };

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

private:
    class send_waiter_core
        : public elio::detail::intrusive_list_node<send_waiter_core> {
    public:
        send_waiter_core(channel& ch, T& value, bool cancellable = false)
            : ch_(ch)
            , value_(std::addressof(value))
            , wake_state_(detail::make_wake_state())
            , cancellable_(cancellable) {}

        ~send_waiter_core() {
            // Fast path: if we never suspended, we were never enqueued
            if (!suspended_) return;

            // Slow path: acquire mutex to prevent race with close/recv
            std::lock_guard<std::mutex> guard(ch_.mutex_);
            if (this->is_linked()) {
                ch_.send_waiters_.remove(this);
            }
            detail::cancel_wake_state(wake_state_);
        }

        bool await_ready() const noexcept { return false; }

        bool await_suspend(std::coroutine_handle<> h) noexcept {
            if (!wake_state_->set_handle_blocked(h)) {
                return false;
            }

            detail::wake_state_ptr to_schedule;
            bool should_suspend = true;

            {
                std::lock_guard<std::mutex> guard(ch_.mutex_);

                if (cancellable_ && wake_state_->was_cancelled()) {
                    should_suspend = false;
                } else if (ch_.closed_) {
                    (void)claim_completion();
                    should_suspend = false;
                } else if (ch_.is_rendezvous()) {
                    // Rendezvous: direct handoff to a waiting receiver
                    if (auto* receiver = ch_.claim_receiver_locked()) {
                        to_schedule = receiver->wake_state_;
                        if (claim_completion()) {
                            ch_.queue_.push(std::move(*value_));
                            success_ = true;
                        }
                        should_suspend = false;
                    }
                } else if (ch_.is_unbounded()) {
                    if (claim_completion()) {
                        ch_.queue_.push(std::move(*value_));
                        success_ = true;
                        if (auto* receiver = ch_.claim_receiver_locked()) {
                            to_schedule = receiver->wake_state_;
                        }
                    }
                    should_suspend = false;
                } else {
                    // Bounded: try to push into ring
                    if (ch_.ring_->size() < ch_.capacity_ &&
                        ch_.ring_->can_push()) {
                        if (claim_completion()) {
                            const bool pushed = ch_.ring_->try_push(*value_);
                            assert(pushed);
                            (void)pushed;
                            success_ = true;
                            if (auto* receiver =
                                    ch_.claim_receiver_locked()) {
                                to_schedule = receiver->wake_state_;
                            }
                        }
                        // Reusable room selects either normal completion or a
                        // racing cancellation. Neither result may be enqueued.
                        should_suspend = false;
#ifdef ELIO_RUNTIME_TEST_HOOKS
                    } else if (ch_.ring_->size() < ch_.capacity_) {
                        detail::bounded_send_publish_waits_for_test.fetch_add(
                            1, std::memory_order_release);
                        detail::bounded_send_publish_waits_for_test.notify_all();
#endif
                    }
                }

                if (should_suspend) {
                    // Cannot send now — suspend
                    ch_.send_waiters_.push_back(this);
                    suspended_ = true;  // Mark as enqueued
                }
            }

            if (should_suspend) {
                // A successful unblock makes the frame externally resumable.
                // Do not access this awaiter after that transition.
                if (wake_state_->unblock_after_publish()) {
                    return true;
                }

                std::lock_guard<std::mutex> guard(ch_.mutex_);
                if (this->is_linked()) {
                    ch_.send_waiters_.remove(this);
                }
                suspended_ = false;
                return false;
            }

            // Schedule outside lock to avoid deadlock
            if (to_schedule) {
                ch_.schedule_receiver_or_retry(to_schedule);
            }
            return false;
        }

        bool await_resume() noexcept {
            suspended_ = false;
            return success_;
        }

        const detail::wake_state_ptr& cancellation_wake_state() const noexcept {
            return wake_state_;
        }

        cancellable_send_result await_resume_cancellable() noexcept {
            if (wake_state_->was_cancelled()) {
                if (suspended_) {
                    std::lock_guard<std::mutex> guard(ch_.mutex_);
                    if (this->is_linked()) {
                        ch_.send_waiters_.remove(this);
                    }
                }
                suspended_ = false;
                return {false, coro::cancel_result::cancelled};
            }

            suspended_ = false;
            return {success_, coro::cancel_result::completed};
        }

        bool claim_completion() noexcept {
            return !cancellable_ ||
                   detail::claim_wake_state(wake_state_) !=
                       detail::wake_action::rejected;
        }

        T& value() noexcept { return *value_; }
        void mark_success() noexcept { success_ = true; }
        const detail::wake_state_ptr& wake_state() const noexcept {
            return wake_state_;
        }
        bool is_cancellable() const noexcept { return cancellable_; }

    private:
        channel& ch_;
        T* value_;
        detail::wake_state_ptr wake_state_;
        bool success_ = false;
        bool suspended_ = false;  // True if enqueued in send_waiters_
        bool cancellable_ = false;
    };

    class borrowed_cancellable_send_awaitable {
    public:
        borrowed_cancellable_send_awaitable(
                channel& ch, T& value, coro::cancel_token token)
            : core_(ch, value, true) {
            cancel_registration_ = token.on_cancel(
                [state = core_.cancellation_wake_state()] {
                state->request_cancel();
            });
        }

        ~borrowed_cancellable_send_awaitable() {
            cancel_registration_.unregister();
        }

        bool await_ready() const noexcept {
            return core_.cancellation_wake_state()->was_cancelled();
        }

        bool await_suspend(std::coroutine_handle<> h) noexcept {
            return core_.await_suspend(h);
        }

        cancellable_send_result await_resume() noexcept {
            cancel_registration_.unregister();
            return core_.await_resume_cancellable();
        }

    private:
        send_waiter_core core_;
        coro::cancel_token::registration cancel_registration_;
    };

public:
    /// Send awaitable — handles both bounded and rendezvous channels.
    /// Directly constructed awaiters own their value independently.
    class send_awaitable
        : public elio::detail::intrusive_list_node<send_awaitable> {
    public:
        send_awaitable(channel& ch, T value)
            : value_(std::move(value))
            , core_(ch, value_) {}

        bool await_ready() const noexcept { return core_.await_ready(); }

        bool await_suspend(std::coroutine_handle<> h) noexcept {
            return core_.await_suspend(h);
        }

        bool await_resume() noexcept { return core_.await_resume(); }
        bool is_linked() const noexcept { return core_.is_linked(); }

    protected:
        send_awaitable(channel& ch, T value, bool cancellable)
            : value_(std::move(value))
            , core_(ch, value_, cancellable) {}

        const detail::wake_state_ptr& cancellation_wake_state() const noexcept {
            return core_.cancellation_wake_state();
        }

        cancellable_send_result await_resume_cancellable() noexcept {
            return core_.await_resume_cancellable();
        }

    private:
        T value_;
        send_waiter_core core_;
    };

    class cancellable_send_awaitable : public send_awaitable {
    public:
        cancellable_send_awaitable(channel& ch, T value,
                                   coro::cancel_token token)
            : send_awaitable(ch, std::move(value), true) {
            cancel_registration_ = token.on_cancel(
                [state = this->cancellation_wake_state()] {
                state->request_cancel();
            });
        }

        ~cancellable_send_awaitable() {
            cancel_registration_.unregister();
        }

        bool await_ready() const noexcept {
            return this->cancellation_wake_state()->was_cancelled();
        }

        [[nodiscard("check whether the value was sent or cancellation won")]]
        cancellable_send_result await_resume() noexcept {
            cancel_registration_.unregister();
            return this->await_resume_cancellable();
        }

    private:
        coro::cancel_token::registration cancel_registration_;
    };

    /// Recv awaitable — handles bounded, unbounded, and rendezvous channels.
    class recv_awaitable : public elio::detail::intrusive_list_node<recv_awaitable> {
    public:
        explicit recv_awaitable(channel& ch)
            : ch_(ch)
            , wake_state_(detail::make_wake_state()) {}

        ~recv_awaitable() {
            // Fast path: if we never suspended, we were never enqueued
            if (!suspended_) return;

            // Slow path: acquire mutex to prevent race with close/send
            std::lock_guard<std::mutex> guard(ch_.mutex_);
            if (this->is_linked()) {
                ch_.recv_waiters_.remove(this);
            }
            detail::cancel_wake_state(wake_state_);
        }

        bool await_ready() const noexcept { return false; }

        bool await_suspend(std::coroutine_handle<> h) noexcept {
            if (!wake_state_->set_handle_blocked(h)) {
                return false;
            }

            {
                std::lock_guard<std::mutex> guard(ch_.mutex_);

                if (ch_.is_bounded()) {
                    if (!ch_.ring_->empty() || !ch_.send_waiters_.empty() ||
                        ch_.closed_.load(std::memory_order_acquire)) {
                        (void)claim_completion();
                        return false;
                    }
                } else {
                    if (!ch_.queue_.empty() ||
                        ch_.closed_.load(std::memory_order_acquire)) {
                        (void)claim_completion();
                        return false;
                    }
                    if (ch_.is_rendezvous() && !ch_.send_waiters_.empty()) {
                        (void)claim_completion();
                        return false;
                    }
                }

                if (cancellable_ && wake_state_->was_cancelled()) {
                    return false;
                }
#ifdef ELIO_RUNTIME_TEST_HOOKS
                if (ch_.is_bounded()) {
                    detail::bounded_recv_waits_for_test.fetch_add(
                        1, std::memory_order_release);
                    detail::bounded_recv_waits_for_test.notify_all();
                }
#endif
                ch_.recv_waiters_.push_back(this);
                suspended_ = true;  // Mark as enqueued
            }

            // A successful unblock makes the frame externally resumable.
            // Do not access this awaiter after that transition.
            if (wake_state_->unblock_after_publish()) {
                return true;
            }

            {
                std::lock_guard<std::mutex> guard(ch_.mutex_);
                if (this->is_linked()) {
                    ch_.recv_waiters_.remove(this);
                }
                suspended_ = false;
            }
            return false;
        }

        void await_resume() noexcept { suspended_ = false; }

    protected:
        recv_awaitable(channel& ch, bool cancellable)
            : ch_(ch)
            , wake_state_(detail::make_wake_state())
            , cancellable_(cancellable) {}

        const detail::wake_state_ptr& cancellation_wake_state() const noexcept {
            return wake_state_;
        }

        bool claim_completion() noexcept {
            return !cancellable_ ||
                   detail::claim_wake_state(wake_state_) !=
                       detail::wake_action::rejected;
        }

        coro::cancel_result await_resume_cancellable() noexcept {
            if (wake_state_->was_cancelled()) {
                if (suspended_) {
                    std::lock_guard<std::mutex> guard(ch_.mutex_);
                    if (this->is_linked()) {
                        ch_.recv_waiters_.remove(this);
                    }
                }
                suspended_ = false;
                return coro::cancel_result::cancelled;
            }

            suspended_ = false;
            return coro::cancel_result::completed;
        }

    private:
        channel& ch_;
        detail::wake_state_ptr wake_state_;
        bool suspended_ = false;  // True if enqueued in recv_waiters_
        bool cancellable_ = false;

        friend class channel;
    };

    class cancellable_recv_awaitable : public recv_awaitable {
    public:
        cancellable_recv_awaitable(channel& ch, coro::cancel_token token)
            : recv_awaitable(ch, true) {
            cancel_registration_ = token.on_cancel(
                [state = this->cancellation_wake_state()] {
                state->request_cancel();
            });
        }

        ~cancellable_recv_awaitable() {
            cancel_registration_.unregister();
        }

        bool await_ready() const noexcept {
            return this->cancellation_wake_state()->was_cancelled();
        }

        [[nodiscard("check whether a value, close, or cancellation won")]]
        coro::cancel_result await_resume() noexcept {
            cancel_registration_.unregister();
            return this->await_resume_cancellable();
        }

    private:
        coro::cancel_token::registration cancel_registration_;
    };

    /// Send a value to the channel
    auto send(T value) -> coro::task<bool> {
        if (closed_.load(std::memory_order_acquire)) {
            co_return false;
        }

        if (is_unbounded()) {
            detail::wake_state_ptr receiver_handle;
            {
                std::lock_guard<std::mutex> guard(mutex_);
                if (closed_) {
                    co_return false;
                }
                queue_.push(std::move(value));
                if (auto* receiver = claim_receiver_locked()) {
                    receiver_handle = receiver->wake_state_;
                }
            }
            if (receiver_handle) {
                schedule_receiver_or_retry(receiver_handle);
            }
            co_return true;
        }

        // Bounded or rendezvous: try fast path, then suspend
        if (is_bounded() && ring_->size() < capacity_) {
            bool pushed = false;
            detail::wake_state_ptr receiver_handle;
            {
                std::lock_guard<std::mutex> guard(mutex_);
                if (!closed_ && ring_->size() < capacity_ &&
                    ring_->can_push()) {
                    const bool did_push = ring_->try_push(value);
                    assert(did_push);
                    (void)did_push;
                    pushed = true;
                    if (auto* receiver = claim_receiver_locked()) {
                        receiver_handle = receiver->wake_state_;
                    }
                }
            }
            if (receiver_handle) {
                schedule_receiver_or_retry(receiver_handle);
            }
            if (pushed) {
                co_return true;
            }
            if (closed_.load(std::memory_order_acquire)) {
                co_return false;
            }
        }

        // The coroutine frame owns value for the complete suspended lifetime.
        send_waiter_core awaitable{*this, value};
        bool pushed = co_await awaitable;

        if (pushed) {
            co_return true;
        }
        co_return !closed_.load(std::memory_order_acquire);
    }

    /// Send a value, preserving closed-channel status when cancellation races.
    /// A cancellation winner does not transfer the value into the channel.
    coro::task<cancellable_send_result> send(
            T value, coro::cancel_token token) {
        co_return co_await borrowed_cancellable_send_awaitable(
            *this, value, std::move(token));
    }

    /// Try to send without waiting
    bool try_send(T value) {
        if (closed_.load(std::memory_order_acquire)) {
            return false;
        }

        if (is_bounded()) {
            detail::wake_state_ptr receiver_handle;
            {
#ifdef ELIO_RUNTIME_TEST_HOOKS
                detail::bounded_try_send_lock_attempts_for_test.fetch_add(
                    1, std::memory_order_release);
                detail::bounded_try_send_lock_attempts_for_test.notify_all();
#endif
                std::lock_guard<std::mutex> guard(mutex_);
                if (closed_) return false;
                if (ring_->size() >= capacity_) return false;
                if (ring_->try_push(value)) {
                    if (auto* receiver = claim_receiver_locked()) {
                        receiver_handle = receiver->wake_state_;
                    }
                } else {
                    return false;
                }
            }
            if (receiver_handle) {
                schedule_receiver_or_retry(receiver_handle);
            }
            return true;
        }

        if (is_rendezvous()) {
            detail::wake_state_ptr receiver_handle;
            {
                std::lock_guard<std::mutex> guard(mutex_);
                if (closed_) return false;
                if (auto* receiver = claim_receiver_locked()) {
                    queue_.push(std::move(value));
                    receiver_handle = receiver->wake_state_;
                } else {
                    return false;
                }
            }
            schedule_receiver_or_retry(receiver_handle);
            return true;
        }

        // Unbounded
        detail::wake_state_ptr receiver_handle;
        {
            std::lock_guard<std::mutex> guard(mutex_);
            if (closed_) return false;
            queue_.push(std::move(value));
            if (auto* receiver = claim_receiver_locked()) {
                receiver_handle = receiver->wake_state_;
            }
        }
        if (receiver_handle) {
            schedule_receiver_or_retry(receiver_handle);
        }
        return true;
    }

    /// Receive a value from the channel
    coro::task<std::optional<T>> recv() {
        if (is_bounded()) {
            while (true) {
                auto result = ring_->try_pop();
                if (result.has_value()) {
                    refill_senders_after_pop();
                    co_return result;
                }
#ifdef ELIO_RUNTIME_TEST_HOOKS
                detail::maybe_pause_bounded_recv_after_failed_pop_for_test();
#endif

                detail::wake_state_ptr sender_handle;
                bool should_wait = false;
                bool refill_senders = false;
                {
                    std::lock_guard<std::mutex> guard(mutex_);
                    // Revalidate before sender/closed selection: a producer
                    // can publish after the optimistic empty observation.
                    auto locked_result = ring_->try_pop();
                    if (locked_result.has_value()) {
                        result.emplace(std::move(*locked_result));
                        refill_senders = true;
                    } else if (auto* sender = claim_sender_locked()) {
                        result = std::optional<T>(std::move(sender->value()));
                        sender->mark_success();
                        sender_handle = sender->wake_state();
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
                    detail::schedule_wake_state(sender_handle);
                }
                if (refill_senders) {
                    refill_senders_after_pop();
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
            detail::wake_state_ptr sender_handle;
            std::optional<T> result;
            bool should_wait = false;
            {
                std::lock_guard<std::mutex> guard(mutex_);
                if (!queue_.empty()) {
                    result = std::move(queue_.front());
                    queue_.pop();
                } else if (is_rendezvous()) {
                    auto* sender = claim_sender_locked();
                    if (sender) {
                        result = std::optional<T>(
                            std::move(sender->value()));
                        sender->mark_success();
                        sender_handle = sender->wake_state();
                    } else if (closed_.load(std::memory_order_acquire)) {
                        result = std::nullopt;
                    } else {
                        should_wait = true;
                    }
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
                detail::schedule_wake_state(sender_handle);
            }
            co_return result;
        }
    }

    /// Receive a value while preserving closed-channel status on cancellation.
    /// A cancellation winner does not consume a value from the channel.
    coro::task<cancellable_recv_result> recv(coro::cancel_token token) {
        bool notification_selected = false;
        while (true) {
            // Give a selected normal notification one cancellation-immune
            // attempt to consume the advertised state. Notifications do not
            // reserve values, so restore the caller token if another receiver
            // wins that attempt.
            const bool consume_notification =
                std::exchange(notification_selected, false);
            auto effective_token = consume_notification
                ? coro::cancel_token{}
                : token;
            cancellable_recv_awaitable awaitable{
                *this, std::move(effective_token)};
            detail::wake_state_ptr sender_handle;
            std::optional<T> result;
            bool resolved = false;
            bool retry = false;
            bool refill_senders = false;
#ifdef ELIO_RUNTIME_TEST_HOOKS
            bool bounded_pop_failed = false;
#endif

            {
                std::lock_guard<std::mutex> guard(mutex_);

                if (is_bounded()) {
                    if (!ring_->empty()) {
                        if (!awaitable.claim_completion()) {
                            resolved = true;
                        } else {
#ifdef ELIO_RUNTIME_TEST_HOOKS
                            if (detail::pause_bounded_recv_after_claim_for_test.load(
                                    std::memory_order_acquire)) {
                                detail::bounded_recv_paused_after_claim_for_test.store(
                                    true, std::memory_order_release);
                                detail::bounded_recv_paused_after_claim_for_test.notify_all();
                                while (detail::pause_bounded_recv_after_claim_for_test.load(
                                    std::memory_order_acquire)) {
                                    detail::pause_bounded_recv_after_claim_for_test.wait(
                                        true, std::memory_order_acquire);
                                }
                                detail::bounded_recv_paused_after_claim_for_test.store(
                                    false, std::memory_order_release);
                            }
#endif
                            result = ring_->try_pop();
                            if (!result.has_value()) {
                                retry = true;
#ifdef ELIO_RUNTIME_TEST_HOOKS
                                bounded_pop_failed = true;
#endif
                            } else {
                                resolved = true;
                                if (send_waiters_.empty()) {
#ifdef ELIO_RUNTIME_TEST_HOOKS
                                    detail::bounded_refill_token_skips_for_test
                                        .fetch_add(1, std::memory_order_relaxed);
                                    detail::pause_bounded_refill_on_empty_for_test();
#endif
                                } else {
                                    refill_senders = true;
                                }
                            }
                        }
                    } else if (!send_waiters_.empty()) {
                        if (!awaitable.claim_completion()) {
                            resolved = true;
                        } else if (auto* sender = claim_sender_locked()) {
                            result = std::optional<T>(
                                std::move(sender->value()));
                            sender->mark_success();
                            sender_handle = sender->wake_state();
                            resolved = true;
                        } else {
                            retry = true;
                        }
                    } else if (closed_.load(std::memory_order_acquire)) {
                        if (awaitable.claim_completion() && !queue_.empty()) {
                            result = std::move(queue_.front());
                            queue_.pop();
                        }
                        resolved = true;
                    }
                } else if (!queue_.empty()) {
                    if (!awaitable.claim_completion()) {
                        resolved = true;
                    } else {
                        result = std::move(queue_.front());
                        queue_.pop();
                        resolved = true;
                    }
                } else if (is_rendezvous() && !send_waiters_.empty()) {
                    if (!awaitable.claim_completion()) {
                        resolved = true;
                    } else if (auto* sender = claim_sender_locked()) {
                        result = std::optional<T>(std::move(sender->value()));
                        sender->mark_success();
                        sender_handle = sender->wake_state();
                        resolved = true;
                    } else {
                        retry = true;
                    }
                } else if (closed_.load(std::memory_order_acquire)) {
                    (void)awaitable.claim_completion();
                    resolved = true;
                }
            }

            if (sender_handle) {
                detail::schedule_wake_state(sender_handle);
            }
            if (refill_senders) {
                refill_senders_after_pop();
            }

            if (resolved) {
                const auto completion = awaitable.await_resume();
                if (completion == coro::cancel_result::cancelled) {
                    co_return cancellable_recv_result{
                        std::nullopt, coro::cancel_result::cancelled};
                }
                co_return cancellable_recv_result{
                    std::move(result), coro::cancel_result::completed};
            }

            if (retry) {
                const auto completion = awaitable.await_resume();
                if (completion == coro::cancel_result::cancelled) {
                    co_return cancellable_recv_result{
                        std::nullopt, coro::cancel_result::cancelled};
                }
#ifdef ELIO_RUNTIME_TEST_HOOKS
                if (bounded_pop_failed) {
                    detail::maybe_pause_bounded_recv_after_failed_pop_for_test();
                }
#endif
                continue;
            }

            if (consume_notification) {
                continue;
            }

            const auto completion = co_await awaitable;
            if (completion == coro::cancel_result::cancelled) {
                co_return cancellable_recv_result{
                    std::nullopt, coro::cancel_result::cancelled};
            }
            notification_selected = true;
        }
    }

    /// Try to receive without waiting
    std::optional<T> try_recv() {
        if (is_bounded()) {
            auto result = ring_->try_pop();
            if (!result.has_value()) {
                if (closed_.load(std::memory_order_acquire)) {
                    std::lock_guard<std::mutex> guard(mutex_);
                    if (!queue_.empty()) {
                        result = std::move(queue_.front());
                        queue_.pop();
                    }
                }
                return result;
            }

            refill_senders_after_pop();
            return result;
        }

        detail::wake_state_ptr sender_handle;
        std::optional<T> result;
        {
            std::lock_guard<std::mutex> guard(mutex_);
            if (queue_.empty()) {
                if (is_rendezvous()) {
                    auto* sender = claim_sender_locked();
                    if (!sender) {
                        return std::nullopt;
                    }
                    result = std::optional<T>(std::move(sender->value()));
                    sender->mark_success();
                    sender_handle = sender->wake_state();
                } else {
                    return std::nullopt;
                }
            } else {
                result = std::move(queue_.front());
                queue_.pop();
            }
        }
        if (sender_handle) {
            detail::schedule_wake_state(sender_handle);
        }
        return result;
    }

    /// Close the channel. Calling close() again is a no-op.
    void close() {
        bool expected = false;
        if (!closed_.compare_exchange_strong(expected, true)) {
            return;
        }
#ifdef ELIO_RUNTIME_TEST_HOOKS
        if (is_bounded()) {
            detail::maybe_pause_bounded_close_after_publish_for_test();
        }
#endif

        std::vector<detail::wake_state_ptr> to_schedule;
        {
            std::lock_guard<std::mutex> guard(mutex_);
            to_schedule.reserve(send_waiters_.size() + recv_waiters_.size());

            // Drain ring and send_waiters_ into queue_ for bounded channels
            if (is_bounded()) {
                while (true) {
                    auto val = ring_->try_pop();
                    if (!val.has_value()) break;
                    queue_.push(std::move(*val));
                }
                while (auto* sender = claim_sender_locked()) {
                    queue_.push(std::move(sender->value()));
                    sender->mark_success();  // Value was delivered to queue
                    to_schedule.push_back(sender->wake_state());
                }
            }

            // Drain rendezvous send_waiters_
            if (is_rendezvous()) {
                while (auto* sender = claim_sender_locked()) {
                    queue_.push(std::move(sender->value()));
                    sender->mark_success();  // Value was delivered to queue
                    to_schedule.push_back(sender->wake_state());
                }
            }

            // Wake all waiting receivers
            while (auto* receiver = claim_receiver_locked()) {
                to_schedule.push_back(receiver->wake_state_);
            }
        }

        // Schedule outside lock to avoid deadlock
        detail::schedule_wake_states(to_schedule);
    }

    /// Check if channel is closed
    bool is_closed() const noexcept {
        return closed_.load(std::memory_order_acquire);
    }

    /// Get the number of currently buffered values.
    ///
    /// @note This is an exact count only while the channel is quiescent. Under
    /// concurrent send, receive, or close operations, the result is an
    /// approximate diagnostic observation and may already be stale when
    /// returned. Do not use it for synchronization.
    size_t size() const noexcept {
        std::lock_guard<std::mutex> guard(mutex_);
        if (is_bounded()) {
            return ring_->size() + queue_.size();
        }
        return queue_.size();
    }

    /// Check whether the channel currently has no buffered values.
    ///
    /// @note This is exact only while the channel is quiescent. Under concurrent
    /// send, receive, or close operations, the result is an approximate
    /// diagnostic observation and may already be stale when returned. Do not
    /// use it for synchronization.
    bool empty() const noexcept {
        std::lock_guard<std::mutex> guard(mutex_);
        if (is_bounded()) {
            return ring_->empty() && queue_.empty();
        }
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

    recv_awaitable* claim_receiver_locked() noexcept {
        while (!recv_waiters_.empty()) {
            auto* receiver = recv_waiters_.pop_front();
            if (receiver->cancellable_ &&
                detail::claim_wake_state(receiver->wake_state_) ==
                    detail::wake_action::rejected) {
                continue;
            }
            return receiver;
        }
        return nullptr;
    }

    send_waiter_core* claim_sender_locked() noexcept {
        while (!send_waiters_.empty()) {
            auto* sender = send_waiters_.pop_front();
            if (sender->is_cancellable() &&
                detail::claim_wake_state(sender->wake_state()) ==
                    detail::wake_action::rejected) {
                continue;
            }
            return sender;
        }
        return nullptr;
    }

    void refill_senders_after_pop() {
        // Out-of-order consumers can publish several reusable slots before the
        // next producer slot becomes available. Drain every currently usable
        // refill credit so queued senders cannot be stranded without a future
        // pop to wake them. Snapshot the logical credit so sustained traffic
        // cannot keep one receiver servicing later arrivals indefinitely.
        size_t refill_budget;
        {
            std::lock_guard<std::mutex> guard(mutex_);
#ifdef ELIO_RUNTIME_TEST_HOOKS
            detail::bounded_refill_snapshot_locks_for_test.fetch_add(
                1, std::memory_order_relaxed);
#endif
            // Enqueue and this observation use the same mutex. A sender that
            // arrives after an empty observation either uses the newly
            // reusable producer slot directly or, after out-of-order pops,
            // is refilled by the consumer that publishes the next slot.
            if (send_waiters_.empty()) {
#ifdef ELIO_RUNTIME_TEST_HOOKS
                detail::bounded_refill_empty_returns_for_test.fetch_add(
                    1, std::memory_order_relaxed);
                detail::pause_bounded_refill_on_empty_for_test();
#endif
                return;
            }
            const size_t logical_size = ring_->size();
            refill_budget = logical_size < capacity_
                ? capacity_ - logical_size
                : 0;
        }

        while (refill_budget > 0) {
            --refill_budget;
            detail::wake_state_ptr sender_handle;
            detail::wake_state_ptr receiver_handle;
            {
                std::lock_guard<std::mutex> guard(mutex_);
#ifdef ELIO_RUNTIME_TEST_HOOKS
                detail::bounded_refill_credit_locks_for_test.fetch_add(
                    1, std::memory_order_relaxed);
#endif
                if (ring_->size() >= capacity_ || !ring_->can_push()) {
                    return;
                }

                auto* sender = claim_sender_locked();
                if (!sender) {
                    return;
                }

                const bool pushed = ring_->try_push(sender->value());
                assert(pushed);
                (void)pushed;
                sender->mark_success();
                sender_handle = sender->wake_state();
                if (auto* receiver = claim_receiver_locked()) {
                    receiver_handle = receiver->wake_state_;
                }
            }

#ifdef ELIO_RUNTIME_TEST_HOOKS
            detail::maybe_pause_bounded_refill_after_dequeue_for_test();
#endif
            if (receiver_handle) {
                schedule_receiver_or_retry(receiver_handle);
            }
            detail::schedule_wake_state(sender_handle);
        }
    }

    void schedule_receiver_or_retry(detail::wake_state_ptr receiver) {
        while (receiver) {
            if (detail::schedule_wake_state(receiver) ||
                receiver->notification_was_selected()) {
                return;
            }

            std::lock_guard<std::mutex> guard(mutex_);
            auto* next = claim_receiver_locked();
            receiver = next ? next->wake_state_ : nullptr;
        }
    }

    mutable std::mutex mutex_;
    std::unique_ptr<LockfreeMPMCRing<T>> ring_;
    std::queue<T> queue_;
    elio::detail::intrusive_list<recv_awaitable> recv_waiters_;
    elio::detail::intrusive_list<send_waiter_core> send_waiters_;
    size_t capacity_;
    std::atomic<bool> closed_;
};

} // namespace elio::sync
