#pragma once

#include <exception>
#include <atomic>
#include <cstdint>
#include <limits>
#include <type_traits>

namespace elio::runtime {
class scheduler;
}

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
/// Implements lightweight virtual stack tracking via a thread-local pointer.
///
/// Debug support (when ELIO_ENABLE_DEBUG_METADATA=1):
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
#if ELIO_ENABLE_DEBUG_METADATA
        , debug_state_(coroutine_state::created)
        , debug_worker_id_(static_cast<uint32_t>(-1))
        , debug_id_(0)  // Lazy allocation - only allocated when id() is called
#endif
        , affinity_(NO_AFFINITY)
    {
        current_frame_ = this;
    }

    ~promise_base() noexcept {
        // Invoke spawn-completion callback first. This is the universal
        // -1 for active_tracked_ paired with the +1 the scheduler did at
        // ``go``-time. It runs whether the body completed normally, was
        // cancelled, or the handle was force-destroyed before resuming.
        if (on_spawn_completion_) {
            on_spawn_completion_(on_spawn_completion_data_);
        }

        // Only restore current_frame_ if this frame is actually the current
        // frame on this thread.  For detached coroutines destroyed on a
        // foreign thread (e.g., during shutdown drain or failed spawn),
        // unconditionally setting current_frame_ = parent_ (nullptr after
        // detach) would clobber an unrelated active frame chain.
        if (current_frame_ == this) {
            current_frame_ = parent_;
        }
    }

    /// Detach this frame from the current thread's frame chain.
    /// Call this before spawning a coroutine to another thread to avoid
    /// use-after-free when the original thread creates another coroutine.
    void detach_from_parent() noexcept {
        if (current_frame_ == this) {
            // Set to nullptr instead of parent_ to avoid use-after-free.
            // parent_ may have been spawned to another thread and destroyed.
            current_frame_ = nullptr;
        }
        parent_ = nullptr;
        // Ensure all writes before detach are visible to the thread that will execute this coroutine
        std::atomic_thread_fence(std::memory_order_release);
    }

    promise_base(const promise_base&) = delete;
    promise_base& operator=(const promise_base&) = delete;
    promise_base(promise_base&&) = delete;
    promise_base& operator=(promise_base&&) = delete;

    void unhandled_exception() noexcept {
        exception_ = std::current_exception();
#if ELIO_ENABLE_DEBUG_METADATA
        debug_state_ = coroutine_state::failed;
#endif
    }

    [[nodiscard]] std::exception_ptr exception() const noexcept {
        return exception_;
    }

    [[nodiscard]] promise_base* parent() const noexcept {
        return parent_;
    }

    [[nodiscard]] static promise_base* current_frame() noexcept {
        return current_frame_;
    }

    static void set_current_frame(promise_base* frame) noexcept {
        current_frame_ = frame;
    }

    // Debug accessors (available only when debug metadata is enabled)
#if ELIO_ENABLE_DEBUG_METADATA
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
#else
    // Stub accessors when debug metadata is disabled
    [[nodiscard]] uint64_t frame_magic() const noexcept { return frame_magic_; }
    [[nodiscard]] uint64_t id() noexcept { return 0; }
    [[nodiscard]] uint32_t worker_id() const noexcept { return static_cast<uint32_t>(-1); }
    [[nodiscard]] coroutine_state state() const noexcept { return coroutine_state::running; }
    [[nodiscard]] const debug_location& location() const noexcept {
        static const debug_location empty{};
        return empty;
    }
    void set_location(const char*, const char*, uint32_t) noexcept {}
    void set_state(coroutine_state) noexcept {}
    void set_worker_id(uint32_t) noexcept {}
#endif

    // Affinity accessors
    /// Get the current thread affinity for this vthread
    /// @return Worker ID this vthread is bound to, or NO_AFFINITY if unbound
    [[nodiscard]] size_t affinity() const noexcept { 
        return affinity_.load(std::memory_order_acquire); 
    }

    /// Set thread affinity for this vthread
    /// @param worker_id Worker ID to bind to, or NO_AFFINITY to clear
    void set_affinity(size_t worker_id) noexcept { 
        affinity_.store(worker_id, std::memory_order_release); 
    }

    /// Check if this vthread has affinity set
    [[nodiscard]] bool has_affinity() const noexcept { 
        return affinity_.load(std::memory_order_acquire) != NO_AFFINITY; 
    }

    /// Clear thread affinity, allowing this vthread to migrate freely
    void clear_affinity() noexcept { 
        affinity_.store(NO_AFFINITY, std::memory_order_release); 
    }

    /// Mark this coroutine as internal work that must run on its affinity
    /// owner. Used by scheduler maintenance tasks that access worker-local
    /// state such as an io_context.
    void set_worker_local(bool worker_local = true) noexcept {
        worker_local_.store(worker_local, std::memory_order_release);
    }

    /// Check whether this coroutine must stay on its affinity owner.
    [[nodiscard]] bool is_worker_local() const noexcept {
        return worker_local_.load(std::memory_order_acquire);
    }

private:
    friend class runtime::scheduler;

    /// Optional callback invoked from the promise destructor to notify a
    /// scheduler that a tracked task is going away. Living in the promise
    /// covers frames destroyed before their coroutine body starts.
    using spawn_completion_fn = void (*)(void*) noexcept;

    void set_spawn_completion(spawn_completion_fn callback, void* data) noexcept {
        on_spawn_completion_data_ = data;
        on_spawn_completion_ = callback;
    }

    // Magic number at start for debugger validation
    uint64_t frame_magic_;

    // Virtual stack tracking
    promise_base* parent_;
    std::exception_ptr exception_;

#if ELIO_ENABLE_DEBUG_METADATA
    // Debug metadata (conditionally compiled)
    debug_location debug_location_;
    coroutine_state debug_state_;
    uint32_t debug_worker_id_;
    uint64_t debug_id_;
#endif

    // Thread affinity: NO_AFFINITY means can migrate freely
    // Must be atomic to avoid data races in work-stealing scenarios
    std::atomic<size_t> affinity_;

    // Internal scheduler maintenance tasks may access worker-local state and
    // must not be migrated when their affinity owner is retired.
    std::atomic<bool> worker_local_{false};

    // Keep scheduler accounting after the debugger-visible frame fields so the
    // stable magic/parent prefix remains at the start of promise_base.
    spawn_completion_fn on_spawn_completion_ = nullptr;
    void* on_spawn_completion_data_ = nullptr;

    static inline thread_local promise_base* current_frame_ = nullptr;
};

static_assert(std::is_standard_layout_v<promise_base>,
              "promise_base must retain a debugger-readable member layout");

/// Install a coroutine as the current virtual-stack frame for one resume call.
/// The previous virtual-stack frame is restored when control returns to the
/// resumer.
namespace detail {

class frame_context_scope {
public:
    explicit frame_context_scope(promise_base* frame) noexcept
        : previous_(promise_base::current_frame()) {
        if (frame) {
            promise_base::set_current_frame(frame);
        }
    }

    ~frame_context_scope() {
        promise_base::set_current_frame(previous_);
    }

    frame_context_scope(const frame_context_scope&) = delete;
    frame_context_scope& operator=(const frame_context_scope&) = delete;

private:
    promise_base* previous_;
};

} // namespace detail

} // namespace elio::coro
