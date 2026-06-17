#pragma once

#include <coroutine>
#include <atomic>
#include "../runtime/scheduler.hpp"

namespace elio::sync {

class mutex {
public:
    mutex() = default;
    ~mutex() = default;

    // Non-copyable, non-movable
    mutex(const mutex&) = delete;
    mutex& operator=(const mutex&) = delete;
    mutex(mutex&&) = delete;
    mutex& operator=(mutex&&) = delete;

    /// Lock awaitable - suspends the coroutine until the lock is acquired
    class lock_awaitable {
    public:
        explicit lock_awaitable(mutex& m) : mtx_(m) {}

        bool await_ready() const noexcept {
            return mtx_.try_lock();
        }

        bool await_suspend(std::coroutine_handle<> awaiter) noexcept {
            // Try to acquire the lock
            void* expected = nullptr;
            if (mtx_.state_.compare_exchange_strong(
                    expected, awaiter.address(),
                    std::memory_order_acq_rel, std::memory_order_acquire)) {
                return false; // Acquired, don't suspend
            }

            // Lock is held, add to wait queue
            std::lock_guard<std::mutex> guard(mtx_.internal_mutex_);

            // Double-check after acquiring internal lock
            expected = nullptr;
            if (mtx_.state_.compare_exchange_strong(
                    expected, awaiter.address(),
                    std::memory_order_acq_rel, std::memory_order_acquire)) {
                return false; // Acquired, don't suspend
            }

            // Add to wait queue
            mtx_.waiters_.push(awaiter);
            return true; // Suspend
        }

        void await_resume() const noexcept {}

    private:
        mutex& mtx_;
    };

    /// Unlock awaitable - releases the lock and wakes one waiter if any
    class unlock_awaitable {
    public:
        explicit unlock_awaitable(mutex& m) : mtx_(m) {}

        bool await_ready() const noexcept {
            return true; // Always ready
        }

        bool await_suspend(std::coroutine_handle<>) const noexcept {
            return false; // Never suspend
        }

        void await_resume() const noexcept {
            mtx_.unlock();
        }

    private:
        mutex& mtx_;
    };

    /// Lock the mutex (coroutine-aware)
    [[nodiscard]] lock_awaitable lock() noexcept {
        return lock_awaitable(*this);
    }

    /// Try to lock the mutex (non-blocking)
    bool try_lock() noexcept {
        void* expected = nullptr;
        return state_.compare_exchange_strong(
            expected, reinterpret_cast<void*>(1),
            std::memory_order_acq_rel, std::memory_order_acquire);
    }

    /// Unlock the mutex and wake one waiter if any
    void unlock() noexcept {
        std::coroutine_handle<> waiter_to_wake = nullptr;

        {
            std::lock_guard<std::mutex> guard(internal_mutex_);

            if (waiters_.empty()) {
                // No waiters, just release
                state_.store(nullptr, std::memory_order_release);
            } else {
                // Wake the next waiter — use a sentinel locked marker instead
                // of the waiter's address to avoid dangling pointers if the
                // woken coroutine is destroyed before resuming (e.g., during
                // scheduler shutdown). The waiter knows it owns the lock by
                // virtue of being woken from the queue.
                waiter_to_wake = waiters_.front();
                waiters_.pop();
                state_.store(reinterpret_cast<void*>(1), std::memory_order_release);
            }
        }

        if (waiter_to_wake) {
            runtime::schedule_handle(waiter_to_wake);
        }
    }

    /// Check if the mutex is locked
    bool is_locked() const noexcept {
        return state_.load(std::memory_order_acquire) != nullptr;
    }

private:
    std::atomic<void*> state_{nullptr};
    mutable std::mutex internal_mutex_;
    std::queue<std::coroutine_handle<>> waiters_;
};

/// RAII lock guard for mutex (synchronous)
/// Note: This guard assumes the mutex is already locked when constructed.
/// For coroutine-aware locking, use: co_await m.lock();
class lock_guard {
public:
    explicit lock_guard(mutex& m) : mutex_(&m), owns_lock_(true) {}

    ~lock_guard() {
        if (owns_lock_) {
            mutex_->unlock();
        }
    }

    // Non-copyable, movable
    lock_guard(const lock_guard&) = delete;
    lock_guard& operator=(const lock_guard&) = delete;

    lock_guard(lock_guard&& other) noexcept
        : mutex_(other.mutex_), owns_lock_(other.owns_lock_) {
        other.owns_lock_ = false;
    }

    lock_guard& operator=(lock_guard&& other) noexcept {
        if (this != &other) {
            if (owns_lock_) {
                mutex_->unlock();
            }
            mutex_ = other.mutex_;
            owns_lock_ = other.owns_lock_;
            other.owns_lock_ = false;
        }
        return *this;
    }

    void unlock() {
        if (owns_lock_) {
            mutex_->unlock();
            owns_lock_ = false;
        }
    }

private:
    mutex* mutex_;
    bool owns_lock_;
};

} // namespace elio::sync
