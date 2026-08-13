#pragma once

#include "task_execution_context.hpp"
#include <exception>
#include <atomic>
#include <cstdint>
#include <memory>
#include <type_traits>

namespace elio::runtime {
class scheduler;
}

namespace elio::coro {

namespace detail {
struct defer_nested_task_context_t final {};
inline constexpr defer_nested_task_context_t defer_nested_task_context{};
}  // namespace detail

#ifdef ELIO_RUNTIME_TEST_HOOKS
namespace detail {
inline std::atomic<size_t> promise_constructions_for_test{0};
}  // namespace detail
#endif

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

    promise_base()
        : promise_base(false) {}

protected:
    /// Elio task promises may defer their control block while they are created
    /// inside another Elio frame. The actual first await or runtime handoff,
    /// rather than the creation site, decides their execution context.
    explicit promise_base(detail::defer_nested_task_context_t)
        : promise_base(true) {}

public:
    ~promise_base() noexcept {
        // Invoke spawn-completion callback first. This is the universal
        // -1 for active_tracked_ paired with the +1 the scheduler did at
        // ``go``-time. It runs whether the body completed normally, was
        // cancelled, or the handle was force-destroyed before resuming.
        if (on_spawn_completion_) {
            on_spawn_completion_(on_spawn_completion_state_.get());
        }

        // Only restore current_frame_ if this frame is actually the current
        // frame on this thread.  For detached coroutines destroyed on a
        // foreign thread (e.g., during shutdown drain or failed spawn),
        // unconditionally setting current_frame_ = parent_ (nullptr after
        // detach) would clobber an unrelated active frame chain.
        leave_frame_context();
    }

private:
    explicit promise_base(bool defer_nested_context)
        : frame_magic_(FRAME_MAGIC)
        , parent_(current_frame_)
#if ELIO_ENABLE_DEBUG_METADATA
        , debug_state_(coroutine_state::created)
        , debug_worker_id_(static_cast<uint32_t>(-1))
        , debug_id_(0)  // Lazy allocation - only allocated when id() is called
#endif
        , execution_context_(
              defer_nested_context && parent_
                  ? nullptr
                  : detail::make_task_execution_context())
    {
#ifdef ELIO_RUNTIME_TEST_HOOKS
        detail::promise_constructions_for_test.fetch_add(
            1, std::memory_order_relaxed);
#endif
        current_frame_ = this;
    }

public:

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

    /// Install this frame as a child of the coroutine that is actually
    /// awaiting it. Lazy task creation and ownership order are independent of
    /// the logical execution stack.
    void enter_frame_context(promise_base* parent) noexcept {
        parent_ = parent;
        current_frame_ = this;
    }

    /// Remove the temporary constructor-time linkage of a lazy task. Its
    /// logical parent is unknown until an await operation actually starts it.
    void leave_creation_context() noexcept {
        leave_frame_context();
        parent_ = nullptr;
    }

    /// Restore the logical caller when this frame is current on this thread.
    /// This is also used when a lazy task returns from get_return_object(), so
    /// an unstarted frame never remains installed in creator-thread TLS.
    void leave_frame_context() noexcept {
        if (current_frame_ == this) {
            current_frame_ = parent_;
        }
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

    /// Shared scheduler-visible runtime policy state for this logical vthread.
    /// A nested unstarted task may return null until its first direct await or
    /// independent runtime handoff binds the authoritative context.
    [[nodiscard]] std::shared_ptr<task_execution_context>
    execution_context() const noexcept {
        return execution_context_;
    }

    /// Bind an unstarted transparent child to the context of the Elio promise
    /// that actually awaits it. A task created outside the awaiter may already
    /// have a provisional independent context; no task body has run yet, so
    /// replacing it here is still safe and makes the actual await authoritative.
    void bind_direct_await_context(
        const std::shared_ptr<task_execution_context>& context) noexcept {
        assert(context && "direct Elio await requires an execution context");
        assert(!parent_cancellation_linked_ &&
               "cannot rebind a task after parent cancellation is linked");
        execution_context_ = context;
    }

    /// Mark an explicit structured-cancellation scope. Its direct await keeps
    /// a distinct context so cancelling the scope cannot poison the caller's
    /// logical vthread after the scope has joined.
    void isolate_direct_await_context() noexcept {
        isolate_direct_await_context_ = true;
    }

    [[nodiscard]] bool direct_await_context_isolated() const noexcept {
        return isolate_direct_await_context_;
    }

    /// Bind an isolated structured scope to an Elio caller. Cancellation flows
    /// into the scope, while the distinct context prevents scope cancellation
    /// from becoming a sticky state on the caller.
    void bind_isolated_direct_await_context(promise_base& parent) {
        assert(isolate_direct_await_context_ &&
               "only an isolated task may use an isolated direct await");
        parent.ensure_independent_execution_context();
        ensure_independent_execution_context();
        if (parent.has_affinity()) {
            set_affinity(parent.affinity());
        }
        set_worker_local(parent.is_worker_local());
        link_parent_cancellation(
            parent.execution_context()->get_cancel_token());
        isolated_elio_awaiter_bound_ = true;
    }

    /// Preserve the logical vthread's user affinity when an isolated scope
    /// returns. Runtime-owned I/O pins remain local to the completed scope.
    void propagate_isolated_direct_await_policy_to_parent() noexcept {
        if (!isolated_elio_awaiter_bound_ || !parent_) {
            return;
        }
        if (has_affinity()) {
            parent_->set_affinity(affinity());
        } else {
            parent_->clear_affinity();
        }
    }

    /// Materialize a distinct control block before an independent runtime
    /// handoff or a foreign-promise await boundary.
    void ensure_independent_execution_context() {
        if (!execution_context_) {
            execution_context_ = detail::make_task_execution_context();
        }
    }

    /// Establish one-way cancellation propagation for an independently owned
    /// task root linked by structured runtime policy. Transparent direct Elio
    /// awaits share a context and do not need a callback registration.
    void link_parent_cancellation(cancel_token parent) {
        ensure_independent_execution_context();
        if (parent_cancellation_linked_) {
            throw std::logic_error(
                "task cancellation context already has a parent");
        }
        auto registration =
            execution_context_->make_parent_cancellation_registration(
                std::move(parent));
        parent_cancellation_registration_ = std::move(registration);
        parent_cancellation_linked_ = true;
    }

    /// End independently linked propagation when this task reaches final suspend.
    /// A named task object may retain the completed frame, but that ownership
    /// must not extend the logical parent/child cancellation relationship.
    void unlink_parent_cancellation() noexcept {
        parent_cancellation_registration_.deactivate();
        parent_cancellation_linked_ = false;
    }

    // Affinity accessors
    /// Get the current thread affinity for this vthread
    /// @return Worker ID this vthread is bound to, or NO_AFFINITY if unbound
    [[nodiscard]] size_t affinity() const noexcept {
        return execution_context_->user_affinity();
    }

    /// Get the scheduler placement constraint. This remains distinct from
    /// caller affinity so active I/O pins can take precedence.
    [[nodiscard]] size_t effective_affinity() const noexcept {
        return execution_context_->effective_affinity();
    }

    [[nodiscard]] bool has_active_io_pin() const noexcept {
        return execution_context_->has_active_io_pin();
    }

    [[nodiscard]] size_t io_owner_worker() const noexcept {
        return execution_context_->io_owner_worker();
    }

    [[nodiscard]] uint64_t io_context_generation() const noexcept {
        return execution_context_->io_context_generation();
    }

    [[nodiscard]] size_t active_io_pin_count() const noexcept {
        return execution_context_->active_io_pin_count();
    }

    [[nodiscard]] bool is_io_pin_owner(
        size_t worker_id, uint64_t context_generation) const noexcept {
        return execution_context_->is_io_pin_owner(
            worker_id, context_generation);
    }

    /// Set thread affinity for this vthread
    /// @param worker_id Worker ID to bind to, or NO_AFFINITY to clear
    void set_affinity(size_t worker_id) noexcept {
        execution_context_->set_user_affinity(worker_id);
    }

    /// Check if this vthread has affinity set
    [[nodiscard]] bool has_affinity() const noexcept {
        return execution_context_->has_user_affinity();
    }

    /// Clear caller affinity. Active runtime ownership may still prevent
    /// migration until its operation reaches a terminal state.
    void clear_affinity() noexcept {
        execution_context_->clear_user_affinity();
    }

    /// Mark this coroutine as internal work that must run on its affinity
    /// owner. Used by scheduler maintenance tasks that access worker-local
    /// state such as an io_context.
    void set_worker_local(bool worker_local = true) noexcept {
        execution_context_->set_worker_local(worker_local);
    }

    /// Check whether this coroutine must stay on its affinity owner.
    [[nodiscard]] bool is_worker_local() const noexcept {
        return execution_context_->is_worker_local();
    }

private:
    friend class runtime::scheduler;

    /// Optional callback invoked from the promise destructor to notify a
    /// scheduler that a tracked task is going away. Living in the promise
    /// covers frames destroyed before their coroutine body starts.
    using spawn_completion_fn = void (*)(void*) noexcept;

    void set_spawn_completion(spawn_completion_fn callback,
                              std::shared_ptr<void> state) noexcept {
        on_spawn_completion_state_ = std::move(state);
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

    // Shared runtime policy/control plane. Transparent direct-await frames use
    // the root's state; external runtime owners may retain it after frame
    // destruction. A nested unstarted task may remain null until it is bound.
    std::shared_ptr<task_execution_context> execution_context_;

    // An independently linked task frame owns parent propagation until final
    // suspend. Transparent direct-await frames leave this registration empty.
    detail::task_parent_registration parent_cancellation_registration_;
    bool parent_cancellation_linked_ = false;
    bool isolate_direct_await_context_ = false;
    bool isolated_elio_awaiter_bound_ = false;

    // Keep scheduler accounting after the debugger-visible frame fields so the
    // stable magic/parent prefix remains at the start of promise_base.
    spawn_completion_fn on_spawn_completion_ = nullptr;
    std::shared_ptr<void> on_spawn_completion_state_;

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
