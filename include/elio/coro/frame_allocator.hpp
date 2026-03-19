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
/// Design: Each allocated frame has a hidden header storing the source pool ID.
/// When deallocated on a different thread, the frame is returned via an MPSC queue
/// to its source pool. This handles work-stealing scenarios where coroutines
/// are allocated on thread A but deallocated on thread B.
///
/// Note: Under sanitizers, pooling is disabled to allow proper leak/error detection.
class frame_allocator {
public:
    struct owner_metadata {
        void* owner = nullptr;
        bool is_root = false;
        bool found = false;
    };

    // Support frames up to 256 bytes (covers most simple tasks)
    // Actual allocation includes header, so user-visible size is MAX_FRAME_SIZE
    static constexpr size_t MAX_FRAME_SIZE = 256;
    static constexpr size_t POOL_SIZE = 1024;
    static constexpr size_t REMOTE_QUEUE_BATCH = 64;  // Process remote returns in batches
    static constexpr uint32_t INVALID_POOL_ID = UINT32_MAX;

// Detect sanitizers: GCC uses __SANITIZE_*, Clang uses __has_feature
#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_THREAD__)
#define ELIO_SANITIZER_ACTIVE 1
#elif defined(__has_feature)
#if __has_feature(address_sanitizer) || __has_feature(thread_sanitizer)
#define ELIO_SANITIZER_ACTIVE 1
#endif
#endif

#ifdef ELIO_SANITIZER_ACTIVE
    // Under sanitizers, bypass pooling entirely, but still keep the hidden
    // header so delete-path metadata inspection remains valid.
    static void* allocate(size_t size) {
        void* block = ::operator new(HEADER_SIZE + size);
        auto* header = static_cast<block_header*>(block);
        header->source_pool_id = INVALID_POOL_ID;
        header->next.store(nullptr, std::memory_order_relaxed);
        header->owner = nullptr;
        header->is_root = false;
        return block_to_user(block);
    }

    static void deallocate(void* ptr, [[maybe_unused]] size_t size) noexcept {
        delete_block(user_to_block(ptr));
    }
#else
    static void* allocate(size_t size) {
        if (size <= MAX_FRAME_SIZE) {
            auto& alloc = instance();

            // First try to reclaim remote returns periodically
            alloc.reclaim_remote_returns();

            if (alloc.free_count_ > 0) {
                void* block = alloc.pool_[--alloc.free_count_];
                // Update header to reflect current pool ownership
                // This is important because blocks may have been returned from remote threads
                auto* header = static_cast<block_header*>(block);
                header->source_pool_id = alloc.pool_id_;
                header->owner = nullptr;
                header->is_root = false;
                return block_to_user(block);
            }

            // Allocate new block with header
            void* block = ::operator new(ALLOC_BLOCK_SIZE);
            auto* header = static_cast<block_header*>(block);
            header->source_pool_id = alloc.pool_id_;
            header->next.store(nullptr, std::memory_order_relaxed);
            header->owner = nullptr;
            header->is_root = false;
            return block_to_user(block);
        }
        // Large frames still carry a small header so owner metadata can be
        // attached later without touching promise memory in operator delete.
        void* block = ::operator new(HEADER_SIZE + size);
        auto* header = static_cast<block_header*>(block);
        header->source_pool_id = INVALID_POOL_ID;
        header->next.store(nullptr, std::memory_order_relaxed);
        header->owner = nullptr;
        header->is_root = false;
        return block_to_user(block);
    }

    static void deallocate(void* ptr, size_t size) noexcept {
        if (size <= MAX_FRAME_SIZE) {
            void* block = user_to_block(ptr);
            auto* header = static_cast<block_header*>(block);
            auto& alloc = instance();

            // Fast path: same thread - return directly to local pool
            if (header->source_pool_id == alloc.pool_id_) {
                if (alloc.free_count_ < POOL_SIZE) {
                    alloc.pool_[alloc.free_count_++] = block;
                    return;
                }
                // Pool full, delete the block (not the user pointer!)
                delete_block(block);
                return;
            } else {
                // Cross-thread deallocation: push to source pool's remote queue
                frame_allocator* source = get_pool_by_id(header->source_pool_id);
                if (source) {
                    source->push_remote_return(block);
                    return;
                }
                // Source pool no longer exists (thread exited), delete the block
                delete_block(block);
                return;
            }
        }
        // Large allocation - free the underlying block carrying the header
        delete_block(user_to_block(ptr));
    }
#endif

    static void set_owner_metadata(void* ptr, void* owner, bool is_root) noexcept {
        if (!ptr) return;
        auto* header = static_cast<block_header*>(user_to_block(ptr));
        header->owner = owner;
        header->is_root = is_root;
    }

    [[nodiscard]] static owner_metadata inspect_owner_metadata(void* ptr) noexcept {
        if (!ptr) return {};
        auto* header = static_cast<block_header*>(user_to_block(ptr));
        return owner_metadata{
            .owner = header->owner,
            .is_root = header->is_root,
            .found = header->owner != nullptr,
        };
    }

private:
    static void delete_block(void* block) noexcept {
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wmismatched-new-delete"
#endif
        ::operator delete(block);
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif
    }

    // Block header stored before user data
    struct block_header {
        uint32_t source_pool_id;              // ID of the pool that allocated this block
        std::atomic<block_header*> next;      // For MPSC queue linkage
        void* owner;                          // Owning vthread domain, if attached later
        bool is_root;                         // Root frame responsible for owner lifetime
    };

    // Total block size including header, aligned for user data
    static constexpr size_t HEADER_SIZE = sizeof(block_header);
    static constexpr size_t ALLOC_BLOCK_SIZE = HEADER_SIZE + MAX_FRAME_SIZE;

    // Convert between block (with header) and user pointer
    static void* block_to_user(void* block) noexcept {
        return static_cast<char*>(block) + HEADER_SIZE;
    }

    static void* user_to_block(void* user) noexcept {
        return static_cast<char*>(user) - HEADER_SIZE;
    }

    frame_allocator()
        : free_count_(0)
        , pool_id_(next_pool_id_.fetch_add(1, std::memory_order_relaxed))
        , remote_head_{0, {nullptr}, nullptr, false}  // Dummy head for remote queue
        , remote_tail_(&remote_head_) {
        // Register this pool for cross-thread access
        register_pool(this);
    }

    ~frame_allocator() {
        // Unregister before cleanup
        unregister_pool(this);

        // Reclaim any remaining remote returns
        reclaim_all_remote_returns();

        // Free all cached frames when thread exits
        for (size_t i = 0; i < free_count_; ++i) {
            ::operator delete(pool_[i]);
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

    // Called by owner thread to reclaim remote returns
    void reclaim_remote_returns() noexcept {
        // Quick check without full synchronization
        block_header* head = remote_head_.next.load(std::memory_order_acquire);
        if (!head) return;

        size_t count = 0;
        while (head && count < REMOTE_QUEUE_BATCH && free_count_ < POOL_SIZE) {
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

            pool_[free_count_++] = head;
            remote_head_.next.store(next, std::memory_order_release);
            head = next;
            ++count;
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

            if (free_count_ < POOL_SIZE) {
                pool_[free_count_++] = head;
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

    std::array<void*, POOL_SIZE> pool_;
    size_t free_count_;
    uint32_t pool_id_;

    // MPSC queue for remote returns (dummy head node pattern)
    block_header remote_head_;  // Dummy node - next points to actual head
    std::atomic<block_header*> remote_tail_;

    // Global pool ID counter
    static inline std::atomic<uint32_t> next_pool_id_{0};
};

} // namespace elio::coro
