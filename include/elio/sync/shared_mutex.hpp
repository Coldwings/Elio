#pragma once

#include <array>
#include <coroutine>
#include <atomic>
#include <mutex>
#include <vector>
#include "../detail/intrusive_list.hpp"
#include "../runtime/scheduler.hpp"

namespace elio::sync {

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
    /// Shared lock awaitable (for readers)
    /// Optimized with lock-free fast path for uncontended acquisition
    class lock_shared_awaitable : public elio::detail::intrusive_list_node<lock_shared_awaitable> {
    public:
        explicit lock_shared_awaitable(shared_mutex& m) : mtx_(m) {}

        ~lock_shared_awaitable() {
            // Fast path: if we never suspended, we were never enqueued
            if (!suspended_) return;

            // Slow path: acquire internal_mutex_ to prevent race with unlock
            std::lock_guard<std::mutex> guard(mtx_.internal_mutex_);
            if (this->is_linked()) {
                mtx_.reader_waiters_.remove(this);
            }
        }

        bool await_ready() const noexcept {
            return mtx_.try_lock_shared();
        }

        bool await_suspend(std::coroutine_handle<> awaiter) noexcept {
            // Lock-free fast path: attempt to increment reader count without mutex
            // This avoids cache line bouncing on the internal_mutex_ in reader-heavy
            // workloads where writers are rare.
            uint64_t state = mtx_.state_.load(std::memory_order_relaxed);
            while (!(state & WRITER_FLAGS)) {
                // Overflow guard: if reader count is at max, fall through to slow path
                if ((state & READER_MASK) == READER_MASK) break;
                // No writer active or waiting - try CAS to increment reader count
                if (mtx_.state_.compare_exchange_weak(state, state + 1,
                        std::memory_order_acquire, std::memory_order_relaxed)) {
                    return false;  // Acquired lock-free, don't suspend
                }
                // CAS failed, state updated - loop continues with new state
            }

            // Slow path: writer present or CAS failed multiple times
            // Fall back to mutex for proper waiter queue management
            std::lock_guard<std::mutex> guard(mtx_.internal_mutex_);

            // Double-check under lock - state may have changed
            state = mtx_.state_.load(std::memory_order_relaxed);
            if (!(state & WRITER_FLAGS) && (state & READER_MASK) != READER_MASK) {
                // Try one more time under lock for fairness
                if (mtx_.state_.compare_exchange_strong(state, state + 1,
                        std::memory_order_acquire, std::memory_order_relaxed)) {
                    return false;  // Acquired, don't suspend
                }
            }

            // Add to reader wait queue
            handle_ = awaiter;
            suspended_ = true;
            mtx_.reader_waiters_.push_back(this);
            return true;  // Suspend
        }

        void await_resume() const noexcept {}

    private:
        shared_mutex& mtx_;
        std::coroutine_handle<> handle_;
        bool suspended_ = false;

        friend class shared_mutex;
    };

    /// Exclusive lock awaitable (for writers)
    class lock_awaitable : public elio::detail::intrusive_list_node<lock_awaitable> {
    public:
        explicit lock_awaitable(shared_mutex& m) : mtx_(m) {}

        ~lock_awaitable() {
            // Fast path: if we never suspended, we were never enqueued
            if (!suspended_) return;

            // Slow path: acquire internal_mutex_ to prevent race with unlock
            std::lock_guard<std::mutex> guard(mtx_.internal_mutex_);
            if (this->is_linked()) {
                mtx_.writer_waiters_.remove(this);
            }
        }

        bool await_ready() const noexcept {
            return mtx_.try_lock();
        }

        bool await_suspend(std::coroutine_handle<> awaiter) noexcept {
            std::lock_guard<std::mutex> guard(mtx_.internal_mutex_);

            // Try to acquire write lock atomically
            uint64_t expected = 0;
            if (mtx_.state_.compare_exchange_strong(expected, WRITER_ACTIVE,
                    std::memory_order_acquire, std::memory_order_relaxed)) {
                return false;  // Acquired, don't suspend
            }

            // Lock is held — publish WRITER_WAITING so future readers/writers
            // observe contention. A reader's lock-free unlock_shared could have
            // dropped the count to 0 between our failing CAS above and now,
            // leaving the lock effectively free without anyone scheduled to
            // wake us.  Re-attempt the acquire from the WRITER_WAITING state
            // before enqueuing to close that window.
            mtx_.state_.fetch_or(WRITER_WAITING, std::memory_order_acq_rel);

            expected = WRITER_WAITING;
            uint64_t claim_state = WRITER_ACTIVE;
            if (mtx_.pending_writers_ > 0) {
                claim_state |= WRITER_WAITING;
            }
            if (mtx_.state_.compare_exchange_strong(expected, claim_state,
                    std::memory_order_acquire, std::memory_order_relaxed)) {
                return false;  // Acquired — lock was just released
            }

            ++mtx_.pending_writers_;
            handle_ = awaiter;
            suspended_ = true;
            mtx_.writer_waiters_.push_back(this);
            return true;  // Suspend
        }

        void await_resume() const noexcept {}

    private:
        shared_mutex& mtx_;
        std::coroutine_handle<> handle_;
        bool suspended_ = false;

        friend class shared_mutex;
    };

    /// Acquire shared (read) lock
    auto lock_shared() {
        return lock_shared_awaitable(*this);
    }

    /// Acquire exclusive (write) lock
    auto lock() {
        return lock_awaitable(*this);
    }

    /// Try to acquire shared lock without waiting
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

        // Slow path: might need to wake a writer
        std::coroutine_handle<> to_resume;
        {
            std::lock_guard<std::mutex> guard(internal_mutex_);

            // Double-check under lock
            uint64_t state = state_.load(std::memory_order_relaxed);
            if ((state & READER_MASK) == 0 && !writer_waiters_.empty()) {
                auto* writer = writer_waiters_.pop_front();
                --pending_writers_;

                // Clear WRITER_WAITING if no more pending writers, set WRITER_ACTIVE
                uint64_t new_state = WRITER_ACTIVE;
                if (pending_writers_ > 0) {
                    new_state |= WRITER_WAITING;
                }
                state_.store(new_state, std::memory_order_release);
                to_resume = writer->handle_;
            }
        }

        if (to_resume) {
            runtime::schedule_handle(to_resume);
        }
    }

    /// Release exclusive (write) lock
    void unlock() {
        // Stack-allocated small buffer to avoid heap allocation in the common
        // case.  The writer path always resumes exactly one handle; the reader
        // path typically resumes a handful.  Only when reader waiters exceed
        // the inline capacity do we fall back to a heap-allocated vector.
        static constexpr size_t kInlineCapacity = 8;
        std::array<std::coroutine_handle<>, kInlineCapacity> inline_buf;
        size_t inline_count = 0;
        std::vector<std::coroutine_handle<>> overflow;

        {
            std::lock_guard<std::mutex> guard(internal_mutex_);

            // Prefer writers over readers to prevent writer starvation
            if (!writer_waiters_.empty()) {
                auto* writer = writer_waiters_.pop_front();
                --pending_writers_;

                // Keep WRITER_ACTIVE, update WRITER_WAITING based on remaining writers
                uint64_t new_state = WRITER_ACTIVE;
                if (pending_writers_ > 0) {
                    new_state |= WRITER_WAITING;
                }
                state_.store(new_state, std::memory_order_release);

                // Writer path: always exactly one handle — fits in inline buffer
                inline_buf[0] = writer->handle_;
                inline_count = 1;
            } else {
                // Wake all waiting readers
                size_t reader_count = reader_waiters_.size();
                while (!reader_waiters_.empty()) {
                    auto* reader = reader_waiters_.pop_front();
                    if (inline_count < kInlineCapacity) {
                        inline_buf[inline_count++] = reader->handle_;
                    } else {
                        overflow.push_back(reader->handle_);
                    }
                }
                // Set reader count, preserve WRITER_WAITING if there are pending writers
                uint64_t new_state = reader_count;
                if (pending_writers_ > 0) {
                    new_state |= WRITER_WAITING;
                }
                state_.store(new_state, std::memory_order_release);
            }
        }

        for (size_t i = 0; i < inline_count; ++i) {
            runtime::schedule_handle(inline_buf[i]);
        }
        for (auto& h : overflow) {
            runtime::schedule_handle(h);
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
    // state_ is the hot field: read on every lock_shared() fast path,
    // and written on every reader acquire/release.  Keeping it isolated
    // avoids false sharing with the slow-path internal_mutex_.
    alignas(64) std::atomic<uint64_t> state_{0};  // Packed: [writer_waiting:1][writer_active:1][readers:62]

    // slow-path fields: only accessed under internal_mutex_
    alignas(64) mutable std::mutex internal_mutex_;
    size_t pending_writers_ = 0;       // Count of pending writers (for WRITER_WAITING flag management)
    elio::detail::intrusive_list<lock_shared_awaitable> reader_waiters_;
    elio::detail::intrusive_list<lock_awaitable> writer_waiters_;
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
