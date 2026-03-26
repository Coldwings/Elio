#pragma once

#include <cstddef>
#include <cstdint>
#include <new>
#include <cassert>

// Sanitizer detection for vthread_stack
#ifndef ELIO_SANITIZER_ACTIVE
#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
#define ELIO_SANITIZER_ACTIVE 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer)
#define ELIO_SANITIZER_ACTIVE 1
#endif
#endif
#endif

namespace elio::coro {

/// Segmented bump-pointer stack allocator for vthread coroutine frames.
///
/// Each vthread maintains its own stack allocator. Coroutine frames are
/// allocated in LIFO order within stack segments. When a segment is exhausted,
/// a new segment is allocated and linked. When all frames in a segment are
/// freed, the segment is released.
///
/// This allocator provides significant performance improvements over
/// general-purpose allocation for coroutines that follow strict LIFO
/// allocation/deallocation patterns (which is natural for nested coroutines).
class vthread_stack {
public:
    // Static interface — for promise_type::operator new/delete
#ifdef ELIO_SANITIZER_ACTIVE
    static void* allocate(size_t size) {
        return ::operator new(size);
    }

    static void deallocate(void* ptr, [[maybe_unused]] size_t size) noexcept {
        ::operator delete(ptr);
    }
#else
    static void* allocate(size_t size) {
        if (current_ != nullptr) {
            return current_->push(size);
        }
        // No vthread context, use global new directly
        return ::operator new(size);
    }

    static void deallocate(void* ptr, size_t size) noexcept {
        if (current_ != nullptr) {
            current_->pop(ptr, size);
        } else {
            // When current_ is nullptr and we get here via tagged_dealloc() with
            // a vstack tag, it means the vstack that owned this memory has been
            // deleted (its destructor clears current_ and frees all segments).
            // The memory pointed to by ptr is now invalid (already freed by vstack's
            // destructor), so we must NOT try to free it again.
            //
            // This is a no-op: the memory was already freed when the vstack was deleted.
            // Note: calling ::operator delete(ptr) here would be wrong
            // because the memory was already freed by the owning vthread_stack.
            //
            // This situation occurs when:
            // 1. A coroutine owns its vstack (owns_vstack_ = true)
            // 2. The coroutine completes and its promise destructor runs
            // 3. Promise destructor deletes the vstack (freeing all segment memory)
            // 4. Then operator delete calls tagged_dealloc() -> vthread_stack::deallocate()
            // 5. But current_ is now nullptr because the vstack was just deleted
        }
    }
#endif

    // thread-local current vthread_stack management
    static vthread_stack* current() noexcept {
        return current_;
    }

    static void set_current(vthread_stack* s) noexcept {
        current_ = s;
    }

    // Instance lifecycle
    vthread_stack() = default;

    ~vthread_stack() {
        free_segments();
    }

    vthread_stack(const vthread_stack&) = delete;
    vthread_stack& operator=(const vthread_stack&) = delete;

    // Instance allocation interface
    void* push(size_t size) {
        size_t aligned_size = align_up(size);

        // Check if current segment has enough space
        if (current_segment_ == nullptr ||
            current_segment_->used + aligned_size > current_segment_->capacity) {
            allocate_segment(aligned_size);
        }

        void* ptr = current_segment_->data() + current_segment_->used;
        current_segment_->used += aligned_size;
        return ptr;
    }

    void pop([[maybe_unused]] void* ptr, size_t size) noexcept {
        size_t aligned_size = align_up(size);

        assert(current_segment_ != nullptr && "pop called with no segment");
        assert(current_segment_->used >= aligned_size && "pop size exceeds used");
        assert(ptr == current_segment_->data() + current_segment_->used - aligned_size &&
               "pop ptr does not match expected position");

        current_segment_->used -= aligned_size;

        // If current segment is empty and has a previous segment, free current segment and backtrack
        if (current_segment_->used == 0 && current_segment_->prev != nullptr) {
            segment* old = current_segment_;
            current_segment_ = current_segment_->prev;
            ::operator delete(old);
        }
    }

private:
    struct segment {
        segment* prev;
        size_t capacity;
        size_t used;
        
        // Flexible array member workaround: compute data pointer from end of struct
        char* data() noexcept {
            return reinterpret_cast<char*>(this + 1);
        }
        const char* data() const noexcept {
            return reinterpret_cast<const char*>(this + 1);
        }
    };

    segment* current_segment_ = nullptr;
    static constexpr size_t DEFAULT_SEGMENT_SIZE = 16384;  // 16KB
    static constexpr size_t ALIGNMENT = alignof(std::max_align_t);

    static constexpr size_t align_up(size_t n) noexcept {
        return (n + ALIGNMENT - 1) & ~(ALIGNMENT - 1);
    }

    void allocate_segment(size_t min_payload) {
        size_t payload = min_payload > DEFAULT_SEGMENT_SIZE ? min_payload : DEFAULT_SEGMENT_SIZE;
        void* mem = ::operator new(sizeof(segment) + payload);
        segment* seg = static_cast<segment*>(mem);
        seg->prev = current_segment_;
        seg->capacity = payload;
        seg->used = 0;
        current_segment_ = seg;
    }

    void free_segments() {
        while (current_segment_ != nullptr) {
            segment* prev = current_segment_->prev;
            ::operator delete(current_segment_);
            current_segment_ = prev;
        }
    }

    static inline thread_local vthread_stack* current_ = nullptr;
};

} // namespace elio::coro
