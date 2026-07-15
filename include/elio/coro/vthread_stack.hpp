#pragma once

#include <cstddef>
#include <cstdint>
#include <limits>
#include <new>

#include <functional>

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

// TSAN annotations for coroutine frame reuse
// These tell TSAN that memory allocated by one coroutine and freed by another
// has a proper happens-before relationship, avoiding false positives.
#if defined(__SANITIZE_THREAD__)
#define ELIO_TSAN_ACTIVE 1
#elif defined(__has_feature)
#if __has_feature(thread_sanitizer)
#define ELIO_TSAN_ACTIVE 1
#endif
#endif

#if ELIO_TSAN_ACTIVE
extern "C" {
void __tsan_acquire(void* addr);
void __tsan_release(void* addr);
}
#define ELIO_TSAN_ACQUIRE(addr) __tsan_acquire(addr)
#define ELIO_TSAN_RELEASE(addr) __tsan_release(addr)
#else
#define ELIO_TSAN_ACQUIRE(addr) ((void)0)
#define ELIO_TSAN_RELEASE(addr) ((void)0)
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
    static inline thread_local vthread_stack* deferred_delete_ = nullptr;

#ifndef ELIO_SANITIZER_ACTIVE
    struct alignas(std::max_align_t) allocation_header {
        uint64_t magic;
        vthread_stack* owner;
        size_t total_size;
    };

    static constexpr uint64_t ALLOCATION_MAGIC = 0x454C494F5653544BULL; // "ELIOVSTK"

    static size_t allocation_size_with_header(size_t size) {
        if (size > std::numeric_limits<size_t>::max() - sizeof(allocation_header)) {
            throw std::bad_alloc();
        }
        return size + sizeof(allocation_header);
    }
#endif

    static void delete_deferred_stack() noexcept;

public:
    // Static interface — for promise_type::operator new/delete
#ifdef ELIO_SANITIZER_ACTIVE
    static void* allocate(size_t size) {
        void* ptr = ::operator new(size);
        ELIO_TSAN_RELEASE(ptr);  // Mark allocation as happening-after any previous deallocation
        return ptr;
    }

    static void deallocate(void* ptr, [[maybe_unused]] size_t size) noexcept {
        ELIO_TSAN_RELEASE(ptr);  // Mark deallocation as happening-before any subsequent allocation
        ::operator delete(ptr);
        delete_deferred_stack();
    }
#else
    static void* allocate(size_t size) {
        const size_t total_size = allocation_size_with_header(size);
        auto* owner = current_;
        allocation_header* header = nullptr;
        if (owner != nullptr) {
            header = static_cast<allocation_header*>(owner->push(total_size));
        } else {
            header = static_cast<allocation_header*>(::operator new(total_size));
        }
        header->magic = ALLOCATION_MAGIC;
        header->owner = owner;
        header->total_size = total_size;
        return header + 1;
    }

    static void deallocate(void* ptr, size_t size) noexcept {
        if (ptr == nullptr) {
            return;
        }

        auto* header = static_cast<allocation_header*>(ptr) - 1;
        if (header->magic != ALLOCATION_MAGIC) [[unlikely]] {
            if (current_ != nullptr) {
                current_->pop(ptr, size);
            }
            return;
        }

        auto* owner = header->owner;
        if (owner == nullptr) {
            header->magic = 0;
            ::operator delete(header);
            delete_deferred_stack();
        } else if (owner == deferred_delete_) {
            header->magic = 0;
            delete_deferred_stack();
        } else if (owner == current_) {
            const size_t total_size = header->total_size;
            header->magic = 0;
            owner->pop(header, total_size);
        } else {
            // The memory belongs to another vthread_stack. Its owner will release
            // the segment storage; do not pop an unrelated current stack.
        }
    }
#endif

    static void delete_after_current_deallocation(vthread_stack* stack) noexcept {
        if (stack == nullptr) {
            return;
        }
        deferred_delete_ = stack;
    }

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
        if (spare_ != nullptr) {
            ::operator delete(spare_);
            spare_ = nullptr;
        }
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

        if (!current_segment_) [[unlikely]] __builtin_trap();
        if (current_segment_->used < aligned_size) [[unlikely]] __builtin_trap();
        if (ptr != current_segment_->data() + current_segment_->used - aligned_size) [[unlikely]] __builtin_trap();

        current_segment_->used -= aligned_size;

        // If current segment is empty and has a previous segment, retire it
        // and backtrack. To avoid heap thrash on workloads where each task fills
        // exactly one segment (e.g. tight spawn loops of short coroutines), we
        // cache a single freed segment in spare_ for reuse by allocate_segment.
        // One slot is sufficient: the spawn-loop pattern only ever needs a
        // segment immediately replaced, never several at once. Holding more
        // would risk pinning peak memory long after a burst.
        if (current_segment_->used == 0 && current_segment_->prev != nullptr) {
            segment* old = current_segment_;
            current_segment_ = current_segment_->prev;
            if (spare_ == nullptr) {
                spare_ = old;
            } else {
                ::operator delete(old);
            }
        }
    }

private:
    static constexpr size_t ALIGNMENT = alignof(std::max_align_t);
    static constexpr size_t DEFAULT_SEGMENT_SIZE = 16384;  // 16KB

    // Ensure segment header size is a multiple of ALIGNMENT so that
    // data() (which is this + 1) is also ALIGNMENT-aligned. Without this,
    // coroutine frames allocated from vthread_stack can have misaligned
    // addresses, causing SIGSEGV from SSE instructions (movaps) when
    // the frame contains alignas(16) or higher objects.
    struct alignas(ALIGNMENT) segment {
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
    static_assert(sizeof(segment) % ALIGNMENT == 0,
                  "sizeof(segment) must be a multiple of ALIGNMENT");

    segment* current_segment_ = nullptr;
    // Cache for one freed segment to skip alloc/free churn on spawn loops.
    // Only populated by pop(); consumed by allocate_segment(). Cleared on dtor.
    segment* spare_ = nullptr;

    static constexpr size_t align_up(size_t n) noexcept {
        return (n + ALIGNMENT - 1) & ~(ALIGNMENT - 1);
    }

    void allocate_segment(size_t min_payload) {
        // Reuse the cached spare if it's large enough. The segment header was
        // allocated with the same ALIGNMENT-aware layout originally, so data()
        // remains correctly aligned (commit afc3ee1 invariant). Resetting used=0
        // brings it back to the same start state as a fresh allocation.
        if (spare_ != nullptr && spare_->capacity >= min_payload) {
            segment* seg = spare_;
            spare_ = nullptr;
            seg->prev = current_segment_;
            seg->used = 0;
            // capacity is preserved; no other bookkeeping to reset
            current_segment_ = seg;
            return;
        }
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

inline void vthread_stack::delete_deferred_stack() noexcept {
    auto* stack = deferred_delete_;
    deferred_delete_ = nullptr;
    delete stack;
}

class vthread_stack_scope {
public:
    explicit vthread_stack_scope(vthread_stack* next) noexcept
        : previous_(vthread_stack::current()) {
        vthread_stack::set_current(next);
    }

    ~vthread_stack_scope() {
        vthread_stack::set_current(previous_);
    }

    vthread_stack_scope(const vthread_stack_scope&) = delete;
    vthread_stack_scope& operator=(const vthread_stack_scope&) = delete;
    vthread_stack_scope(vthread_stack_scope&&) = delete;
    vthread_stack_scope& operator=(vthread_stack_scope&&) = delete;

private:
    vthread_stack* previous_;
};

} // namespace elio::coro
