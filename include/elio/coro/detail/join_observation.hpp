#pragma once

#include "completion_waiter.hpp"

#include <atomic>
#include <coroutine>
#include <exception>
#include <memory>
#include <optional>
#include <utility>

namespace elio::runtime {
class scheduler;
void schedule_join_observer(scheduler*, std::coroutine_handle<>) noexcept;
}

namespace elio::coro {

enum class join_wait_outcome { completed, timed_out, cancelled };

namespace detail {

struct join_wait_access;

class join_observation final {
public:
    enum class outcome {
        pending, completed, timed_out, cancelled, publishing_failure, failed, abandoned
    };

    explicit join_observation(runtime::scheduler* owner) noexcept
        : waiter_(std::in_place, slot_), owner_(owner) {}

    void notify(outcome value) noexcept {
        auto expected = outcome::pending;
        if (!outcome_.compare_exchange_strong(expected, value,
                std::memory_order_acq_rel, std::memory_order_acquire)) {
            return;
        }
        wake();
    }

    // Reserve the terminal transition before publishing its payload. Setup
    // and timer failures may race, but only one producer writes failure_.
    void fail(std::exception_ptr failure) noexcept {
        auto expected = outcome::pending;
        if (!outcome_.compare_exchange_strong(expected, outcome::publishing_failure,
                std::memory_order_acq_rel, std::memory_order_acquire)) {
            return;
        }
        failure_ = std::move(failure);
        outcome_.store(outcome::failed, std::memory_order_release);
        wake();
    }

    [[nodiscard]] bool ready() const noexcept {
        const auto state = outcome_.load(std::memory_order_acquire);
        return state != outcome::pending && state != outcome::publishing_failure;
    }

    bool register_waiter(std::coroutine_handle<> handle) noexcept {
        return slot_.register_waiter(*waiter_, handle, [this] { return ready(); });
    }

    void abandon() noexcept {
        auto expected = outcome::pending;
        outcome_.compare_exchange_strong(expected, outcome::abandoned,
            std::memory_order_acq_rel, std::memory_order_acquire);
        waiter_.reset();
    }

    join_wait_outcome result() const {
        switch (outcome_.load(std::memory_order_acquire)) {
        case outcome::completed: return join_wait_outcome::completed;
        case outcome::timed_out: return join_wait_outcome::timed_out;
        case outcome::cancelled: return join_wait_outcome::cancelled;
        case outcome::failed: std::rethrow_exception(failure_);
        default: std::terminate();
        }
    }

private:
    void wake() noexcept {
        auto selected = slot_.take();
        auto handle = selected.claim();
        if (handle) runtime::schedule_join_observer(owner_, handle);
    }

    completion_waiter_slot slot_;
    std::optional<completion_waiter> waiter_;
    runtime::scheduler* const owner_;
    std::atomic<outcome> outcome_{outcome::pending};
    std::exception_ptr failure_;
};

struct join_observer_control final {
    std::atomic<std::shared_ptr<join_observation>> observer;
};

class join_observation_awaitable final {
public:
    explicit join_observation_awaitable(std::shared_ptr<join_observation> observation) noexcept
        : observation_(std::move(observation)) {}
    ~join_observation_awaitable() { observation_->abandon(); }
    join_observation_awaitable(const join_observation_awaitable&) = delete;
    join_observation_awaitable& operator=(const join_observation_awaitable&) = delete;

    bool await_ready() const noexcept { return observation_->ready(); }

    bool await_suspend(std::coroutine_handle<> handle) noexcept {
        auto observation = observation_;
        // Publication is the last access to the awaiter. All event sources own
        // this node independently and may resume on another worker immediately.
        return observation->register_waiter(handle);
    }

    join_wait_outcome await_resume() const { return observation_->result(); }

private:
    std::shared_ptr<join_observation> observation_;
};

} // namespace detail
} // namespace elio::coro
