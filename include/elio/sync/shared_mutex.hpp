#pragma once

#include <coroutine>
#include <atomic>
#include <cstddef>
#include <mutex>
#include <utility>
#include "../coro/cancel_token.hpp"
#include "../detail/intrusive_list.hpp"
#include "../runtime/scheduler.hpp"
#include "detail/wake_state.hpp"

namespace elio::sync {

#ifdef ELIO_RUNTIME_TEST_HOOKS
namespace detail {
inline std::atomic<bool> force_shared_reader_slow_path_for_test{false};
inline std::atomic<bool> pause_shared_reader_after_locked_load_for_test{false};
inline std::atomic<bool> shared_reader_paused_after_locked_load_for_test{false};
inline std::atomic<size_t> shared_reader_unlocked_retries_for_test{0};
inline std::atomic<size_t> shared_reader_waiter_publications_for_test{0};
inline std::atomic<bool>
    pause_shared_reader_after_wake_allocation_for_test{false};
inline std::atomic<bool>
    shared_reader_paused_after_wake_allocation_for_test{false};
}
#endif

/// Coroutine-aware shared mutex (read-write lock)
/// Allows multiple readers or a single writer
///
/// Optimized with atomic fast paths for readers:
/// - try_lock_shared uses atomic fetch_add without mutex
/// - Reader-heavy workloads see ~100x improvement
///
/// State encoding (64-bit):
/// - Bit 63: writer_waiting flag
/// - Bit 62: writer_active flag
/// - Bits 0-61: reader_count (max ~4.6 quintillion readers)
///
/// Each successful shared acquisition must be matched by exactly one release.
/// At most (1ULL << 62) - 1 shared acquisitions may be outstanding at once;
/// exceeding that representation limit is outside the supported contract.
class shared_mutex {
public:
    shared_mutex() = default;
    ~shared_mutex() = default;

    // Non-copyable, non-movable
    shared_mutex(const shared_mutex&) = delete;
    shared_mutex& operator=(const shared_mutex&) = delete;
    shared_mutex(shared_mutex&&) = delete;
    shared_mutex& operator=(shared_mutex&&) = delete;

private:
    // State bit masks
    static constexpr uint64_t WRITER_ACTIVE = 1ULL << 62;
    static constexpr uint64_t WRITER_WAITING = 1ULL << 63;
    static constexpr uint64_t READER_MASK = (1ULL << 62) - 1;
    static constexpr uint64_t WRITER_FLAGS = WRITER_ACTIVE | WRITER_WAITING;

public:
    class lock_shared_waiter;
    class cancellable_lock_shared_waiter;
    class lock_shared_awaitable;
    class cancellable_lock_shared_awaitable;
    class lock_waiter;
    class cancellable_lock_waiter;
    class lock_awaitable;
    class cancellable_lock_awaitable;

    /// Shared lock waiter (for readers).
    class lock_shared_waiter : public elio::detail::intrusive_list_node<lock_shared_waiter> {
    public:
        explicit lock_shared_waiter(shared_mutex& m)
            : mtx_(m) {}

        lock_shared_waiter(shared_mutex& m, bool cancellable)
            : mtx_(m)
            , cancellable_(cancellable) {
            if (cancellable_) {
                wake_state_ = detail::make_wake_state();
            }
        }

        ~lock_shared_waiter() {
            // Fast path: if we never suspended, we were never enqueued
            if (!suspended_) return;

            bool release_grant = false;
            // Slow path: acquire internal_mutex_ to prevent race with unlock
            {
                std::lock_guard<std::mutex> guard(mtx_.internal_mutex_);
                if (this->is_linked()) {
                    mtx_.reader_waiters_.remove(this);
                    detail::cancel_wake_state(wake_state_);
                } else if (grant_pending_ && !resumed_) {
                    detail::cancel_wake_state(wake_state_);
                    grant_pending_ = false;
                    release_grant = true;
                } else {
                    detail::cancel_wake_state(wake_state_);
                }
            }

            if (release_grant) {
                mtx_.unlock_shared();
            }
        }

        bool await_ready_non_cancellable_impl() const noexcept {
            assert(!cancellable_);
#ifdef ELIO_RUNTIME_TEST_HOOKS
            if (detail::force_shared_reader_slow_path_for_test.load(
                    std::memory_order_acquire)) {
                return false;
            }
#endif
            return mtx_.try_lock_shared();
        }

        bool await_ready_impl() const {
            if (!cancellable_) {
                return await_ready_non_cancellable_impl();
            }
            if (wake_state_->was_cancelled()) {
                return true;
            }
#ifdef ELIO_RUNTIME_TEST_HOOKS
            if (detail::force_shared_reader_slow_path_for_test.load(
                    std::memory_order_acquire)) {
                return false;
            }
#endif
            if (!mtx_.try_lock_shared()) {
                return false;
            }
            if (detail::claim_wake_state(wake_state_) !=
                detail::wake_action::rejected) {
                return true;
            }

            // Cancellation won after the reader slot was acquired.
            mtx_.unlock_shared();
            return true;
        }

    private:
        template<bool Cancellable>
        bool await_suspend_mode_impl(std::coroutine_handle<> awaiter) {
            assert(cancellable_ == Cancellable);
            if constexpr (Cancellable) {
                if (wake_state_->was_cancelled() ||
                    !wake_state_->set_handle_blocked(awaiter)) {
                    return false;
                }
            }

#ifdef ELIO_RUNTIME_TEST_HOOKS
            bool force_slow_path =
                detail::force_shared_reader_slow_path_for_test.load(
                    std::memory_order_acquire);
#else
            bool force_slow_path = false;
#endif
            for (;;) {
                // Lock-free fast path: attempt to increment reader count
                // without the mutex. This also handles a locked recheck that
                // lost its one-shot CAS to another lock-free reader.
                uint64_t state = mtx_.state_.load(std::memory_order_relaxed);
                while (!force_slow_path && !(state & WRITER_FLAGS)) {
                    // Overflow guard: if reader count is at max, fall through
                    // to the unchanged slow-path saturation behavior.
                    if ((state & READER_MASK) == READER_MASK) break;
                    if (mtx_.state_.compare_exchange_weak(
                            state, state + 1, std::memory_order_acquire,
                            std::memory_order_relaxed)) {
                        if constexpr (!Cancellable) {
                            return false;
                        } else {
                            if (detail::claim_wake_state(wake_state_) !=
                                detail::wake_action::rejected) {
                                return false;
                            }
                            mtx_.unlock_shared();
                            return false;
                        }
                    }
                    // CAS failed, state updated - loop rechecks writer flags.
                }
                // The test seam forces only the initial locked recheck. A
                // retry after that recheck uses this ordinary lock-free path.
                const bool needs_wake_state =
                    !Cancellable && !wake_state_;
                if (needs_wake_state) {
                    assert(force_slow_path || (state & WRITER_FLAGS) ||
                           (state & READER_MASK) == READER_MASK);
                }
                force_slow_path = false;

                // Slow path: a writer is present or the reader count is
                // saturated. A dequeued grant may outlive the coroutine frame,
                // so allocate independent wake ownership before taking the
                // queue mutex. Failure leaves the state and queue untouched.
                if (needs_wake_state) {
                    wake_state_ = detail::make_wake_state();
#ifdef ELIO_RUNTIME_TEST_HOOKS
                    if (detail::pause_shared_reader_after_wake_allocation_for_test
                            .load(std::memory_order_acquire)) {
                        detail::shared_reader_paused_after_wake_allocation_for_test
                            .store(true, std::memory_order_release);
                        detail::shared_reader_paused_after_wake_allocation_for_test
                            .notify_all();
                        while (detail::pause_shared_reader_after_wake_allocation_for_test
                                   .load(std::memory_order_acquire)) {
                            detail::pause_shared_reader_after_wake_allocation_for_test
                                .wait(true, std::memory_order_acquire);
                        }
                        detail::shared_reader_paused_after_wake_allocation_for_test
                            .store(false, std::memory_order_release);
                    }
#endif
                }

                bool release_reader = false;
                bool retry_unlocked = false;
                {
                    std::lock_guard<std::mutex> guard(mtx_.internal_mutex_);

                    if constexpr (Cancellable) {
                        if (wake_state_->was_cancelled()) {
                            return false;
                        }
                    }

                    // Double-check under lock - state may have changed.
                    state = mtx_.state_.load(std::memory_order_relaxed);
#ifdef ELIO_RUNTIME_TEST_HOOKS
                    if (!(state & WRITER_FLAGS) &&
                        (state & READER_MASK) != READER_MASK &&
                        detail::pause_shared_reader_after_locked_load_for_test
                            .load(std::memory_order_acquire)) {
                        detail::shared_reader_paused_after_locked_load_for_test
                            .store(true, std::memory_order_release);
                        detail::shared_reader_paused_after_locked_load_for_test
                            .notify_all();
                        while (detail::pause_shared_reader_after_locked_load_for_test
                                   .load(std::memory_order_acquire)) {
                            detail::pause_shared_reader_after_locked_load_for_test
                                .wait(true, std::memory_order_acquire);
                        }
                        detail::shared_reader_paused_after_locked_load_for_test
                            .store(false, std::memory_order_release);
                    }
#endif
                    // A lock-free reader or try_lock writer can still change
                    // state_ while internal_mutex_ is held. Retain one locked
                    // CAS. If it loses and the updated state still admits a
                    // representable reader, retry after releasing the mutex.
                    if (!(state & WRITER_FLAGS) &&
                        (state & READER_MASK) != READER_MASK) {
                        if (mtx_.state_.compare_exchange_strong(
                                state, state + 1, std::memory_order_acquire,
                                std::memory_order_relaxed)) {
                            if constexpr (!Cancellable) {
                                return false;
                            } else {
                                if (detail::claim_wake_state(wake_state_) !=
                                    detail::wake_action::rejected) {
                                    return false;
                                }
                                release_reader = true;
                            }
                        } else if (!(state & WRITER_FLAGS) &&
                                   (state & READER_MASK) != READER_MASK) {
                            retry_unlocked = true;
#ifdef ELIO_RUNTIME_TEST_HOOKS
                            detail::shared_reader_unlocked_retries_for_test
                                .fetch_add(1, std::memory_order_relaxed);
#endif
                        }
                    }

                    if (!release_reader && !retry_unlocked) {
                        if constexpr (!Cancellable) {
                            wake_state_->set_handle(awaiter);
                        }
                        suspended_ = true;
                        mtx_.reader_waiters_.push_back(this);
#ifdef ELIO_RUNTIME_TEST_HOOKS
                        detail::shared_reader_waiter_publications_for_test
                            .fetch_add(1, std::memory_order_release);
#endif
                        if constexpr (!Cancellable) {
                            return true;
                        } else if (wake_state_->unblock_after_publish()) {
                            return true;
                        }

                        mtx_.reader_waiters_.remove(this);
                        suspended_ = false;
                    }
                }

                if (release_reader) {
                    mtx_.unlock_shared();
                    return false;
                }
                if (!retry_unlocked) {
                    return false;
                }
            }
        }

    public:
        bool await_suspend_non_cancellable_impl(
                std::coroutine_handle<> awaiter) {
            return await_suspend_mode_impl<false>(awaiter);
        }

        bool await_suspend_cancellable_impl(
                std::coroutine_handle<> awaiter) {
            return await_suspend_mode_impl<true>(awaiter);
        }

        bool await_suspend_impl(std::coroutine_handle<> awaiter) {
            return cancellable_
                ? await_suspend_cancellable_impl(awaiter)
                : await_suspend_non_cancellable_impl(awaiter);
        }

        void await_resume_non_cancellable_impl() noexcept {
            assert(!cancellable_);
            resumed_ = true;
            grant_pending_ = false;
            suspended_ = false;
        }

        coro::cancel_result await_resume_impl() noexcept {
            if (!cancellable_) {
                await_resume_non_cancellable_impl();
                return coro::cancel_result::completed;
            }
            if (wake_state_->was_cancelled()) {
                if (suspended_) {
                    std::lock_guard<std::mutex> guard(mtx_.internal_mutex_);
                    if (this->is_linked()) {
                        mtx_.reader_waiters_.remove(this);
                    }
                }
                suspended_ = false;
                return coro::cancel_result::cancelled;
            }

            resumed_ = true;
            grant_pending_ = false;
            suspended_ = false;
            return coro::cancel_result::completed;
        }

    protected:
        const detail::wake_state_ptr& cancellation_wake_state() const noexcept {
            return wake_state_;
        }

    private:
        shared_mutex& mtx_;
        detail::wake_state_ptr wake_state_;
        bool cancellable_ = false;
        bool suspended_ = false;
        bool resumed_ = false;
        bool grant_pending_ = false;

        friend class shared_mutex;
    };

    class cancellable_lock_shared_waiter : public lock_shared_waiter {
    public:
        cancellable_lock_shared_waiter(shared_mutex& m,
                                       coro::cancel_token token)
            : lock_shared_waiter(m, true) {
            cancel_registration_ = token.on_cancel(
                [state = cancellation_wake_state()] {
                state->request_cancel();
            });
        }

        ~cancellable_lock_shared_waiter() {
            cancel_registration_.unregister();
        }

        coro::cancel_result await_resume_cancellable() noexcept {
            cancel_registration_.unregister();
            return await_resume_impl();
        }

    private:
        coro::cancel_token::registration cancel_registration_;
    };

    class lock_shared_awaitable {
    public:
        explicit lock_shared_awaitable(shared_mutex& m) : waiter_(m) {}

        bool await_ready() const noexcept {
            return waiter_.await_ready_non_cancellable_impl();
        }
        bool await_suspend(std::coroutine_handle<> awaiter) {
            return waiter_.await_suspend_non_cancellable_impl(awaiter);
        }
        void await_resume() noexcept {
            waiter_.await_resume_non_cancellable_impl();
        }

    private:
        lock_shared_waiter waiter_;
    };

    class cancellable_lock_shared_awaitable {
    public:
        cancellable_lock_shared_awaitable(shared_mutex& m,
                                          coro::cancel_token token)
            : waiter_(m, std::move(token)) {}

        bool await_ready() const { return waiter_.await_ready_impl(); }
        bool await_suspend(std::coroutine_handle<> awaiter) {
            return waiter_.await_suspend_cancellable_impl(awaiter);
        }
        [[nodiscard("check whether the shared lock was acquired")]]
        coro::cancel_result await_resume() noexcept {
            return waiter_.await_resume_cancellable();
        }

    private:
        cancellable_lock_shared_waiter waiter_;
    };

    /// Exclusive lock waiter (for writers).
    class lock_waiter : public elio::detail::intrusive_list_node<lock_waiter> {
    public:
        explicit lock_waiter(shared_mutex& m)
            : mtx_(m)
            , wake_state_(detail::make_wake_state()) {}

        lock_waiter(shared_mutex& m, bool cancellable)
            : mtx_(m)
            , wake_state_(detail::make_wake_state())
            , cancellable_(cancellable) {}

        ~lock_waiter() {
            // Fast path: if we never suspended, we were never enqueued
            if (!suspended_) return;

            detail::wake_state_ptr reader_to_wake;
            bool wake_more_readers = false;
            bool release_grant = false;
            {
                std::lock_guard<std::mutex> guard(mtx_.internal_mutex_);
                if (this->is_linked()) {
                    mtx_.writer_waiters_.remove(this);
                    detail::cancel_wake_state(wake_state_);
                    assert(mtx_.pending_writers_ > 0);
                    --mtx_.pending_writers_;
                    reader_to_wake =
                        mtx_.finish_writer_removal_locked(wake_more_readers);
                } else if (grant_pending_ && !resumed_) {
                    detail::cancel_wake_state(wake_state_);
                    grant_pending_ = false;
                    release_grant = true;
                } else {
                    detail::cancel_wake_state(wake_state_);
                }
            }

            if (reader_to_wake) {
                detail::schedule_wake_state(reader_to_wake);
            }
            if (wake_more_readers) {
                mtx_.wake_additional_readers();
            }
            if (release_grant) {
                mtx_.unlock();
            }
        }

        bool await_ready_impl() const {
            if (cancellable_ && wake_state_->was_cancelled()) {
                return true;
            }
            if (!mtx_.try_lock()) {
                return false;
            }
            if (!cancellable_ ||
                detail::claim_wake_state(wake_state_) !=
                    detail::wake_action::rejected) {
                return true;
            }

            // Cancellation won after exclusive ownership was acquired.
            mtx_.unlock();
            return true;
        }

        bool await_suspend_impl(std::coroutine_handle<> awaiter) {
            if (cancellable_) {
                if (wake_state_->was_cancelled() ||
                    !wake_state_->set_handle_blocked(awaiter)) {
                    return false;
                }
            }

            detail::wake_state_ptr reader_to_wake;
            bool wake_more_readers = false;
            bool release_writer = false;
            {
                std::lock_guard<std::mutex> guard(mtx_.internal_mutex_);

                if (cancellable_ && wake_state_->was_cancelled()) {
                    return false;
                }

                // Try to acquire write lock atomically
                uint64_t expected = 0;
                if (mtx_.state_.compare_exchange_strong(
                        expected, WRITER_ACTIVE, std::memory_order_acquire,
                        std::memory_order_relaxed)) {
                    if (!cancellable_ ||
                        detail::claim_wake_state(wake_state_) !=
                            detail::wake_action::rejected) {
                        return false;
                    }
                    release_writer = true;
                } else {
                    // Publish WRITER_WAITING before enqueuing and retry the
                    // just-released state to close the last-reader window.
                    mtx_.state_.fetch_or(WRITER_WAITING,
                                         std::memory_order_acq_rel);

                    expected = WRITER_WAITING;
                    uint64_t claim_state = WRITER_ACTIVE;
                    if (mtx_.pending_writers_ > 0) {
                        claim_state |= WRITER_WAITING;
                    }
                    if (mtx_.state_.compare_exchange_strong(
                            expected, claim_state, std::memory_order_acquire,
                            std::memory_order_relaxed)) {
                        if (!cancellable_ ||
                            detail::claim_wake_state(wake_state_) !=
                                detail::wake_action::rejected) {
                            return false;
                        }
                        release_writer = true;
                    } else {
                        ++mtx_.pending_writers_;
                        if (!cancellable_) {
                            wake_state_->set_handle(awaiter);
                        }
                        suspended_ = true;
                        mtx_.writer_waiters_.push_back(this);
                        if (!cancellable_ ||
                            wake_state_->unblock_after_publish()) {
                            return true;
                        }

                        mtx_.writer_waiters_.remove(this);
                        suspended_ = false;
                        assert(mtx_.pending_writers_ > 0);
                        --mtx_.pending_writers_;
                        reader_to_wake = mtx_.finish_writer_removal_locked(
                            wake_more_readers);
                    }
                }
            }

            if (reader_to_wake) {
                detail::schedule_wake_state(reader_to_wake);
            }
            if (wake_more_readers) {
                mtx_.wake_additional_readers();
            }
            if (release_writer) {
                mtx_.unlock();
            }
            return false;
        }

        coro::cancel_result await_resume_impl() noexcept {
            if (cancellable_ && wake_state_->was_cancelled()) {
                detail::wake_state_ptr reader_to_wake;
                bool wake_more_readers = false;
                if (suspended_) {
                    std::lock_guard<std::mutex> guard(mtx_.internal_mutex_);
                    if (this->is_linked()) {
                        mtx_.writer_waiters_.remove(this);
                        assert(mtx_.pending_writers_ > 0);
                        --mtx_.pending_writers_;
                        reader_to_wake = mtx_.finish_writer_removal_locked(
                            wake_more_readers);
                    }
                    suspended_ = false;
                }
                if (reader_to_wake) {
                    detail::schedule_wake_state(reader_to_wake);
                }
                if (wake_more_readers) {
                    mtx_.wake_additional_readers();
                }
                return coro::cancel_result::cancelled;
            }

            resumed_ = true;
            grant_pending_ = false;
            suspended_ = false;
            return coro::cancel_result::completed;
        }

    protected:
        const detail::wake_state_ptr& cancellation_wake_state() const noexcept {
            return wake_state_;
        }

    private:
        shared_mutex& mtx_;
        detail::wake_state_ptr wake_state_;
        bool cancellable_ = false;
        bool suspended_ = false;
        bool resumed_ = false;
        bool grant_pending_ = false;

        friend class shared_mutex;
    };

    class cancellable_lock_waiter : public lock_waiter {
    public:
        cancellable_lock_waiter(shared_mutex& m, coro::cancel_token token)
            : lock_waiter(m, true) {
            cancel_registration_ = token.on_cancel(
                [state = cancellation_wake_state()] {
                state->request_cancel();
            });
        }

        ~cancellable_lock_waiter() {
            cancel_registration_.unregister();
        }

        coro::cancel_result await_resume_cancellable() noexcept {
            cancel_registration_.unregister();
            return await_resume_impl();
        }

    private:
        coro::cancel_token::registration cancel_registration_;
    };

    class lock_awaitable {
    public:
        explicit lock_awaitable(shared_mutex& m) : waiter_(m) {}

        bool await_ready() const noexcept { return waiter_.await_ready_impl(); }
        bool await_suspend(std::coroutine_handle<> awaiter) noexcept {
            return waiter_.await_suspend_impl(awaiter);
        }
        void await_resume() noexcept {
            (void)waiter_.await_resume_impl();
        }

    private:
        lock_waiter waiter_;
    };

    class cancellable_lock_awaitable {
    public:
        cancellable_lock_awaitable(shared_mutex& m, coro::cancel_token token)
            : waiter_(m, std::move(token)) {}

        bool await_ready() const { return waiter_.await_ready_impl(); }
        bool await_suspend(std::coroutine_handle<> awaiter) {
            return waiter_.await_suspend_impl(awaiter);
        }
        [[nodiscard("check whether the exclusive lock was acquired")]]
        coro::cancel_result await_resume() noexcept {
            return waiter_.await_resume_cancellable();
        }

    private:
        cancellable_lock_waiter waiter_;
    };

    /// Acquire shared (read) lock.
    ///
    /// The application must ensure fewer than (1ULL << 62) - 1 shared
    /// acquisitions are outstanding when this awaitable attempts admission.
    /// An immediately admitted reader does not allocate wake state. A reader
    /// that reaches the writer/saturation slow path allocates before taking
    /// the queue mutex, and await_suspend may propagate std::bad_alloc.
    auto lock_shared() {
        return lock_shared_awaitable(*this);
    }

    /// Acquire a shared lock, or return cancelled if the token wins.
    ///
    /// The application must ensure fewer than (1ULL << 62) - 1 shared
    /// acquisitions are outstanding when this awaitable attempts admission.
    auto lock_shared(coro::cancel_token token) {
        return cancellable_lock_shared_awaitable(*this, std::move(token));
    }

    /// Acquire exclusive (write) lock
    auto lock() {
        return lock_awaitable(*this);
    }

    /// Acquire an exclusive lock, or return cancelled if the token wins.
    auto lock(coro::cancel_token token) {
        return cancellable_lock_awaitable(*this, std::move(token));
    }

    /// Try to acquire shared lock without waiting.
    /// Returns false when the shared-acquisition count cannot be represented.
    /// Lock-free fast path using atomic CAS - no mutex needed in common case
    bool try_lock_shared() noexcept {
        uint64_t state = state_.load(std::memory_order_relaxed);

        // Fast path: if no writer active/waiting, try to increment reader count
        while (!(state & WRITER_FLAGS)) {
            // Overflow guard: if reader count is at max, fail instead of corrupting state
            if ((state & READER_MASK) == READER_MASK) return false;
            if (state_.compare_exchange_weak(state, state + 1,
                    std::memory_order_acquire, std::memory_order_relaxed)) {
                return true;
            }
            // CAS failed, state was updated - loop will re-check
        }
        return false;
    }

    /// Try to acquire exclusive lock without waiting
    bool try_lock() noexcept {
        uint64_t expected = 0;
        return state_.compare_exchange_strong(expected, WRITER_ACTIVE,
            std::memory_order_acquire, std::memory_order_relaxed);
    }

    /// Release shared (read) lock
    ///
    /// No new reader can sneak in via the lock-free fast path between our
    /// fetch_sub and the slow-path mutex acquisition below: WRITER_WAITING
    /// is already set in state_, and try_lock_shared()/lock_shared_awaitable
    /// both check WRITER_FLAGS before the CAS, so they fail immediately.
    /// The writer's own re-acquire CAS (after its fetch_or of WRITER_WAITING)
    /// closes any remaining window where all readers have exited but no one
    /// has woken the writer.
    void unlock_shared() {
        // Decrement reader count atomically
        uint64_t prev_state = state_.fetch_sub(1, std::memory_order_release);
        uint64_t old_readers = prev_state & READER_MASK;

        // Fast path: if we weren't the last reader OR no writer waiting, done
        // Use old_readers != 1 to avoid arithmetic underflow
        if (old_readers != 1 || !(prev_state & WRITER_WAITING)) {
            return;
        }

        // Slow path: transfer ownership to an eligible writer. Cancelled
        // writers are removed without consuming the wakeup; if none remain,
        // clear writer preference and release parked readers.
        detail::wake_state_ptr to_resume;
        bool wake_more_readers = false;
        {
            std::lock_guard<std::mutex> guard(internal_mutex_);

            // Double-check under lock
            uint64_t state = state_.load(std::memory_order_relaxed);
            // A writer may have acquired directly after our last-reader
            // decrement but before we obtained internal_mutex_. In that case
            // ownership is already settled and must not be handed off again.
            if ((state & (READER_MASK | WRITER_ACTIVE)) == 0) {
                if (auto* writer = claim_writer_locked()) {
                    uint64_t new_state = WRITER_ACTIVE;
                    if (pending_writers_ > 0) {
                        new_state |= WRITER_WAITING;
                    }
                    state_.store(new_state, std::memory_order_release);
                    to_resume = writer->wake_state_;
                } else {
                    to_resume = finish_writer_removal_locked(
                        wake_more_readers);
                }
            }
        }

        if (to_resume) {
            detail::schedule_wake_state(to_resume);
        }
        if (wake_more_readers) {
            wake_additional_readers();
        }
    }

    /// Release exclusive (write) lock
    void unlock() {
        detail::wake_state_ptr to_resume;
        bool wake_more_readers = false;

        {
            std::lock_guard<std::mutex> guard(internal_mutex_);

            // Prefer writers over readers to prevent writer starvation
            if (auto* writer = claim_writer_locked()) {
                // Keep WRITER_ACTIVE, update WRITER_WAITING based on remaining writers
                uint64_t new_state = WRITER_ACTIVE;
                if (pending_writers_ > 0) {
                    new_state |= WRITER_WAITING;
                }
                state_.store(new_state, std::memory_order_release);
                to_resume = writer->wake_state_;
            } else {
                auto* reader = claim_reader_locked();
                state_.store(reader ? 1 : 0, std::memory_order_release);
                if (reader) {
                    to_resume = reader->wake_state_;
                    wake_more_readers = !reader_waiters_.empty();
                }
            }
        }

        if (to_resume) {
            detail::schedule_wake_state(to_resume);
        }
        if (wake_more_readers) {
            wake_additional_readers();
        }
    }

    /// Get current reader count
    size_t reader_count() const noexcept {
        return state_.load(std::memory_order_acquire) & READER_MASK;
    }

    /// Check if a writer holds the lock
    bool is_writer_active() const noexcept {
        return (state_.load(std::memory_order_acquire) & WRITER_ACTIVE) != 0;
    }

private:
    lock_shared_waiter* claim_reader_locked() noexcept {
        while (!reader_waiters_.empty()) {
            auto* reader = reader_waiters_.pop_front();
            if (reader->cancellable_ &&
                detail::claim_wake_state(reader->wake_state_) ==
                    detail::wake_action::rejected) {
                continue;
            }
            reader->grant_pending_ = true;
            return reader;
        }
        return nullptr;
    }

    lock_waiter* claim_writer_locked() noexcept {
        while (!writer_waiters_.empty()) {
            auto* writer = writer_waiters_.pop_front();
            assert(pending_writers_ > 0);
            --pending_writers_;
            if (writer->cancellable_ &&
                detail::claim_wake_state(writer->wake_state_) ==
                    detail::wake_action::rejected) {
                continue;
            }
            writer->grant_pending_ = true;
            return writer;
        }
        return nullptr;
    }

    // Clear writer preference after the last pending writer disappears. If
    // no writer is active, reserve one reader slot in the same atomic update
    // that clears WRITER_WAITING so a new writer cannot take the lock between
    // those transitions.
    detail::wake_state_ptr finish_writer_removal_locked(
            bool& wake_more_readers) noexcept {
        wake_more_readers = false;
        if (pending_writers_ != 0) {
            return nullptr;
        }

        auto current = state_.load(std::memory_order_acquire);
        if (current & WRITER_ACTIVE) {
            state_.fetch_and(~WRITER_WAITING, std::memory_order_release);
            return nullptr;
        }

        auto* reader = claim_reader_locked();
        const uint64_t reader_grant = reader ? 1 : 0;
        for (;;) {
            assert((current & READER_MASK) <= READER_MASK - reader_grant);
            const auto desired = (current & ~WRITER_WAITING) + reader_grant;
            if (state_.compare_exchange_weak(
                    current, desired, std::memory_order_acq_rel,
                    std::memory_order_acquire)) {
                break;
            }
            assert(!(current & WRITER_ACTIVE));
        }

        if (!reader) {
            return nullptr;
        }
        wake_more_readers = !reader_waiters_.empty();
        return reader->wake_state_;
    }

    // Add parked readers without allocating a temporary handle array. A
    // reader slot is reserved atomically before its waiter is removed, so a
    // racing try_lock() either sees the reader or wins before queue mutation.
    void wake_additional_readers() noexcept {
        for (;;) {
            detail::wake_state_ptr to_resume;
            {
                std::lock_guard<std::mutex> guard(internal_mutex_);
                if (pending_writers_ != 0 || reader_waiters_.empty()) {
                    return;
                }

                auto current = state_.load(std::memory_order_acquire);
                for (;;) {
                    if ((current & WRITER_FLAGS) ||
                        (current & READER_MASK) == READER_MASK) {
                        return;
                    }
                    if (state_.compare_exchange_weak(
                            current, current + 1, std::memory_order_acq_rel,
                            std::memory_order_acquire)) {
                        break;
                    }
                }

                auto* reader = claim_reader_locked();
                if (!reader) {
                    state_.fetch_sub(1, std::memory_order_release);
                    return;
                }
                to_resume = reader->wake_state_;
            }

            detail::schedule_wake_state(to_resume);
        }
    }

    // state_ is the hot field: read on every lock_shared() fast path,
    // and written on every reader acquire/release.  Keeping it isolated
    // avoids false sharing with the slow-path internal_mutex_.
    alignas(64) std::atomic<uint64_t> state_{0};  // Packed: [writer_waiting:1][writer_active:1][readers:62]

    // slow-path fields: only accessed under internal_mutex_
    alignas(64) mutable std::mutex internal_mutex_;
    size_t pending_writers_ = 0;       // Count of pending writers (for WRITER_WAITING flag management)
    elio::detail::intrusive_list<lock_shared_waiter> reader_waiters_;
    elio::detail::intrusive_list<lock_waiter> writer_waiters_;
};

/// RAII shared lock guard for shared_mutex (reader lock)
class shared_lock_guard {
public:
    explicit shared_lock_guard(shared_mutex& m) : mtx_(&m), owns_lock_(true) {}

    ~shared_lock_guard() {
        if (owns_lock_) {
            mtx_->unlock_shared();
        }
    }

    // Non-copyable, movable
    shared_lock_guard(const shared_lock_guard&) = delete;
    shared_lock_guard& operator=(const shared_lock_guard&) = delete;

    shared_lock_guard(shared_lock_guard&& other) noexcept
        : mtx_(other.mtx_), owns_lock_(other.owns_lock_) {
        other.owns_lock_ = false;
    }

    shared_lock_guard& operator=(shared_lock_guard&& other) noexcept {
        if (this != &other) {
            if (owns_lock_) {
                mtx_->unlock_shared();
            }
            mtx_ = other.mtx_;
            owns_lock_ = other.owns_lock_;
            other.owns_lock_ = false;
        }
        return *this;
    }

    void unlock() {
        if (owns_lock_) {
            mtx_->unlock_shared();
            owns_lock_ = false;
        }
    }

private:
    shared_mutex* mtx_;
    bool owns_lock_;
};

/// RAII unique lock guard for shared_mutex (writer lock)
class unique_lock_guard {
public:
    explicit unique_lock_guard(shared_mutex& m) : mtx_(&m), owns_lock_(true) {}

    ~unique_lock_guard() {
        if (owns_lock_) {
            mtx_->unlock();
        }
    }

    // Non-copyable, movable
    unique_lock_guard(const unique_lock_guard&) = delete;
    unique_lock_guard& operator=(const unique_lock_guard&) = delete;

    unique_lock_guard(unique_lock_guard&& other) noexcept
        : mtx_(other.mtx_), owns_lock_(other.owns_lock_) {
        other.owns_lock_ = false;
    }

    unique_lock_guard& operator=(unique_lock_guard&& other) noexcept {
        if (this != &other) {
            if (owns_lock_) {
                mtx_->unlock();
            }
            mtx_ = other.mtx_;
            owns_lock_ = other.owns_lock_;
            other.owns_lock_ = false;
        }
        return *this;
    }

    void unlock() {
        if (owns_lock_) {
            mtx_->unlock();
            owns_lock_ = false;
        }
    }

private:
    shared_mutex* mtx_;
    bool owns_lock_;
};

} // namespace elio::sync
