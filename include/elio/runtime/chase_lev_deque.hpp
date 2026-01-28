#pragma once

#include <atomic>
#include <memory>
#include <cstddef>
#include <vector>
#include <mutex>
#include <array>
#include <algorithm>

namespace elio::runtime {

/// Lock-free work-stealing deque (Chase-Lev algorithm)
/// 
/// Owner thread operations (push/pop) are lock-free and very fast.
/// Thief thread operations (steal) are also lock-free but slower.
/// 
/// Owner pushes/pops at the bottom, thieves steal from the top.
/// This creates LIFO order for owner, FIFO order for thieves.
template<typename T>
class chase_lev_deque {
public:
    /// Circular buffer for storing elements
    class circular_buffer {
    public:
        explicit circular_buffer(size_t capacity)
            : capacity_(capacity)
            , mask_(capacity - 1)
            , buffer_(new std::atomic<T*>[capacity]) {
            for (size_t i = 0; i < capacity_; ++i) {
                buffer_[i].store(nullptr, std::memory_order_relaxed);
            }
        }
        
        ~circular_buffer() = default;
        
        circular_buffer(const circular_buffer&) = delete;
        circular_buffer& operator=(const circular_buffer&) = delete;
        circular_buffer(circular_buffer&&) = delete;
        circular_buffer& operator=(circular_buffer&&) = delete;
        
        [[nodiscard]] size_t capacity() const noexcept { return capacity_; }
        
        void store(size_t index, T* item) noexcept {
            buffer_[index & mask_].store(item, std::memory_order_release);
        }
        
        [[nodiscard]] T* load(size_t index) const noexcept {
            return buffer_[index & mask_].load(std::memory_order_acquire);
        }
        
        [[nodiscard]] std::unique_ptr<circular_buffer> grow(size_t top, size_t bottom, size_t new_capacity) const {
            auto new_buf = std::make_unique<circular_buffer>(new_capacity);
            for (size_t i = top; i < bottom; ++i) {
                new_buf->store(i, load(i));
            }
            return new_buf;
        }
        
    private:
        size_t capacity_;
        size_t mask_;
        std::unique_ptr<std::atomic<T*>[]> buffer_;
    };

    explicit chase_lev_deque(size_t initial_capacity = 1024)
        : top_(0), bottom_(0) {
        size_t capacity = 1;
        while (capacity < initial_capacity) {
            capacity <<= 1;
        }
        buffer_.store(new circular_buffer(capacity), std::memory_order_relaxed);
    }

    ~chase_lev_deque() {
        delete buffer_.load(std::memory_order_relaxed);
        std::lock_guard<std::mutex> lock(old_buffers_mutex_);
        for (auto* buf : old_buffers_) {
            delete buf;
        }
    }

    chase_lev_deque(const chase_lev_deque&) = delete;
    chase_lev_deque& operator=(const chase_lev_deque&) = delete;
    chase_lev_deque(chase_lev_deque&&) = delete;
    chase_lev_deque& operator=(chase_lev_deque&&) = delete;

    /// Push an element (owner only) - lock-free
    void push(T* item) noexcept {
        size_t b = bottom_.load(std::memory_order_relaxed);
        size_t t = top_.load(std::memory_order_acquire);
        circular_buffer* buf = buffer_.load(std::memory_order_relaxed);

        if (b - t >= buf->capacity() - 1) {
            buf = resize(buf, t, b);
        }

        buf->store(b, item);
        // Release fence ensures the store to buffer is visible before bottom update
        // This is sufficient - no need for seq_cst here since push doesn't race with pop
        std::atomic_thread_fence(std::memory_order_release);
        bottom_.store(b + 1, std::memory_order_relaxed);
    }

    /// Pop an element (owner only) - lock-free
    [[nodiscard]] T* pop() noexcept {
        size_t b = bottom_.load(std::memory_order_relaxed);
        circular_buffer* buf = buffer_.load(std::memory_order_relaxed);

        if (b == 0) return nullptr;

        b = b - 1;
        // Use relaxed store - the seq_cst fence provides synchronization with steal()
        bottom_.store(b, std::memory_order_relaxed);
        // seq_cst fence is REQUIRED here for correctness with steal()
        // It ensures the bottom store is visible to thieves before we read top,
        // and that we see any concurrent top updates from thieves
        std::atomic_thread_fence(std::memory_order_seq_cst);

        size_t t = top_.load(std::memory_order_relaxed);

        if (t <= b) {
            T* item = buf->load(b);
            if (t == b) {
                // Last element - race with thieves
                // acq_rel is sufficient here: acquire ensures we see the item,
                // release ensures our update to top is visible
                if (!top_.compare_exchange_strong(t, t + 1,
                                                   std::memory_order_acq_rel,
                                                   std::memory_order_relaxed)) {
                    // Lost race to thief
                    bottom_.store(b + 1, std::memory_order_relaxed);
                    return nullptr;
                }
                bottom_.store(b + 1, std::memory_order_relaxed);
            }
            return item;
        }

        // Queue was empty
        bottom_.store(b + 1, std::memory_order_relaxed);
        return nullptr;
    }
    
    /// Pop without seq_cst fence - ONLY use when there are no concurrent stealers
    /// This is safe in single-worker mode where no other thread can steal
    [[nodiscard]] T* pop_local() noexcept {
        size_t b = bottom_.load(std::memory_order_relaxed);
        if (b == 0) return nullptr;
        
        circular_buffer* buf = buffer_.load(std::memory_order_relaxed);
        b = b - 1;
        bottom_.store(b, std::memory_order_relaxed);
        
        size_t t = top_.load(std::memory_order_relaxed);
        if (t <= b) {
            return buf->load(b);
        }
        
        // Queue was empty
        bottom_.store(b + 1, std::memory_order_relaxed);
        return nullptr;
    }

    /// Steal an element (thieves only) - lock-free
    [[nodiscard]] T* steal() noexcept {
        size_t t = top_.load(std::memory_order_acquire);
        // seq_cst fence is REQUIRED here for correctness with pop()
        // It ensures we see the latest bottom value after any concurrent pop
        std::atomic_thread_fence(std::memory_order_seq_cst);
        size_t b = bottom_.load(std::memory_order_acquire);

        if (t < b) {
            circular_buffer* buf = buffer_.load(std::memory_order_acquire);
            T* item = buf->load(t);
            // acq_rel CAS: acquire ensures we see the item data,
            // release ensures our top update is visible to owner's pop
            if (top_.compare_exchange_strong(t, t + 1,
                                              std::memory_order_acq_rel,
                                              std::memory_order_relaxed)) {
                return item;
            }
        }
        return nullptr;
    }

    /// Steal multiple elements at once (thieves only) - lock-free
    /// WARNING: This function has a race condition with pop() when the owner
    /// pops items between when we read bottom and when we load items.
    /// Use steal() for single-item stealing which is properly synchronized.
    /// This function is kept for reference but should not be used.
    template<size_t N>
    [[deprecated("Use steal() instead - steal_batch has race conditions with pop()")]]
    size_t steal_batch(std::array<T*, N>& output) noexcept {
        size_t stolen = 0;
        
        while (stolen < N) {
            // Single-item steal to avoid race with owner's pop
            T* item = steal();
            if (!item) break;
            output[stolen++] = item;
        }
        
        return stolen;
    }

    [[nodiscard]] size_t size() const noexcept {
        size_t b = bottom_.load(std::memory_order_relaxed);
        size_t t = top_.load(std::memory_order_relaxed);
        return (b >= t) ? (b - t) : 0;
    }

    [[nodiscard]] bool empty() const noexcept {
        return size() == 0;
    }

private:
    circular_buffer* resize(circular_buffer* old_buf, size_t top, size_t bottom) noexcept {
        auto new_buf = old_buf->grow(top, bottom, old_buf->capacity() * 2);
        circular_buffer* raw_ptr = new_buf.release();
        buffer_.store(raw_ptr, std::memory_order_release);
        
        {
            std::lock_guard<std::mutex> lock(old_buffers_mutex_);
            old_buffers_.push_back(old_buf);
        }
        
        return raw_ptr;
    }

    alignas(64) std::atomic<size_t> top_;
    alignas(64) std::atomic<size_t> bottom_;
    alignas(64) std::atomic<circular_buffer*> buffer_;
    
    mutable std::mutex old_buffers_mutex_;
    std::vector<circular_buffer*> old_buffers_;
};

} // namespace elio::runtime
