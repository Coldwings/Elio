#pragma once

#include "io_context_identity.hpp"
#include <elio/coro/task_execution_context.hpp>

#include <memory>
#include <stdexcept>
#include <utility>

namespace elio::io::detail {

/// @internal Operation-local ownership guard for I/O submitted to an
/// io_context.
///
/// Worker-owned contexts pin an Elio coroutine to the context owner until the
/// backend reaches a terminal completion. Foreign coroutine promises and
/// standalone contexts retain context-level active-operation accounting; the
/// owning backend still resumes their operation directly on its worker.
class io_operation_guard final {
public:
    io_operation_guard() noexcept = default;

    io_operation_guard(
        std::shared_ptr<coro::task_execution_context> execution_context,
        std::shared_ptr<io_context_identity> identity)
        : execution_context_(std::move(execution_context))
        , identity_(std::move(identity)) {
        if (!identity_) {
            throw std::invalid_argument(
                "I/O operation guard requires an io_context identity");
        }

        if (identity_->is_worker_owned()) {
            if (execution_context_) {
                execution_context_->acquire_io_pin(
                    identity_->owner_worker_id, identity_->generation);
                task_pinned_ = true;
            }
        }

        identity_->active_pins.fetch_add(1, std::memory_order_acq_rel);
        active_ = true;
    }

    ~io_operation_guard() {
        release();
    }

    io_operation_guard(const io_operation_guard&) = delete;
    io_operation_guard& operator=(const io_operation_guard&) = delete;

    io_operation_guard(io_operation_guard&& other) noexcept {
        move_from(std::move(other));
    }

    io_operation_guard& operator=(io_operation_guard&& other) noexcept {
        if (this != &other) {
            release();
            move_from(std::move(other));
        }
        return *this;
    }

    void release() noexcept {
        if (!active_) return;

        // Keep context-level accounting nonzero until the task pin is gone so
        // a draining worker cannot observe both backend and pin counts at zero
        // during the release transition.
        if (task_pinned_) {
            execution_context_->release_io_pin(
                identity_->owner_worker_id, identity_->generation);
        }
        identity_->active_pins.fetch_sub(1, std::memory_order_acq_rel);

        active_ = false;
        task_pinned_ = false;
        execution_context_.reset();
        identity_.reset();
    }

    [[nodiscard]] bool active() const noexcept {
        return active_;
    }

private:
    void move_from(io_operation_guard&& other) noexcept {
        execution_context_ = std::move(other.execution_context_);
        identity_ = std::move(other.identity_);
        active_ = std::exchange(other.active_, false);
        task_pinned_ = std::exchange(other.task_pinned_, false);
    }

    std::shared_ptr<coro::task_execution_context> execution_context_;
    std::shared_ptr<io_context_identity> identity_;
    bool active_ = false;
    bool task_pinned_ = false;
};

} // namespace elio::io::detail
