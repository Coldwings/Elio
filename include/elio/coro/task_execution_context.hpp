#pragma once

#include "cancel_token.hpp"
#include <atomic>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <utility>

namespace elio::io::detail {
class io_operation_guard;
}

namespace elio::coro {

class task_execution_context;
class promise_base;

namespace detail {
struct task_execution_control_block;
[[nodiscard]] std::shared_ptr<task_execution_context>
make_task_execution_context();
#ifdef ELIO_RUNTIME_TEST_HOOKS
inline std::atomic<size_t> task_execution_context_allocations_for_test{0};
#endif
}

/// Constant indicating no user affinity. Internal ownership such as an active
/// worker-local I/O pin may still prevent migration.
inline constexpr size_t NO_AFFINITY = std::numeric_limits<size_t>::max();

/// Shared runtime policy state for one logical vthread execution root.
///
/// A root promise, its transparently awaited Elio task frames, and external
/// runtime owners keep shared references to this control block. It records
/// operation-local I/O ownership for scheduler placement and owns vthread-wide
/// cancellation authority. Each pending operation still owns its own completion
/// and cancellation state machine.
class task_execution_context final {
private:
    struct coallocated_state_key final {};

    friend struct detail::task_execution_control_block;
    friend std::shared_ptr<task_execution_context>
    detail::make_task_execution_context();
    friend class promise_base;

public:
    /// Standalone construction preserves source compatibility and allocates a
    /// separate task-local cancellation state.
    task_execution_context() = default;

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
        if (has_active_io_pin()) {
            return io_owner_worker_.load(std::memory_order_acquire);
        }
        return user_affinity();
    }

    [[nodiscard]] bool has_active_io_pin() const noexcept {
        return active_io_pins_.load(std::memory_order_acquire) != 0;
    }

    /// Last observed owner. Meaningful for placement only while
    /// has_active_io_pin() is true.
    [[nodiscard]] size_t io_owner_worker() const noexcept {
        return io_owner_worker_.load(std::memory_order_acquire);
    }

    /// Last observed context generation. Meaningful for placement only while
    /// has_active_io_pin() is true.
    [[nodiscard]] uint64_t io_context_generation() const noexcept {
        return io_context_generation_.load(std::memory_order_acquire);
    }

    [[nodiscard]] size_t active_io_pin_count() const noexcept {
        return active_io_pins_.load(std::memory_order_acquire);
    }

    [[nodiscard]] bool is_io_pin_owner(
        size_t worker_id, uint64_t context_generation) const noexcept {
        return has_active_io_pin() &&
               io_owner_worker() == worker_id &&
               io_context_generation() == context_generation;
    }

    void set_worker_local(bool worker_local = true) noexcept {
        worker_local_.store(worker_local, std::memory_order_release);
    }

    [[nodiscard]] bool is_worker_local() const noexcept {
        return worker_local_.load(std::memory_order_acquire);
    }

    /// Token observed by code running in this logical vthread. Directly awaited
    /// Elio task frames share this state; independent runtime roots do not.
    [[nodiscard]] cancel_token get_cancel_token() const noexcept {
        return cancellation_context_.token();
    }

    /// Request cooperative, best-effort cancellation. Registered callbacks run
    /// synchronously and the first callback exception is rethrown after every
    /// callback not suppressed by same-dispatch reentrant teardown has been
    /// dispatched, matching cancel_source::cancel().
    void request_cancel() {
        cancellation_context_.request_cancel();
    }

    [[nodiscard]] bool is_cancellation_requested() const noexcept {
        return cancellation_context_.is_cancellation_requested();
    }

private:
    friend class elio::io::detail::io_operation_guard;

    explicit task_execution_context(coallocated_state_key) noexcept
        : cancellation_context_(
              detail::cancellation_context::deferred_state_t{}) {}

    [[nodiscard]] detail::task_parent_registration
    make_parent_cancellation_registration(cancel_token parent) {
        return cancellation_context_.link_parent(std::move(parent));
    }

    void acquire_io_pin(size_t worker_id, uint64_t context_generation) {
        std::lock_guard<std::mutex> lock(io_pin_mutex_);
        size_t count = active_io_pins_.load(std::memory_order_relaxed);
        if (count != 0 &&
            (io_owner_worker_.load(std::memory_order_relaxed) != worker_id ||
             io_context_generation_.load(std::memory_order_relaxed) !=
                 context_generation)) {
            throw std::logic_error(
                "one task cannot await I/O from multiple worker contexts");
        }
        if (count == std::numeric_limits<size_t>::max()) {
            throw std::overflow_error("task I/O pin count overflow");
        }
        if (count == 0) {
            io_owner_worker_.store(worker_id, std::memory_order_relaxed);
            io_context_generation_.store(
                context_generation, std::memory_order_relaxed);
        }
        active_io_pins_.store(count + 1, std::memory_order_release);
    }

    void release_io_pin(size_t worker_id,
                        uint64_t context_generation) noexcept {
        std::lock_guard<std::mutex> lock(io_pin_mutex_);
        size_t count = active_io_pins_.load(std::memory_order_relaxed);
        const bool valid =
            count != 0 &&
            io_owner_worker_.load(std::memory_order_relaxed) == worker_id &&
            io_context_generation_.load(std::memory_order_relaxed) ==
                context_generation;
        assert(valid && "I/O operation guard released a mismatched task pin");
        if (!valid) return;

        // Owner and generation deliberately remain as the last observed
        // identity when count reaches zero. The count is authoritative, and
        // retaining immutable snapshot fields avoids torn clear/read windows
        // for lock-free scheduler diagnostics.
        active_io_pins_.store(count - 1, std::memory_order_release);
    }

    std::atomic<size_t> user_affinity_{NO_AFFINITY};
    std::atomic<bool> worker_local_{false};
    detail::cancellation_context cancellation_context_;
    mutable std::mutex io_pin_mutex_;
    std::atomic<size_t> io_owner_worker_{NO_AFFINITY};
    std::atomic<uint64_t> io_context_generation_{0};
    std::atomic<size_t> active_io_pins_{0};
};

namespace detail {

struct task_execution_control_block final {
    task_execution_control_block()
        : context(task_execution_context::coallocated_state_key{}) {}

    cancel_state cancellation_state;
    task_execution_context context;
};

} // namespace detail

inline std::shared_ptr<task_execution_context>
detail::make_task_execution_context() {
    auto control = std::make_shared<detail::task_execution_control_block>();
#ifdef ELIO_RUNTIME_TEST_HOOKS
    task_execution_context_allocations_for_test.fetch_add(
        1, std::memory_order_relaxed);
#endif
    auto context = std::shared_ptr<task_execution_context>(
        control, &control->context);
    auto cancellation_state = std::shared_ptr<detail::cancel_state>(
        control, &control->cancellation_state);
    context->cancellation_context_.bind_shared_state(
        std::move(cancellation_state));
    return context;
}

} // namespace elio::coro
