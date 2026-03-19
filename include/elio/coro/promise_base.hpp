#pragma once

#include <cstddef>
#include <exception>
#include <cassert>
#include <atomic>
#include <cstdint>
#include <limits>

namespace elio::coro {

/// Constant indicating no affinity (vthread can migrate freely)
inline constexpr size_t NO_AFFINITY = std::numeric_limits<size_t>::max();

/// Coroutine state for debugging
enum class coroutine_state : uint8_t {
    created = 0,    // Just created, not started
    running = 1,    // Currently executing
    suspended = 2,  // Suspended (awaiting)
    completed = 3,  // Finished execution
    failed = 4      // Threw an exception
};

/// Convert state to string for display
inline const char* state_to_string(coroutine_state state) noexcept {
    switch (state) {
        case coroutine_state::created: return "created";
        case coroutine_state::running: return "running";
        case coroutine_state::suspended: return "suspended";
        case coroutine_state::completed: return "completed";
        case coroutine_state::failed: return "failed";
        default: return "unknown";
    }
}

/// Source location for debugging
struct debug_location {
    const char* file = nullptr;
    const char* function = nullptr;
    uint32_t line = 0;
};

/// Thread-local ID allocator for coroutine debug IDs
/// Allocates IDs in batches to avoid global atomic contention
class id_allocator {
public:
    static constexpr uint64_t BATCH_SIZE = 1024;

    static uint64_t allocate() noexcept {
        auto& alloc = instance();
        if (alloc.next_id_ >= alloc.end_id_) {
            // Batch exhausted - get a new batch
            uint64_t batch_start = global_counter_.fetch_add(BATCH_SIZE, std::memory_order_relaxed);
            alloc.next_id_ = batch_start;
            alloc.end_id_ = batch_start + BATCH_SIZE;
        }
        return alloc.next_id_++;
    }

private:
    id_allocator() noexcept : next_id_(0), end_id_(0) {}

    static id_allocator& instance() noexcept {
        static thread_local id_allocator alloc;
        return alloc;
    }

    uint64_t next_id_;
    uint64_t end_id_;

    static inline std::atomic<uint64_t> global_counter_{1};
};

/// Base class for all coroutine promise types
/// Implements lightweight virtual stack tracking via thread-local intrusive list
///
/// Debug support:
/// - Each frame has a unique ID for identification
/// - Source location can be set for debugging
/// - State tracking (created/running/suspended/completed/failed)
/// - Virtual stack via parent_ pointer chain
///
/// Note: No global frame registry to avoid synchronization overhead.
/// Debuggers should find coroutine frames through scheduler's worker queues.
class promise_base {
public:
    /// Magic number for debugger validation: "ELIOFRME"
    static constexpr uint64_t FRAME_MAGIC = 0x454C494F46524D45ULL;

    promise_base() noexcept
        : frame_magic_(FRAME_MAGIC)
        , parent_(current_frame_)
        , activation_parent_(nullptr)
        , vthread_owner_(current_owner_)
        , debug_state_(coroutine_state::created)
        , debug_worker_id_(static_cast<uint32_t>(-1))
        , debug_id_(0)  // Lazy allocation - only allocated when id() is called
        , affinity_(NO_AFFINITY)
        , frame_size_(consume_next_frame_size())
        , started_(false)
        , vthread_root_(false)
    {
        current_frame_ = this;
    }
    
    ~promise_base() noexcept {
        current_frame_ = parent_;
    }

    promise_base(const promise_base&) = delete;
    promise_base& operator=(const promise_base&) = delete;
    promise_base(promise_base&&) = delete;
    promise_base& operator=(promise_base&&) = delete;

    void unhandled_exception() noexcept {
        exception_ = std::current_exception();
        debug_state_ = coroutine_state::failed;
    }
    
    [[nodiscard]] std::exception_ptr exception() const noexcept {
        return exception_;
    }
    
    [[nodiscard]] promise_base* parent() const noexcept {
        return parent_;
    }

    // Construction-time parent relationship (legacy parent semantics)
    [[nodiscard]] promise_base* construction_parent() const noexcept {
        return parent_;
    }

    // First-activation parent relationship (runtime await-chain semantics)
    [[nodiscard]] promise_base* activation_parent() const noexcept {
        return activation_parent_;
    }

    void set_activation_parent(promise_base* parent) noexcept {
        activation_parent_ = parent;
    }

    bool bind_activation_parent_once(promise_base* parent) noexcept {
        if (activation_parent_ == nullptr) {
            activation_parent_ = parent;
            activation_bindings_.fetch_add(1, std::memory_order_relaxed);
            return true;
        }
        assert(activation_parent_ == parent && "activation_parent rebound inconsistently");
        return false;
    }

    [[nodiscard]] void* vthread_owner() const noexcept {
        return vthread_owner_;
    }

    void set_vthread_owner(void* owner) noexcept {
        vthread_owner_ = owner;
    }

    bool bind_vthread_owner_once(void* owner) noexcept {
        if (vthread_owner_ == nullptr) {
            vthread_owner_ = owner;
            owner_bindings_.fetch_add(1, std::memory_order_relaxed);
            return true;
        }
        // Construction-time owner can differ from first-activation owner.
        // For cold frames (not started yet), allow one-way owner transfer
        // during first activation binding (e.g. task created in A, first
        // awaited in B via spawn/join wrapper).
        if (!started_ && vthread_owner_ != owner) {
            vthread_owner_ = owner;
            owner_bindings_.fetch_add(1, std::memory_order_relaxed);
            return true;
        }
        assert(vthread_owner_ == owner && "vthread_owner rebound inconsistently");
        return false;
    }

    [[nodiscard]] size_t frame_size() const noexcept {
        return frame_size_;
    }

    [[nodiscard]] bool started() const noexcept {
        return started_;
    }

    void mark_started() noexcept {
        started_ = true;
    }

    [[nodiscard]] bool is_vthread_root() const noexcept {
        return vthread_root_;
    }

    void set_vthread_root(bool value) noexcept {
        vthread_root_ = value;
    }

    [[nodiscard]] static promise_base* current_frame() noexcept {
        return current_frame_;
    }

    [[nodiscard]] static void* current_owner() noexcept {
        return current_owner_;
    }

    static void set_current_owner(void* owner) noexcept {
        current_owner_ = owner;
    }

    static void set_next_frame_size(size_t size) noexcept {
        next_frame_size_ = size;
    }

    static void record_root_owner_creation() noexcept {
        root_owner_creations_.fetch_add(1, std::memory_order_relaxed);
    }

    static void record_owner_context_restore() noexcept {
        owner_context_restores_.fetch_add(1, std::memory_order_relaxed);
    }

    static void record_ownerless_resume() noexcept {
        ownerless_resumes_.fetch_add(1, std::memory_order_relaxed);
    }

    [[nodiscard]] static uint64_t owner_bindings() noexcept {
        return owner_bindings_.load(std::memory_order_relaxed);
    }

    [[nodiscard]] static uint64_t activation_bindings() noexcept {
        return activation_bindings_.load(std::memory_order_relaxed);
    }

    [[nodiscard]] static uint64_t root_owner_creations() noexcept {
        return root_owner_creations_.load(std::memory_order_relaxed);
    }

    [[nodiscard]] static uint64_t owner_context_restores() noexcept {
        return owner_context_restores_.load(std::memory_order_relaxed);
    }

    [[nodiscard]] static uint64_t ownerless_resumes() noexcept {
        return ownerless_resumes_.load(std::memory_order_relaxed);
    }

    [[nodiscard]] static promise_base* from_handle_address(void* handle_addr) noexcept {
        if (!handle_addr) return nullptr;

        // GCC/Clang coroutine frame layout:
        // [resume_fn_ptr][destroy_fn_ptr][promise...]
        constexpr size_t promise_offset = 2 * sizeof(void*);
        auto* candidate = reinterpret_cast<promise_base*>(
            static_cast<char*>(handle_addr) + promise_offset);

        return candidate->frame_magic() == FRAME_MAGIC ? candidate : nullptr;
    }

    // Debug accessors
    [[nodiscard]] uint64_t frame_magic() const noexcept { return frame_magic_; }
    [[nodiscard]] const debug_location& location() const noexcept { return debug_location_; }
    [[nodiscard]] coroutine_state state() const noexcept { return debug_state_; }
    [[nodiscard]] uint32_t worker_id() const noexcept { return debug_worker_id_; }
    [[nodiscard]] uint64_t id() noexcept {
        // Lazy allocation - only allocate ID when first requested
        if (debug_id_ == 0) {
            debug_id_ = id_allocator::allocate();
        }
        return debug_id_;
    }

    // Debug setters
    void set_location(const char* file, const char* func, uint32_t line) noexcept {
        debug_location_.file = file;
        debug_location_.function = func;
        debug_location_.line = line;
    }

    void set_state(coroutine_state state) noexcept {
        debug_state_ = state;
    }

    void set_worker_id(uint32_t id) noexcept {
        debug_worker_id_ = id;
    }

    // Affinity accessors
    /// Get the current thread affinity for this vthread
    /// @return Worker ID this vthread is bound to, or NO_AFFINITY if unbound
    [[nodiscard]] size_t affinity() const noexcept { return affinity_; }
    
    /// Set thread affinity for this vthread
    /// @param worker_id Worker ID to bind to, or NO_AFFINITY to clear
    void set_affinity(size_t worker_id) noexcept { affinity_ = worker_id; }
    
    /// Check if this vthread has affinity set
    [[nodiscard]] bool has_affinity() const noexcept { return affinity_ != NO_AFFINITY; }
    
    /// Clear thread affinity, allowing this vthread to migrate freely
    void clear_affinity() noexcept { affinity_ = NO_AFFINITY; }

private:
    static size_t consume_next_frame_size() noexcept {
        size_t size = next_frame_size_;
        next_frame_size_ = 0;
        return size;
    }

    // Magic number at start for debugger validation
    uint64_t frame_magic_;
    
    // Construction-time stack tracking
    promise_base* parent_;
    // Runtime activation relationship
    promise_base* activation_parent_;
    // Runtime vthread ownership context
    void* vthread_owner_;
    std::exception_ptr exception_;
    
    // Debug metadata
    debug_location debug_location_;
    coroutine_state debug_state_;
    uint32_t debug_worker_id_;
    uint64_t debug_id_;
    
    // Thread affinity: NO_AFFINITY means can migrate freely
    size_t affinity_;

    // Frame metadata
    size_t frame_size_;
    bool started_;
    bool vthread_root_;
    
    static inline thread_local promise_base* current_frame_ = nullptr;
    static inline thread_local void* current_owner_ = nullptr;
    static inline thread_local size_t next_frame_size_ = 0;

    static inline std::atomic<uint64_t> owner_bindings_{0};
    static inline std::atomic<uint64_t> activation_bindings_{0};
    static inline std::atomic<uint64_t> root_owner_creations_{0};
    static inline std::atomic<uint64_t> owner_context_restores_{0};
    static inline std::atomic<uint64_t> ownerless_resumes_{0};
};

} // namespace elio::coro
