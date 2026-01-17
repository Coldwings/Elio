#pragma once

#include <cstddef>
#include <cstdlib>
#include <new>
#include <array>
#include <atomic>

namespace elio::coro {

/// Thread-local free-list based frame allocator for small coroutine frames
/// Dramatically reduces allocation overhead for frequently created/destroyed coroutines
/// 
/// Note: Under sanitizers, pooling is disabled to allow proper leak/error detection.
/// This is because coroutines may be allocated on one thread and deallocated on another
/// due to work stealing, which can confuse thread-local pooling.
class frame_allocator {
public:
    // Support frames up to 256 bytes (covers most simple tasks)
    static constexpr size_t MAX_FRAME_SIZE = 256;
    static constexpr size_t POOL_SIZE = 1024;

// Detect sanitizers: GCC uses __SANITIZE_*, Clang uses __has_feature
#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
#define ELIO_SANITIZER_ACTIVE 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer)
#define ELIO_SANITIZER_ACTIVE 1
#endif
#endif

#ifdef ELIO_SANITIZER_ACTIVE
    // Under sanitizers, bypass pooling entirely for accurate leak detection
    static void* allocate(size_t size) {
        return ::operator new(size);
    }

    static void deallocate(void* ptr, [[maybe_unused]] size_t size) noexcept {
        ::operator delete(ptr);
    }
#else
    static void* allocate(size_t size) {
        if (size <= MAX_FRAME_SIZE) {
            auto& alloc = instance();
            if (alloc.free_count_ > 0) {
                return alloc.pool_[--alloc.free_count_];
            }
            // Allocate MAX_FRAME_SIZE so pooled blocks are always large enough
            return ::operator new(MAX_FRAME_SIZE);
        }
        // Fall back to standard allocation for large frames
        return ::operator new(size);
    }

    static void deallocate(void* ptr, size_t size) noexcept {
        if (size <= MAX_FRAME_SIZE) {
            auto& alloc = instance();
            if (alloc.free_count_ < POOL_SIZE) {
                alloc.pool_[alloc.free_count_++] = ptr;
                return;
            }
        }
        ::operator delete(ptr);
    }
#endif

private:
    frame_allocator() : free_count_(0) {}
    
    ~frame_allocator() {
        // Free all cached frames when thread exits
        for (size_t i = 0; i < free_count_; ++i) {
            ::operator delete(pool_[i]);
        }
    }
    
    static frame_allocator& instance() {
        static thread_local frame_allocator alloc;
        return alloc;
    }

    std::array<void*, POOL_SIZE> pool_;
    size_t free_count_;
};

} // namespace elio::coro
