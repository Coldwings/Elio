#pragma once

#include <cstddef>
#include <cstdlib>
#include <new>
#include <array>
#include <atomic>
#include <cstdint>
#include <mutex>

// Architecture-specific CPU pause/yield hint for tight spin loops.
// Reduces power consumption and allows the HT sibling to run.
#if defined(__x86_64__) || defined(__i386__)
#  define ELIO_CPU_PAUSE() __builtin_ia32_pause()
#elif defined(__aarch64__) || defined(__arm__)
#  define ELIO_CPU_PAUSE() __asm__ __volatile__("yield" ::: "memory")
#else
#  include <thread>
#  define ELIO_CPU_PAUSE() std::this_thread::yield()
#endif

namespace elio::coro {

/// Thread-local free-list based frame allocator for small coroutine frames
/// Dramatically reduces allocation overhead for frequently created/destroyed coroutines
///
/// Design: Each allocated frame has a hidden header storing the source pool ID and size class.
/// When deallocated on a different thread, the frame is returned via an MPSC queue
/// to its source pool. This handles work-stealing scenarios where coroutines
/// are allocated on thread A but deallocated on thread B.
///
/// Size Classes: Multiple pools for different frame sizes (32, 64, 128, 256 bytes)
/// reduce memory waste for small frames while maintaining allocation performance.
///
/// Note: Under sanitizers, pooling is disabled to allow proper leak/error detection.
class frame_allocator {
public:
    // Size classes for different frame sizes
    static constexpr size_t SIZE_CLASSES[] = {32, 64, 128, 256};
    static constexpr size_t NUM_SIZE_CLASSES = 4;
    static constexpr size_t POOL_SIZE = 512;  // Per size class
    static constexpr size_t REMOTE_QUEUE_BATCH = 64;  // Process remote returns in batches

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
        size_t sc = find_size_class(size);
        if (sc < NUM_SIZE_CLASSES) {
            auto& alloc = instance();

            // First try to reclaim remote returns periodically
            alloc.reclaim_remote_returns();

            if (alloc.free_count_[sc] > 0) {
                void* block = alloc.pool_[sc][--alloc.free_count_[sc]];
                // Update header to reflect current pool ownership
                auto* header = static_cast<block_header*>(block);
                header->source_pool_id = alloc.pool_id_;
                header->size_class = static_cast<uint8_t>(sc);
                return block_to_user(block);
            }

            // Allocate new block with header
            void* block = ::operator new(alloc_block_size(sc));
            auto* header = static_cast<block_header*>(block);
            header->source_pool_id = alloc.pool_id_;
            header->size_class = static_cast<uint8_t>(sc);
            header->next.store(nullptr, std::memory_order_relaxed);
            return block_to_user(block);
        }
        // Fall back to standard allocation for large frames
        return ::operator new(size);
    }

    static void deallocate(void* ptr, size_t size) noexcept {
        size_t sc = find_size_class(size);
        if (sc < NUM_SIZE_CLASSES) {
            void* block = user_to_block(ptr);
            auto* header = static_cast<block_header*>(block);
            auto& alloc = instance();

            // Fast path: same thread - return directly to local pool
            if (header->source_pool_id == alloc.pool_id_) {
                if (alloc.free_count_[sc] < POOL_SIZE) {
                    alloc.pool_[sc][alloc.free_count_[sc]++] = block;
                    return;
                }
                // Pool full, delete the block
                ::operator delete(block);
                return;
            } else {
                // Cross-thread deallocation: push to source pool's remote queue
                frame_allocator* source = get_pool_by_id(header->source_pool_id);
                if (source) {
                    source->push_remote_return(block);
                    return;
                }
                // Source pool no longer exists (thread exited), delete the block
                ::operator delete(block);
                return;
            }
        }
        // Large allocation - was allocated without header
        ::operator delete(ptr);
    }
#endif

private:
    // Block header stored before user data
    struct block_header {
        uint32_t source_pool_id;      // ID of the pool that allocated this block
        uint8_t size_class;           // Size class index (0-3)
        std::atomic<block_header*> next;  // For MPSC queue linkage
    };

    // Header size
    static constexpr size_t HEADER_SIZE = sizeof(block_header);

    // Find size class index for requested size
    static size_t find_size_class(size_t size) noexcept {
        for (size_t i = 0; i < NUM_SIZE_CLASSES; ++i) {
            if (size <= SIZE_CLASSES[i]) {
                return i;
            }
        }
        return NUM_SIZE_CLASSES; // Not found (for sizes > 256)
    }

    // Get actual size for a size class
    static size_t size_class_size(size_t idx) noexcept {
        return SIZE_CLASSES[idx];
    }

    // Total block size including header for a given size class
    static size_t alloc_block_size(size_t size_class_idx) noexcept {
        return HEADER_SIZE + SIZE_CLASSES[size_class_idx];
    }

    // Convert between block (with header) and user pointer
    static void* block_to_user(void* block) noexcept {
        return static_cast<char*>(block) + HEADER_SIZE;
    }

    static void* user_to_block(void* user) noexcept {
        return static_cast<char*>(user) - HEADER_SIZE;
    }

    frame_allocator()
        : pool_id_(next_pool_id_.fetch_add(1, std::memory_order_relaxed))
        , remote_head_{}
        , remote_tail_(&remote_head_) {
        // Initialize remote_head_ fields after default construction
        remote_head_.source_pool_id = 0;
        remote_head_.size_class = 0;
        remote_head_.next.store(nullptr, std::memory_order_relaxed);
        // Initialize free counts to 0
        for (size_t i = 0; i < NUM_SIZE_CLASSES; ++i) {
            free_count_[i] = 0;
        }
        // Register this pool for cross-thread access
        register_pool(this);
    }

    ~frame_allocator() {
        // Unregister before cleanup
        unregister_pool(this);

        // Reclaim any remaining remote returns
        reclaim_all_remote_returns();

        // Free all cached frames when thread exits
        for (size_t sc = 0; sc < NUM_SIZE_CLASSES; ++sc) {
            for (size_t i = 0; i < free_count_[sc]; ++i) {
                ::operator delete(pool_[sc][i]);
            }
        }
    }

    // MPSC queue: push from any thread (producers), pop from owner only (consumer)
    void push_remote_return(void* block) noexcept {
        auto* header = static_cast<block_header*>(block);
        header->next.store(nullptr, std::memory_order_relaxed);

        // Atomic push to MPSC queue (lock-free)
        block_header* prev = remote_tail_.exchange(header, std::memory_order_acq_rel);
        prev->next.store(header, std::memory_order_release);
    }

    // Called by owner thread to reclaim remote returns for all size classes
    void reclaim_remote_returns() noexcept {
        // Quick check without full synchronization
        block_header* head = remote_head_.next.load(std::memory_order_acquire);
        if (!head) return;

        size_t count = 0;
        while (head && count < REMOTE_QUEUE_BATCH) {
            block_header* next = head->next.load(std::memory_order_acquire);

            // If next is null but tail points elsewhere, the producer is in the
            // middle of push() (has done the tail exchange but not yet written
            // prev->next).  Spin briefly with a CPU pause hint.
            if (!next && remote_tail_.load(std::memory_order_acquire) != head) {
                for (int i = 0; i < 16; ++i) {
                    ELIO_CPU_PAUSE();
                    next = head->next.load(std::memory_order_acquire);
                    if (next) break;
                }
                // If the link still isn't ready, stop without consuming 'head'.
                // Consuming it would leave the queue in a broken state because
                // the producer would later write through a recycled pointer.
                if (!next) break;
            }

            // Add to appropriate size class pool
            size_t sc = head->size_class;
            if (sc < NUM_SIZE_CLASSES && free_count_[sc] < POOL_SIZE) {
                pool_[sc][free_count_[sc]++] = head;
                remote_head_.next.store(next, std::memory_order_release);
                ++count;
            } else if (sc >= NUM_SIZE_CLASSES) {
                // Invalid size class - delete the block
                ::operator delete(head);
                remote_head_.next.store(next, std::memory_order_release);
            } else {
                // Pool full - leave it in the queue for later
                break;
            }
            head = next;
        }
    }

    // Called during destruction to reclaim all
    void reclaim_all_remote_returns() noexcept {
        block_header* head = remote_head_.next.load(std::memory_order_acquire);
        while (head) {
            block_header* next = head->next.load(std::memory_order_acquire);

            // Same safe spin pattern as reclaim_remote_returns(), but with more
            // retries because we're in teardown and really want to drain the queue.
            if (!next && remote_tail_.load(std::memory_order_acquire) != head) {
                for (int i = 0; i < 32; ++i) {
                    ELIO_CPU_PAUSE();
                    next = head->next.load(std::memory_order_acquire);
                    if (next) break;
                }
                // Stop safely rather than risk corrupting a partially-linked node.
                if (!next) break;
            }

            size_t sc = head->size_class;
            if (sc < NUM_SIZE_CLASSES && free_count_[sc] < POOL_SIZE) {
                pool_[sc][free_count_[sc]++] = head;
            } else {
                ::operator delete(head);
            }
            head = next;
        }
        remote_head_.next.store(nullptr, std::memory_order_release);
        remote_tail_.store(&remote_head_, std::memory_order_release);
    }

    static frame_allocator& instance() {
        static thread_local frame_allocator alloc;
        return alloc;
    }

    // Pool registry for cross-thread access
    static constexpr size_t MAX_POOLS = 256;

    // Registry entries - atomic for lock-free reads, protected by mutex for writes
    static inline std::atomic<frame_allocator*> pool_registry_[MAX_POOLS]{};
    static inline std::mutex registry_mutex_;  // Protects unregister operations

    static void register_pool(frame_allocator* pool) noexcept {
        uint32_t id = pool->pool_id_;
        if (id < MAX_POOLS) {
            pool_registry_[id].store(pool, std::memory_order_release);
        }
    }

    static void unregister_pool(frame_allocator* pool) noexcept {
        uint32_t id = pool->pool_id_;
        if (id < MAX_POOLS) {
            // Use mutex to ensure no concurrent lookups during unregister
            // This prevents the race where a lookup sees a valid pointer
            // but the pool is being destroyed
            std::lock_guard<std::mutex> lock(registry_mutex_);
            pool_registry_[id].store(nullptr, std::memory_order_release);
        }
    }

    // Get pool by ID - returns nullptr if pool was unregistered
    static frame_allocator* get_pool_by_id(uint32_t id) noexcept {
        if (id < MAX_POOLS) {
            return pool_registry_[id].load(std::memory_order_acquire);
        }
        return nullptr;
    }

    std::array<std::array<void*, POOL_SIZE>, NUM_SIZE_CLASSES> pool_;
    std::array<size_t, NUM_SIZE_CLASSES> free_count_;
    uint32_t pool_id_;

    // MPSC queue for remote returns (dummy head node pattern)
    block_header remote_head_;  // Dummy node - next points to actual head
    std::atomic<block_header*> remote_tail_;

    // Global pool ID counter
    static inline std::atomic<uint32_t> next_pool_id_{0};
};

} // namespace elio::coro
