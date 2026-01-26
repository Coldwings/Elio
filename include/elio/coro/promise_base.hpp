#pragma once

#include <exception>
#include <atomic>
#include <cstdint>

namespace elio::coro {

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
        , debug_state_(coroutine_state::created)
        , debug_worker_id_(static_cast<uint32_t>(-1))
        , debug_id_(next_id_.fetch_add(1, std::memory_order_relaxed))
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

    [[nodiscard]] static promise_base* current_frame() noexcept {
        return current_frame_;
    }

    // Debug accessors
    [[nodiscard]] uint64_t frame_magic() const noexcept { return frame_magic_; }
    [[nodiscard]] const debug_location& location() const noexcept { return debug_location_; }
    [[nodiscard]] coroutine_state state() const noexcept { return debug_state_; }
    [[nodiscard]] uint32_t worker_id() const noexcept { return debug_worker_id_; }
    [[nodiscard]] uint64_t id() const noexcept { return debug_id_; }

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

private:
    // Magic number at start for debugger validation
    uint64_t frame_magic_;
    
    // Virtual stack tracking
    promise_base* parent_;
    std::exception_ptr exception_;
    
    // Debug metadata
    debug_location debug_location_;
    coroutine_state debug_state_;
    uint32_t debug_worker_id_;
    uint64_t debug_id_;
    
    static inline thread_local promise_base* current_frame_ = nullptr;
    static inline std::atomic<uint64_t> next_id_{1};
};

} // namespace elio::coro
