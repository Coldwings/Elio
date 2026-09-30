#pragma once

#include "task.hpp"
#include <elio/runtime/scheduler.hpp>
#include <elio/time/timer.hpp>

#include <atomic>
#include <chrono>
#include <exception>
#include <memory>
#include <optional>
#include <stdexcept>
#include <utility>

namespace elio::coro::detail {

#ifdef ELIO_RUNTIME_TEST_HOOKS
using join_timer_wait_hook = task<cancel_result>(*)(
    std::chrono::steady_clock::time_point, cancel_token);
inline std::atomic<join_timer_wait_hook> join_timer_wait_for_test{nullptr};
inline std::atomic<bool> pause_join_observer_install_for_test{false};
inline std::atomic<bool> join_observer_install_paused_for_test{false};
inline std::atomic<size_t> join_observer_installed_for_test{0};
inline std::atomic<void(*)()> join_timer_admission_for_test{nullptr};
#endif

struct join_wait_access final {
    static task_parent_registration on_cancel(cancel_token token,
            const std::shared_ptr<join_observation>& observation) {
        std::weak_ptr<join_observation> weak = observation;
        return token.on_cancel_for_task([weak]() noexcept {
            if (auto current = weak.lock()) {
                current->notify(join_observation::outcome::cancelled);
            }
        });
    }

    static join_handle<void> launch_timer(runtime::scheduler& scheduler,
            cancel_token stop, task<void> operation,
            const std::shared_ptr<join_observation>& observation) {
        auto handle = task_access::handle(operation);
        handle.promise().ensure_independent_execution_context();
        auto state = std::make_shared<join_state<void>>(
            handle.promise().execution_context());
        handle.promise().join_state_ = state;
        try {
#ifdef ELIO_RUNTIME_TEST_HOOKS
            if (auto hook = join_timer_admission_for_test.load(std::memory_order_acquire)) hook();
#endif
            if (!scheduler.do_go_task_linked_(std::move(stop), std::move(operation))) {
                observation->fail(std::make_exception_ptr(std::logic_error(
                    "scheduler rejected join observer timer")));
                state->set_value();
            }
        } catch (...) {
            observation->fail(std::current_exception());
            state->set_value();
        }
        return join_handle<void>(std::move(state));
    }
};

class join_observer_reservation final {
public:
    explicit join_observer_reservation(std::shared_ptr<join_state_base> state) noexcept
        : state_(std::move(state)) {}
    join_observer_reservation(const join_observer_reservation&) = delete;
    join_observer_reservation& operator=(const join_observer_reservation&) = delete;
    ~join_observer_reservation() {
        if (observation) observation->abandon();
        if (control) {
            auto expected = observation;
            control->observer.compare_exchange_strong(expected, {},
                std::memory_order_acq_rel, std::memory_order_acquire);
        }
        state_->release_result_observer();
    }

    std::shared_ptr<join_observation> observation;
    join_observer_control* control = nullptr;

private:
    std::shared_ptr<join_state_base> state_;
};

class join_timer_stop_guard final {
public:
    explicit join_timer_stop_guard(std::optional<cancel_source>& source) noexcept
        : source_(source) {}
    join_timer_stop_guard(const join_timer_stop_guard&) = delete;
    join_timer_stop_guard& operator=(const join_timer_stop_guard&) = delete;
    ~join_timer_stop_guard() {
        if (source_) {
            try { source_->cancel(); } catch (...) {}
        }
    }
private:
    std::optional<cancel_source>& source_;
};

inline task<void> run_join_observer_timer(std::shared_ptr<join_observation> observation,
        std::chrono::steady_clock::time_point deadline, cancel_token stop) {
    try {
        cancel_result result;
#ifdef ELIO_RUNTIME_TEST_HOOKS
        if (auto hook = join_timer_wait_for_test.load(std::memory_order_acquire)) {
            result = co_await hook(deadline, stop);
        } else
#endif
        {
            result = co_await time::sleep_for(
                deadline - std::chrono::steady_clock::now(), stop);
        }
        if (result == cancel_result::completed) {
            observation->notify(join_observation::outcome::timed_out);
        }
    } catch (...) {
        observation->fail(std::current_exception());
    }
}

inline task<join_wait_outcome> observe_join_result(
        std::shared_ptr<join_state_base> state,
        std::optional<std::chrono::steady_clock::time_point> deadline,
        cancel_token token) {
    if (!state) throw std::invalid_argument("cannot observe an empty join handle");
    if (state->is_completed()) co_return join_wait_outcome::completed;
    if (token.is_cancelled()) co_return join_wait_outcome::cancelled;
    if (deadline && *deadline <= std::chrono::steady_clock::now()) {
        co_return join_wait_outcome::timed_out;
    }
    auto* owner = runtime::scheduler::current();
    auto* worker = runtime::worker_thread::current();
    if (!owner || !worker || !owner->is_running()) {
        throw std::logic_error("pending join observation requires a running scheduler worker");
    }
    if (!state->reserve_result_observer(join_state_base::bounded_observer)) {
        throw std::logic_error("join handle already has a pending result observer");
    }
    join_observer_reservation reservation(state);
    if (state->is_completed()) co_return join_wait_outcome::completed;
    auto observation = std::make_shared<join_observation>(owner);
    reservation.observation = observation;
#ifdef ELIO_RUNTIME_TEST_HOOKS
    if (pause_join_observer_install_for_test.load(std::memory_order_acquire)) {
        join_observer_install_paused_for_test.store(true, std::memory_order_release);
        join_observer_install_paused_for_test.notify_all();
        while (pause_join_observer_install_for_test.load(std::memory_order_acquire)) {
            pause_join_observer_install_for_test.wait(true, std::memory_order_acquire);
        }
        join_observer_install_paused_for_test.store(false, std::memory_order_release);
    }
#endif
    reservation.control = state->install_result_observer(observation);
#ifdef ELIO_RUNTIME_TEST_HOOKS
    join_observer_installed_for_test.fetch_add(1, std::memory_order_release);
#endif
    if (observation->ready()) co_return observation->result();

    task_parent_registration registration;
    std::optional<cancel_source> timer_stop;
    join_timer_stop_guard stop_guard(timer_stop);
    std::optional<join_handle<void>> timer;
    std::optional<join_wait_outcome> result;
    std::exception_ptr failure;
    try {
        registration = join_wait_access::on_cancel(token, observation);
        if (deadline && !observation->ready()) {
            timer_stop.emplace();
            timer.emplace(join_wait_access::launch_timer(*owner,
                timer_stop->get_token(), run_join_observer_timer(
                    observation, *deadline, timer_stop->get_token()), observation));
        }
    } catch (...) {
        observation->fail(std::current_exception());
    }
    try {
        result = co_await join_observation_awaitable(observation);
    } catch (...) {
        failure = std::current_exception();
    }
    observation->abandon();
    registration.unregister();
    if (timer_stop) {
        try { timer_stop->cancel(); }
        catch (...) { if (!failure) failure = std::current_exception(); }
    }
    if (timer) {
        try {
            auto& timer_handle = *timer;
            co_await timer_handle;
        }
        catch (...) { if (!failure) failure = std::current_exception(); }
        try { co_await timer->wait_destroyed_async(); }
        catch (...) { if (!failure) failure = std::current_exception(); }
    }
    if (failure) std::rethrow_exception(failure);
    co_return *result;
}

} // namespace elio::coro::detail

namespace elio::coro {

template<typename T>
task<join_wait_outcome> join_handle<T>::wait(cancel_token token) const {
    return detail::observe_join_result(state_, std::nullopt, std::move(token));
}

template<typename T>
task<join_wait_outcome> join_handle<T>::wait_until(
        std::chrono::steady_clock::time_point deadline, cancel_token token) const {
    return detail::observe_join_result(state_, deadline, std::move(token));
}

inline task<join_wait_outcome> join_handle<void>::wait(cancel_token token) const {
    return detail::observe_join_result(state_, std::nullopt, std::move(token));
}

inline task<join_wait_outcome> join_handle<void>::wait_until(
        std::chrono::steady_clock::time_point deadline, cancel_token token) const {
    return detail::observe_join_result(state_, deadline, std::move(token));
}

} // namespace elio::coro
