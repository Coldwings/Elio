#pragma once

#include <atomic>
#include <cerrno>
#include <climits>
#include <cstdint>

#ifdef __linux__
#include <linux/futex.h>
#include <sys/syscall.h>
#include <unistd.h>
#endif

namespace elio::runtime {

/// Low-overhead cross-thread notification using Linux futex
/// Replaces eventfd+epoll for worker wake-up with lower overhead
class futex_notifier {
    // Cache-line aligned to avoid false sharing
    alignas(64) std::atomic<uint32_t> state_{0};

    static constexpr uint32_t WAITING = 0;
    static constexpr uint32_t NOTIFIED = 1;

#ifdef __linux__
    static long futex_wait(std::atomic<uint32_t>* addr, uint32_t expected,
                           const struct timespec* timeout) noexcept {
        return syscall(SYS_futex, addr, FUTEX_WAIT_PRIVATE, expected, timeout,
                       nullptr, 0);
    }

    static long futex_wake(std::atomic<uint32_t>* addr, int count) noexcept {
        return syscall(SYS_futex, addr, FUTEX_WAKE_PRIVATE, count, nullptr,
                       nullptr, 0);
    }
#endif

public:
    futex_notifier() = default;
    ~futex_notifier() = default;

    futex_notifier(const futex_notifier&) = delete;
    futex_notifier& operator=(const futex_notifier&) = delete;
    futex_notifier(futex_notifier&&) = delete;
    futex_notifier& operator=(futex_notifier&&) = delete;

    /// Wake one waiting thread (if any)
    void notify_one() noexcept {
        uint32_t prev = state_.exchange(NOTIFIED, std::memory_order_release);
        if (prev == WAITING) {
            // Someone might be waiting, wake them
#ifdef __linux__
            futex_wake(&state_, 1);
#endif
        }
    }

    /// Wait indefinitely until notified
    void wait() noexcept {
#ifdef __linux__
        while (state_.load(std::memory_order_acquire) == WAITING) {
            long ret = futex_wait(&state_, WAITING, nullptr);
            if (ret == -1) {
                if (errno == EAGAIN) {
                    // State changed before we could wait - recheck
                    break;
                }
                // EINTR: interrupted, retry
                // Other errors: shouldn't happen, but continue anyway
            }
        }
#else
        // Non-Linux fallback: spin
        while (state_.load(std::memory_order_acquire) == WAITING) {
            std::this_thread::yield();
        }
#endif
    }

    /// Wait with timeout
    /// @param timeout_ms Maximum time to wait in milliseconds
    /// @return true if notified, false if timeout
    bool wait_for(int timeout_ms) noexcept {
        if (state_.load(std::memory_order_acquire) == NOTIFIED) {
            return true;
        }

#ifdef __linux__
        struct timespec ts;
        ts.tv_sec = timeout_ms / 1000;
        ts.tv_nsec = (timeout_ms % 1000) * 1000000L;

        while (state_.load(std::memory_order_acquire) == WAITING) {
            long ret = futex_wait(&state_, WAITING, &ts);
            if (ret == -1) {
                if (errno == ETIMEDOUT) {
                    return false;
                }
                if (errno == EAGAIN) {
                    // State changed - check again
                    break;
                }
                // EINTR: interrupted, but timeout might have passed
                // For simplicity, just return (caller will retry if needed)
                return false;
            }
        }
        return state_.load(std::memory_order_acquire) == NOTIFIED;
#else
        // Non-Linux fallback: just yield once
        std::this_thread::yield();
        return state_.load(std::memory_order_acquire) == NOTIFIED;
#endif
    }

    /// Reset state to waiting (call after wake-up to prepare for next wait)
    void reset() noexcept {
        state_.store(WAITING, std::memory_order_release);
    }

    /// Check if notified without blocking
    [[nodiscard]] bool is_notified() const noexcept {
        return state_.load(std::memory_order_acquire) == NOTIFIED;
    }
};

} // namespace elio::runtime
