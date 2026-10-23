#pragma once

#include "scheduler.hpp"
#include <elio/io/io_operation_guard.hpp>
#include <coroutine>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <type_traits>
#include <utility>

namespace elio::runtime {

class lease_current_worker_awaitable;

/// Retains a worker and the acquiring logical task's exact-owner routing.
/// Release only after external owner-thread obligations have been settled.
/// Token access must be serialized; release may run on a foreign thread.
class external_worker_lease final {
public:
    external_worker_lease() noexcept = default;
    external_worker_lease(const external_worker_lease&) = delete;
    external_worker_lease& operator=(const external_worker_lease&) = delete;

    external_worker_lease(external_worker_lease&& other) noexcept {
        move_from(std::move(other));
    }

    external_worker_lease& operator=(external_worker_lease&& other) noexcept {
        if (this != &other) {
            release();
            move_from(std::move(other));
        }
        return *this;
    }

    ~external_worker_lease() { release(); }

    [[nodiscard]] bool active() const noexcept { return guard_.active(); }
    [[nodiscard]] size_t worker_id() const noexcept { return worker_id_; }
    [[nodiscard]] uint64_t context_generation() const noexcept {
        return generation_;
    }

    void release() noexcept {
        if (!active()) return;
        // Serialize the last decrement/wake with destruction of the context.
        // The shared wake slot survives forced scheduler teardown.
        std::lock_guard<std::mutex> lock(wakeup_->mutex);
        guard_.release();
        if (wakeup_->context) wakeup_->context->notify();
    }

private:
    friend class lease_current_worker_awaitable;

    void move_from(external_worker_lease&& other) noexcept {
        guard_ = std::move(other.guard_);
        wakeup_ = std::move(other.wakeup_);
        worker_id_ = std::exchange(other.worker_id_, coro::NO_AFFINITY);
        generation_ = std::exchange(other.generation_, 0);
    }

    io::detail::io_operation_guard guard_;
    std::shared_ptr<detail::external_worker_wakeup> wakeup_;
    size_t worker_id_ = coro::NO_AFFINITY;
    uint64_t generation_ = 0;
};

class lease_current_worker_awaitable final {
public:
    [[nodiscard]] bool await_ready() const noexcept { return false; }

    template<typename Promise>
        requires std::is_base_of_v<coro::promise_base, Promise>
    bool await_suspend(std::coroutine_handle<Promise> handle) {
        auto* worker = worker_thread::current();
        if (!worker) {
            throw std::logic_error("worker lease requires an Elio worker");
        }
        lease_.guard_ = worker->acquire_external_lease(
            handle.promise().execution_context());
        lease_.wakeup_ = worker->external_wakeup_;
        lease_.worker_id_ = worker->worker_id();
        lease_.generation_ = worker->io_context().generation();
        return false;
    }

    [[nodiscard]] external_worker_lease await_resume() noexcept {
        return std::move(lease_);
    }

private:
    external_worker_lease lease_;
};

[[nodiscard]] inline auto lease_current_worker() noexcept {
    return lease_current_worker_awaitable{};
}

} // namespace elio::runtime
