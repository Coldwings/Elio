#pragma once

#include <atomic>
#include <thread>
#include <elio/log/macros.hpp>
#include "../runtime/wait_strategy.hpp"

namespace elio::sync {

/// Thread-blocking spinlock — NOT coroutine-aware.
/// Spins on atomic CAS with cpu_relax(), then yields to the OS scheduler.
/// This blocks the entire worker thread, starving all coroutines on it.
///
/// Use ONLY for very short critical sections (a few instructions) where
/// contention is rare and no co_await occurs while the lock is held.
/// For anything else, use elio::sync::mutex which suspends the coroutine
/// instead of blocking the thread.
/// This spinlock is designed for scenarios where the lock is held very briefly
/// and the overhead of thread/coroutine suspension would exceed the spin time.
class spinlock {
public:
    spinlock() = default;
    ~spinlock() = default;

    // Non-copyable, non-movable
    spinlock(const spinlock&) = delete;
    spinlock& operator=(const spinlock&) = delete;
    spinlock(spinlock&&) = delete;
    spinlock& operator=(spinlock&&) = delete;

    /// Acquire the spinlock (spins until acquired)
    void lock() noexcept {
        // Fast path: try once with CAS
        bool expected = false;
        if (locked_.compare_exchange_weak(expected, true,
                std::memory_order_acquire, std::memory_order_relaxed)) {
            return;
        }

        // Slow path: TTAS (Test-and-Test-and-Set) with backoff
        lock_slow();
    }

    /// Try to acquire without spinning
    bool try_lock() noexcept {
        bool expected = false;
        return locked_.compare_exchange_strong(expected, true,
            std::memory_order_acquire, std::memory_order_relaxed);
    }

    /// Release the spinlock
    void unlock() noexcept {
        locked_.store(false, std::memory_order_release);
    }

    /// Check if locked (for debugging only, not reliable for synchronization)
    bool is_locked() const noexcept {
        return locked_.load(std::memory_order_relaxed);
    }

private:
    void lock_slow() noexcept {
        constexpr int spin_threshold = 8;
        constexpr int warn_threshold = 1024;

        int iterations = 0;

        for (;;) {
            while (locked_.load(std::memory_order_relaxed)) {
                ++iterations;
                if (iterations < spin_threshold) {
                    runtime::cpu_relax();
                } else {
                    std::this_thread::yield();
                }
            }

            bool expected = false;
            if (locked_.compare_exchange_weak(expected, true,
                    std::memory_order_acquire, std::memory_order_relaxed)) {
                if (iterations >= warn_threshold) {
                    ELIO_LOG_DEBUG(
                        "spinlock: high contention ({} iterations), "
                        "consider using elio::sync::mutex instead",
                        iterations);
                }
                return;
            }
        }
    }

    std::atomic<bool> locked_{false};
};

/// RAII lock guard for spinlock
class spinlock_guard {
public:
    explicit spinlock_guard(spinlock& s) : spinlock_(&s), owns_lock_(false) {
        spinlock_->lock();
        owns_lock_ = true;
    }

    ~spinlock_guard() {
        if (owns_lock_) {
            spinlock_->unlock();
        }
    }

    // Non-copyable, movable
    spinlock_guard(const spinlock_guard&) = delete;
    spinlock_guard& operator=(const spinlock_guard&) = delete;

    spinlock_guard(spinlock_guard&& other) noexcept
        : spinlock_(other.spinlock_), owns_lock_(other.owns_lock_) {
        other.owns_lock_ = false;
    }

    spinlock_guard& operator=(spinlock_guard&& other) noexcept {
        if (this != &other) {
            if (owns_lock_) {
                spinlock_->unlock();
            }
            spinlock_ = other.spinlock_;
            owns_lock_ = other.owns_lock_;
            other.owns_lock_ = false;
        }
        return *this;
    }

    void unlock() {
        if (owns_lock_) {
            spinlock_->unlock();
            owns_lock_ = false;
        }
    }

private:
    spinlock* spinlock_;
    bool owns_lock_ = true;
};

} // namespace elio::sync
