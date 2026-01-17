#pragma once

#include <atomic>
#include <cstddef>
#include <array>

namespace elio::runtime {

/// High-performance bounded MPSC queue using a lock-free ring buffer
/// 
/// - No allocation on push/pop (pre-allocated slots)
/// - Cache-line aligned to prevent false sharing
/// - Uses acquire-release semantics for minimal synchronization overhead
/// 
/// Multiple producers can push concurrently, single consumer pops.
template<typename T, size_t Capacity = 4096>
class mpsc_queue {
    static_assert((Capacity & (Capacity - 1)) == 0, "Capacity must be power of 2");
    static constexpr size_t MASK = Capacity - 1;
    
    struct alignas(64) slot {
        std::atomic<size_t> sequence;
        std::atomic<T*> data;
    };
    
public:
    mpsc_queue() noexcept {
        for (size_t i = 0; i < Capacity; ++i) {
            slots_[i].sequence.store(i, std::memory_order_relaxed);
            slots_[i].data.store(nullptr, std::memory_order_relaxed);
        }
        head_.store(0, std::memory_order_relaxed);
        tail_.store(0, std::memory_order_relaxed);
    }
    
    ~mpsc_queue() = default;
    
    mpsc_queue(const mpsc_queue&) = delete;
    mpsc_queue& operator=(const mpsc_queue&) = delete;
    mpsc_queue(mpsc_queue&&) = delete;
    mpsc_queue& operator=(mpsc_queue&&) = delete;
    
    /// Push an item (multiple producers allowed) - lock-free, wait-free for non-full queue
    bool push(T* item) noexcept {
        size_t pos = tail_.load(std::memory_order_relaxed);
        
        for (;;) {
            slot& s = slots_[pos & MASK];
            size_t seq = s.sequence.load(std::memory_order_acquire);
            intptr_t diff = static_cast<intptr_t>(seq) - static_cast<intptr_t>(pos);
            
            if (diff == 0) {
                // Slot is ready for writing
                if (tail_.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed)) {
                    // Use release to ensure all prior writes (including coroutine frame init)
                    // are visible to the consumer
                    s.data.store(item, std::memory_order_release);
                    s.sequence.store(pos + 1, std::memory_order_release);
                    return true;
                }
            } else if (diff < 0) {
                // Queue is full
                return false;
            } else {
                // Another producer took this slot, retry
                pos = tail_.load(std::memory_order_relaxed);
            }
        }
    }
    
    /// Pop an item (single consumer only) - lock-free
    [[nodiscard]] T* pop() noexcept {
        size_t pos = head_.load(std::memory_order_relaxed);
        slot& s = slots_[pos & MASK];
        size_t seq = s.sequence.load(std::memory_order_acquire);
        intptr_t diff = static_cast<intptr_t>(seq) - static_cast<intptr_t>(pos + 1);
        
        if (diff < 0) {
            // Queue is empty
            return nullptr;
        }
        
        // Use acquire to synchronize with the producer's release on data
        T* item = s.data.load(std::memory_order_acquire);
        s.sequence.store(pos + Capacity, std::memory_order_release);
        head_.store(pos + 1, std::memory_order_relaxed);
        return item;
    }
    
    /// Approximate size of queue (not exact due to concurrent access)
    [[nodiscard]] size_t size_approx() const noexcept {
        size_t head = head_.load(std::memory_order_relaxed);
        size_t tail = tail_.load(std::memory_order_relaxed);
        return tail >= head ? tail - head : 0;
    }
    
    /// Check if queue appears empty
    [[nodiscard]] bool empty() const noexcept {
        size_t pos = head_.load(std::memory_order_relaxed);
        slot& s = const_cast<slot&>(slots_[pos & MASK]);
        size_t seq = s.sequence.load(std::memory_order_acquire);
        return static_cast<intptr_t>(seq) - static_cast<intptr_t>(pos + 1) < 0;
    }
    
    /// Drain up to max_count items, returns number drained
    template<typename Callback>
    size_t drain(Callback&& callback, size_t max_count = SIZE_MAX) noexcept {
        size_t count = 0;
        while (count < max_count) {
            T* item = pop();
            if (!item) break;
            callback(item);
            ++count;
        }
        return count;
    }

private:
    std::array<slot, Capacity> slots_;
    alignas(64) std::atomic<size_t> head_;
    alignas(64) std::atomic<size_t> tail_;
};

} // namespace elio::runtime
