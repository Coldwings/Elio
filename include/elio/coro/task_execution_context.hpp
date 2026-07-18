#pragma once

#include <atomic>
#include <cstddef>
#include <limits>

namespace elio::coro {

/// Constant indicating no user affinity (the vthread may migrate freely).
inline constexpr size_t NO_AFFINITY = std::numeric_limits<size_t>::max();

/// Shared runtime policy state for one coroutine task.
///
/// The coroutine promise and external runtime owners keep shared references to
/// this control block. Operation-local completion and cancellation state does
/// not belong here; awaitables retain ownership of those state machines.
class task_execution_context final {
public:
    task_execution_context() noexcept = default;

    task_execution_context(const task_execution_context&) = delete;
    task_execution_context& operator=(const task_execution_context&) = delete;
    task_execution_context(task_execution_context&&) = delete;
    task_execution_context& operator=(task_execution_context&&) = delete;

    [[nodiscard]] size_t user_affinity() const noexcept {
        return user_affinity_.load(std::memory_order_acquire);
    }

    void set_user_affinity(size_t worker_id) noexcept {
        user_affinity_.store(worker_id, std::memory_order_release);
    }

    [[nodiscard]] bool has_user_affinity() const noexcept {
        return user_affinity() != NO_AFFINITY;
    }

    void clear_user_affinity() noexcept {
        set_user_affinity(NO_AFFINITY);
    }

    /// Scheduler placement constraint. Keeping this distinct lets internal
    /// worker-local pins take precedence without rewriting caller affinity.
    [[nodiscard]] size_t effective_affinity() const noexcept {
        return user_affinity();
    }

    void set_worker_local(bool worker_local = true) noexcept {
        worker_local_.store(worker_local, std::memory_order_release);
    }

    [[nodiscard]] bool is_worker_local() const noexcept {
        return worker_local_.load(std::memory_order_acquire);
    }

private:
    std::atomic<size_t> user_affinity_{NO_AFFINITY};
    std::atomic<bool> worker_local_{false};
};

} // namespace elio::coro
