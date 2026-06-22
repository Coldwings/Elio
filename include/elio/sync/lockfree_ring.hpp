#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <type_traits>

namespace elio::sync {

/// Lock-free multi-producer multi-consumer ring buffer
/// Uses Vyukov's bounded MPMC queue algorithm with sequence numbers for ABA prevention
template<typename T>
class LockfreeMPMCRing {
    static_assert(std::is_nothrow_move_constructible_v<T>,
                  "T must be nothrow move constructible for lock-free operations");

    struct Slot {
        std::atomic<uint64_t> sequence;
        std::optional<T> value;

        Slot() : sequence(0), value(std::nullopt) {}
    };

    static size_t next_power_of_2(size_t n) {
        if (n == 0) return 1;
        n--;
        n |= n >> 1;
        n |= n >> 2;
        n |= n >> 4;
        n |= n >> 8;
        n |= n >> 16;
        n |= n >> 32;
        return n + 1;
    }

    const size_t capacity_;
    const size_t mask_;
    std::unique_ptr<Slot[]> slots_;

    // Separate cache lines to avoid false sharing
    alignas(64) std::atomic<uint64_t> head_{0};
    alignas(64) std::atomic<uint64_t> tail_{0};

public:
    explicit LockfreeMPMCRing(size_t capacity)
        : capacity_(next_power_of_2(std::max<size_t>(capacity, 2)))
        , mask_(capacity_ - 1)
        , slots_(new Slot[capacity_])
    {
        // Initialize sequence numbers
        for (size_t i = 0; i < capacity_; ++i) {
            slots_[i].sequence.store(i, std::memory_order_relaxed);
        }
        head_.store(0, std::memory_order_relaxed);
        tail_.store(0, std::memory_order_relaxed);
    }

    LockfreeMPMCRing(const LockfreeMPMCRing&) = delete;
    LockfreeMPMCRing& operator=(const LockfreeMPMCRing&) = delete;

    /// Try to push a value into the ring (lvalue reference)
    /// @param value The value to push (only moved on success)
    /// @return true if successful, false if ring is full
    bool try_push(T& value) noexcept {
        uint64_t head = head_.load(std::memory_order_relaxed);

        while (true) {
            Slot& slot = slots_[head & mask_];
            uint64_t seq = slot.sequence.load(std::memory_order_acquire);
            int64_t diff = static_cast<int64_t>(seq) - static_cast<int64_t>(head);

            if (diff == 0) {
                // Slot is available, try to claim it
                if (head_.compare_exchange_weak(head, head + 1,
                                                 std::memory_order_relaxed)) {
                    // Successfully claimed, write value
                    slot.value = std::move(value);
                    // Mark slot as ready for consumption
                    slot.sequence.store(head + 1, std::memory_order_release);
                    return true;
                }
                // CAS failed, head was reloaded, retry
            } else if (diff < 0) {
                // Ring is full
                return false;
            } else {
                // Another producer beat us, reload head and retry
                head = head_.load(std::memory_order_relaxed);
            }
        }
    }

    /// Try to push a value into the ring (rvalue reference overload)
    /// @param value The value to push (rvalue)
    /// @return true if successful, false if ring is full
    /// @note This overload binds rvalue ref but only moves on successful push,
    ///       preserving the moved-from-on-failure fix from the lvalue version.
    bool try_push(T&& value) noexcept {
        return try_push(value);  // Forward to lvalue version
    }

    /// Try to pop a value from the ring
    /// @return the value if successful, std::nullopt if ring is empty
    std::optional<T> try_pop() noexcept {
        uint64_t tail = tail_.load(std::memory_order_relaxed);

        while (true) {
            Slot& slot = slots_[tail & mask_];
            uint64_t seq = slot.sequence.load(std::memory_order_acquire);
            int64_t diff = static_cast<int64_t>(seq) - static_cast<int64_t>(tail + 1);

            if (diff == 0) {
                // Slot has data, try to claim it
                if (tail_.compare_exchange_weak(tail, tail + 1,
                                                 std::memory_order_relaxed)) {
                    // Successfully claimed, read value
                    T value = std::move(*slot.value);
                    slot.value.reset();
                    // Mark slot as available for next round
                    slot.sequence.store(tail + capacity_, std::memory_order_release);
                    return value;
                }
                // CAS failed, tail was reloaded, retry
            } else if (diff < 0) {
                // Ring is empty
                return std::nullopt;
            } else {
                // Another consumer beat us, reload tail and retry
                tail = tail_.load(std::memory_order_relaxed);
            }
        }
    }

    /// Get the capacity of the ring (always a power of 2)
    size_t capacity() const noexcept { return capacity_; }

    /// Check if the ring is empty (approximate, for diagnostics only)
    bool empty() const noexcept {
        uint64_t head = head_.load(std::memory_order_relaxed);
        uint64_t tail = tail_.load(std::memory_order_relaxed);
        return head == tail;
    }

    /// Get approximate size (for diagnostics only, may be inaccurate under contention)
    size_t size() const noexcept {
        uint64_t head = head_.load(std::memory_order_relaxed);
        uint64_t tail = tail_.load(std::memory_order_relaxed);
        return (head >= tail) ? static_cast<size_t>(head - tail) : 0;
    }
};

} // namespace elio::sync
