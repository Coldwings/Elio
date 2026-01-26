#pragma once

#include "scheduler.hpp"
#include "worker_thread.hpp"
#include <elio/coro/promise_base.hpp>
#include <coroutine>

namespace elio::runtime {

/// Get the current worker thread ID
/// @return Worker ID if called from a worker thread, or NO_AFFINITY otherwise
[[nodiscard]] inline size_t current_worker_id() noexcept {
    auto* worker = worker_thread::current();
    return worker ? worker->worker_id() : coro::NO_AFFINITY;
}

/// Awaitable that sets thread affinity for the current vthread
/// 
/// When awaited, this sets the affinity on the current coroutine's promise
/// and optionally migrates the coroutine to the target worker thread.
/// 
/// Usage:
/// @code
/// co_await set_affinity(2);  // Bind to worker 2 and migrate there
/// co_await set_affinity(2, false);  // Bind to worker 2 but don't migrate now
/// @endcode
class set_affinity_awaitable {
public:
    /// Construct with target worker ID
    /// @param worker_id Worker to bind to
    /// @param migrate If true, migrate to target worker immediately
    explicit set_affinity_awaitable(size_t worker_id, bool migrate = true) noexcept
        : worker_id_(worker_id), migrate_(migrate) {}
    
    bool await_ready() const noexcept {
        // Always return false to ensure await_suspend is called to set affinity
        return false;
    }
    
    template<typename Promise>
    bool await_suspend(std::coroutine_handle<Promise> handle) {
        // Set affinity on the promise
        if constexpr (std::is_base_of_v<coro::promise_base, Promise>) {
            handle.promise().set_affinity(worker_id_);
        }
        
        // If not migrating, or already on target worker, don't suspend
        if (!migrate_ || current_worker_id() == worker_id_) {
            return false;
        }
        
        // Schedule on target worker
        auto* sched = scheduler::current();
        if (sched && sched->is_running()) {
            sched->spawn_to(worker_id_, handle);
            return true;  // Suspend and let target worker resume
        }
        
        return false;  // No scheduler, resume immediately
    }
    
    void await_resume() const noexcept {}

private:
    size_t worker_id_;
    bool migrate_;
};

/// Awaitable that clears thread affinity for the current vthread
/// 
/// After awaiting this, the vthread can migrate freely between workers.
/// 
/// Usage:
/// @code
/// co_await clear_affinity();  // Allow migration to any worker
/// @endcode
class clear_affinity_awaitable {
public:
    bool await_ready() const noexcept { return false; }
    
    template<typename Promise>
    bool await_suspend(std::coroutine_handle<Promise> handle) {
        if constexpr (std::is_base_of_v<coro::promise_base, Promise>) {
            handle.promise().clear_affinity();
        }
        return false;  // Resume immediately after clearing
    }
    
    void await_resume() const noexcept {}
};

/// Set thread affinity for the current vthread
/// 
/// Binds the current vthread to a specific worker thread. The vthread
/// will not be stolen by other workers when it has affinity set.
/// 
/// @param worker_id Worker ID to bind to
/// @param migrate If true (default), migrate to target worker immediately
/// @return Awaitable that sets the affinity
/// 
/// Example:
/// @code
/// // Bind to worker 0 and migrate there
/// co_await elio::runtime::set_affinity(0);
/// 
/// // Bind to worker 2 but don't migrate yet
/// co_await elio::runtime::set_affinity(2, false);
/// @endcode
inline auto set_affinity(size_t worker_id, bool migrate = true) {
    return set_affinity_awaitable(worker_id, migrate);
}

/// Clear thread affinity for the current vthread
/// 
/// Removes any affinity binding, allowing the vthread to migrate
/// freely between workers via work stealing.
/// 
/// @return Awaitable that clears the affinity
/// 
/// Example:
/// @code
/// co_await elio::runtime::clear_affinity();
/// @endcode
inline auto clear_affinity() {
    return clear_affinity_awaitable{};
}

/// Bind current vthread to the worker it's currently running on
/// 
/// This is a convenience function that binds to the current worker.
/// Useful when you want to pin a vthread to its current location.
/// 
/// @return Awaitable that binds to current worker
/// 
/// Example:
/// @code
/// co_await elio::runtime::bind_to_current_worker();
/// @endcode
class bind_to_current_worker_awaitable {
public:
    bool await_ready() const noexcept { return false; }
    
    template<typename Promise>
    bool await_suspend(std::coroutine_handle<Promise> handle) {
        if constexpr (std::is_base_of_v<coro::promise_base, Promise>) {
            size_t current = current_worker_id();
            if (current != coro::NO_AFFINITY) {
                handle.promise().set_affinity(current);
            }
        }
        return false;  // Resume immediately after setting
    }
    
    void await_resume() const noexcept {}
};

inline auto bind_to_current_worker() {
    return bind_to_current_worker_awaitable{};
}

} // namespace elio::runtime

namespace elio {

// Convenience aliases in elio namespace
using runtime::set_affinity;
using runtime::clear_affinity;
using runtime::bind_to_current_worker;
using runtime::current_worker_id;

} // namespace elio
