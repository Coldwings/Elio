#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <elio/coro/join_wait.hpp>
#include <elio/sync/event.hpp>
#include "../test_main.cpp"

#include <atomic>
#include <chrono>
#include <exception>
#include <memory>
#include <optional>
#include <stdexcept>
#include <thread>
#include <utility>

namespace {
using elio::coro::join_handle;
using elio::coro::join_wait_outcome;
using elio::coro::task;
using elio::coro::detail::join_state;
using elio::runtime::scheduler;

template<typename Predicate>
bool wait_for(Predicate predicate) {
    const auto deadline = std::chrono::steady_clock::now() + elio::test::scaled_sec(5);
    while (!predicate()) {
        if (std::chrono::steady_clock::now() >= deadline) return false;
        std::this_thread::yield();
    }
    return true;
}

struct timer_control {
    timer_control() { settle.set(); }
    elio::sync::event expire;
    elio::sync::event settle;
    std::atomic<bool> entered{false};
    std::atomic<bool> cancelled{false};
    std::atomic<bool> settled{false};
    bool throw_on_entry = false;
};

std::atomic<timer_control*> active_timer{nullptr};

task<elio::coro::cancel_result> controlled_timer(
        std::chrono::steady_clock::time_point, elio::coro::cancel_token stop) {
    auto* control = active_timer.load(std::memory_order_acquire);
    if (!control) throw std::logic_error("missing timer control");
    control->entered.store(true, std::memory_order_release);
    if (control->throw_on_entry) throw std::runtime_error("controlled timer failure");
    const auto result = co_await control->expire.wait(stop);
    control->cancelled.store(result == elio::coro::cancel_result::cancelled,
                             std::memory_order_release);
    co_await control->settle.wait();
    control->settled.store(true, std::memory_order_release);
    co_return result;
}

struct timer_hook_guard {
    explicit timer_hook_guard(timer_control& control) {
        active_timer.store(&control, std::memory_order_release);
        elio::coro::detail::join_timer_wait_for_test.store(controlled_timer,
                                                        std::memory_order_release);
    }
    ~timer_hook_guard() {
        elio::coro::detail::join_timer_wait_for_test.store(nullptr,
                                                        std::memory_order_release);
        active_timer.store(nullptr, std::memory_order_release);
    }
};

struct destruction_setup_control {
    std::atomic<bool> entered{false};
    bool fail_allocation = false;
};
std::atomic<destruction_setup_control*> active_destruction_setup{nullptr};

void controlled_destruction_setup() {
    auto* control = active_destruction_setup.load(std::memory_order_acquire);
    control->entered.store(true, std::memory_order_release);
    if (control->fail_allocation) throw std::bad_alloc();
}

struct destruction_setup_hook_guard {
    explicit destruction_setup_hook_guard(destruction_setup_control& control) {
        active_destruction_setup.store(&control, std::memory_order_release);
        elio::coro::detail::join_destroyed_observer_setup_for_test.store(
            controlled_destruction_setup, std::memory_order_release);
    }
    ~destruction_setup_hook_guard() {
        elio::coro::detail::join_destroyed_observer_setup_for_test.store(
            nullptr, std::memory_order_release);
        active_destruction_setup.store(nullptr, std::memory_order_release);
    }
};

struct release_guard {
    timer_control& control;
    ~release_guard() {
        control.expire.set();
        control.settle.set();
        auto& pause = elio::coro::detail::pause_join_observer_install_for_test;
        pause.store(false, std::memory_order_release);
        pause.notify_all();
    }
};

template<typename T>
T run_immediately(task<T> operation) {
    auto handle = elio::coro::detail::task_access::handle(operation);
    handle.resume();
    REQUIRE(handle.done());
    return operation.await_resume();
}

struct observation_result {
    std::optional<join_wait_outcome> value;
    std::exception_ptr failure;
    std::atomic<bool> done{false};
    std::atomic<unsigned> returns{0};
    scheduler* domain = nullptr;
};

struct admission_control {
    std::shared_ptr<join_state<int>> state;
    elio::coro::cancel_source* cancellation;
    int winner = 0;
    bool throw_setup = false;
};
std::atomic<admission_control*> active_admission{nullptr};

void controlled_admission() {
    auto* control = active_admission.load(std::memory_order_acquire);
    if (control->winner == 1) control->state->set_value(23);
    if (control->winner == 2) control->cancellation->cancel();
    if (control->throw_setup) throw std::bad_alloc();
    elio::runtime::detail::reject_next_spawn_for_test.store(true, std::memory_order_release);
}

struct admission_hook_guard {
    explicit admission_hook_guard(admission_control& control) {
        active_admission.store(&control, std::memory_order_release);
        elio::coro::detail::join_timer_admission_for_test.store(controlled_admission,
                                                              std::memory_order_release);
    }
    ~admission_hook_guard() {
        elio::coro::detail::join_timer_admission_for_test.store(nullptr,
                                                              std::memory_order_release);
        active_admission.store(nullptr, std::memory_order_release);
        elio::runtime::detail::reject_next_spawn_for_test.store(false, std::memory_order_release);
    }
};

task<void> record_observation(task<join_wait_outcome> observation,
                              observation_result& result) {
    try { result.value = co_await std::move(observation); }
    catch (...) { result.failure = std::current_exception(); }
    result.domain = scheduler::current();
    result.returns.fetch_add(1, std::memory_order_relaxed);
    result.done.store(true, std::memory_order_release);
}

task<void> raw_node_observer(
        std::shared_ptr<elio::coro::detail::join_observation> node,
        std::atomic<bool>& returned) {
    co_await elio::coro::detail::join_observation_awaitable(std::move(node));
    returned.store(true, std::memory_order_release);
}

template<typename T>
void check_duplicate_direct_awaiters() {
    auto state = std::make_shared<join_state<T>>();
    join_handle<T> handle(state);
    struct direct_result {
        std::exception_ptr failure;
        int value = 0;
        std::atomic<bool> done{false};
    } first, second;
    auto& pause = elio::coro::detail::pause_join_direct_rejection_for_test;
    struct unpause_guard {
        std::atomic<bool>& pause;
        ~unpause_guard() {
            pause.store(false, std::memory_order_release);
            pause.notify_all();
        }
    };
    scheduler sched(2);
    unpause_guard release{pause};
    pause.store(true, std::memory_order_release);
    auto await_direct = [&](direct_result& result) -> task<void> {
        try {
            auto& result_handle = handle;
            if constexpr (std::is_void_v<T>) co_await result_handle;
            else result.value = co_await result_handle;
        } catch (...) { result.failure = std::current_exception(); }
        result.done.store(true, std::memory_order_release);
    };
    sched.start();
    sched.go_to(0, await_direct(first));
    const bool first_registered = wait_for([&] {
        return (state->result_observer_flags_for_test() &
                join_state<T>::observer_mask) == join_state<T>::direct_observer;
    });
    sched.go_to(1, await_direct(second));
    const bool rejection_paused = wait_for([] {
        return elio::coro::detail::join_direct_rejection_paused_for_test.load(
            std::memory_order_acquire);
    });
    sched.go_to(0, [&]() -> task<void> {
        if constexpr (std::is_void_v<T>) state->set_value();
        else state->set_value(41);
        co_return;
    });
    const bool first_finished = wait_for([&] {
        return first.done.load(std::memory_order_acquire);
    });
    const bool second_still_parked = !second.done.load(std::memory_order_acquire);
    pause.store(false, std::memory_order_release);
    pause.notify_all();
    const bool second_finished = wait_for([&] {
        return second.done.load(std::memory_order_acquire);
    });
    const bool drained = sched.shutdown(elio::test::scaled_ms(5000));
    REQUIRE(drained);
    REQUIRE(first_registered);
    REQUIRE(rejection_paused);
    REQUIRE(first_finished);
    REQUIRE(second_still_parked);
    REQUIRE(second_finished);
    REQUIRE_FALSE(first.failure);
    REQUIRE(second.failure);
    REQUIRE_THROWS_AS(std::rethrow_exception(second.failure), std::logic_error);
    if constexpr (!std::is_void_v<T>) REQUIRE(first.value == 41);
}
} // namespace

TEST_CASE("join readiness initial precedence is ready then cancelled then expired",
          "[task][join_handle][join_wait][contract]") {
    STATIC_REQUIRE(sizeof(elio::coro::detail::join_state_base) <= 128);
    STATIC_REQUIRE(sizeof(join_state<void>) <= 128);
    STATIC_REQUIRE(sizeof(join_state<uint64_t>) <= 128);
    auto state = std::make_shared<join_state<std::unique_ptr<int>>>();
    join_handle<std::unique_ptr<int>> handle(state);
    elio::coro::cancel_source cancelled;
    cancelled.cancel();
    const auto expired = std::chrono::steady_clock::now() - std::chrono::seconds(1);
    REQUIRE(run_immediately(handle.wait_until(expired, cancelled.get_token())) ==
            join_wait_outcome::cancelled);
    REQUIRE_FALSE(handle.is_cancellation_requested());
    REQUIRE(run_immediately(handle.wait_until(expired)) == join_wait_outcome::timed_out);
    state->set_value(std::make_unique<int>(7));
    REQUIRE(run_immediately(handle.wait_until(expired, cancelled.get_token())) ==
            join_wait_outcome::completed);
    REQUIRE(run_immediately(handle.wait()) == join_wait_outcome::completed);
    REQUIRE_FALSE(handle.is_destroyed());
    auto value = handle.await_resume();
    REQUIRE(value);
    REQUIRE(*value == 7);
}

TEST_CASE("join readiness observes throwing tasks without consuming their failure",
          "[task][join_handle][join_wait][exception]") {
    auto state = std::make_shared<join_state<void>>();
    join_handle<void> handle(state);
    state->set_exception(std::make_exception_ptr(std::runtime_error("child failed")));
    REQUIRE(run_immediately(handle.wait()) == join_wait_outcome::completed);
    REQUIRE(run_immediately(handle.wait()) == join_wait_outcome::completed);
    REQUIRE_THROWS_AS(handle.await_resume(), std::runtime_error);
}

TEST_CASE("pending join readiness requires an observing scheduler worker",
          "[task][join_handle][join_wait][contract]") {
    auto state = std::make_shared<join_state<int>>();
    join_handle<int> handle(state);
    REQUIRE_THROWS_AS(run_immediately(handle.wait()), std::logic_error);
    auto observation = handle.wait();
    join_handle<int> moved(std::move(handle));
    state->set_value(9);
    REQUIRE(run_immediately(std::move(observation)) == join_wait_outcome::completed);
    REQUIRE(moved.await_resume() == 9);
}

TEST_CASE("token-less join readiness remains pending after observing-task cancellation",
          "[task][join_handle][join_wait][cancel_token][regression]") {
    auto state = std::make_shared<join_state<int>>();
    join_handle<int> handle(state);
    observation_result result;
    std::atomic<bool> marker{false};
    scheduler sched(1);
    sched.start();
    auto observer = sched.go_joinable(record_observation(handle.wait(), result));
    const bool registered = wait_for([&] {
        return !state->result_observer_for_test().expired();
    });
    observer.request_cancel();
    sched.go([&]() -> task<void> {
        marker.store(true, std::memory_order_release);
        co_return;
    });
    const bool marked = wait_for([&] { return marker.load(std::memory_order_acquire); });
    const bool returned_before_child = result.done.load(std::memory_order_acquire);
    state->set_value(53);
    const bool returned = wait_for([&] { return result.done.load(std::memory_order_acquire); });
    const bool drained = sched.shutdown(elio::test::scaled_ms(5000));
    REQUIRE(drained);
    REQUIRE(registered);
    REQUIRE(marked);
    REQUIRE(observer.is_cancellation_requested());
    REQUIRE_FALSE(returned_before_child);
    REQUIRE(returned);
    REQUIRE_FALSE(result.failure);
    REQUIRE(result.value == join_wait_outcome::completed);
    REQUIRE(result.returns.load(std::memory_order_acquire) == 1);
    REQUIRE_FALSE(handle.is_cancellation_requested());
    REQUIRE(handle.await_resume() == 53);
    REQUIRE_NOTHROW(observer.await_resume());
    REQUIRE(observer.is_destroyed());
}

TEST_CASE("join readiness completion cancellation and deadline have one stable winner",
          "[task][join_handle][join_wait][race]") {
    enum class event { completion, cancellation, deadline };
    const auto first = GENERATE(event::completion, event::cancellation, event::deadline);
    const auto second = GENERATE(event::completion, event::cancellation, event::deadline);
    if (first == second) return;
    auto state = std::make_shared<join_state<int>>();
    join_handle<int> handle(state);
    timer_control control;
    timer_hook_guard hooks(control);
    elio::coro::cancel_source cancellation;
    observation_result result;
    scheduler sched(2);
    release_guard release{control};
    sched.start();
    sched.go(record_observation(handle.wait_until(
        std::chrono::steady_clock::now() + elio::test::scaled_sec(10),
        cancellation.get_token()), result));
    const bool entered = wait_for([&] { return control.entered.load(std::memory_order_acquire); });
    bool completed = false;
    auto trigger = [&](event action) {
        switch (action) {
        case event::completion:
            if (!completed) { state->set_value(7); completed = true; }
            break;
        case event::cancellation: cancellation.cancel(); break;
        case event::deadline: control.expire.set(); break;
        }
    };
    trigger(first);
    const bool returned = wait_for([&] { return result.done.load(std::memory_order_acquire); });
    trigger(second);
    if (!completed) state->set_value(7);
    control.expire.set();
    control.settle.set();
    cancellation.cancel();
    const bool drained = sched.shutdown(elio::test::scaled_ms(5000));
    REQUIRE(drained);
    REQUIRE(entered);
    REQUIRE(returned);
    REQUIRE_FALSE(result.failure);
    const auto expected = first == event::completion ? join_wait_outcome::completed :
        first == event::cancellation ? join_wait_outcome::cancelled : join_wait_outcome::timed_out;
    REQUIRE(result.value == expected);
    REQUIRE(result.returns.load(std::memory_order_acquire) == 1);
    REQUIRE(control.settled.load(std::memory_order_acquire));
    REQUIRE_FALSE(handle.is_cancellation_requested());
    REQUIRE(handle.await_resume() == 7);
}

TEST_CASE("join readiness timeout permits retry and late completion",
          "[task][join_handle][join_wait][retry]") {
    auto state = std::make_shared<join_state<int>>();
    join_handle<int> handle(state);
    timer_control control;
    timer_hook_guard hooks(control);
    elio::coro::cancel_source cancellation;
    observation_result first;
    observation_result retry;
    scheduler sched(2);
    release_guard release{control};
    sched.start();
    sched.go(record_observation(handle.wait_until(
        std::chrono::steady_clock::now() + elio::test::scaled_sec(10)), first));
    const bool entered = wait_for([&] { return control.entered.load(std::memory_order_acquire); });
    control.expire.set();
    const bool departed = wait_for([&] { return first.done.load(std::memory_order_acquire); });
    const bool still_pending = !handle.is_ready();
    const auto installed = elio::coro::detail::join_observer_installed_for_test.load();
    sched.go(record_observation(handle.wait(cancellation.get_token()), retry));
    const bool retried = wait_for([&] {
        return elio::coro::detail::join_observer_installed_for_test.load(std::memory_order_acquire) > installed;
    });
    state->set_value(11);
    const bool returned = wait_for([&] { return retry.done.load(std::memory_order_acquire); });
    cancellation.cancel();
    control.settle.set();
    const bool drained = sched.shutdown(elio::test::scaled_ms(5000));
    REQUIRE(drained);
    REQUIRE(entered);
    REQUIRE(departed);
    REQUIRE(still_pending);
    REQUIRE(retried);
    REQUIRE(returned);
    REQUIRE_FALSE(first.failure);
    REQUIRE_FALSE(retry.failure);
    REQUIRE(first.value == join_wait_outcome::timed_out);
    REQUIRE(retry.value == join_wait_outcome::completed);
    REQUIRE(first.returns.load() == 1);
    REQUIRE(retry.returns.load() == 1);
    REQUIRE_FALSE(handle.is_cancellation_requested());
    REQUIRE(handle.await_resume() == 11);
}

TEST_CASE("join readiness cleanup does not wait for an external cancellation dispatcher",
          "[task][join_handle][join_wait][cancel_token][regression]") {
    auto state = std::make_shared<join_state<int>>();
    join_handle<int> handle(state);
    timer_control control;
    timer_hook_guard hooks(control);
    elio::coro::cancel_source cancellation;
    observation_result result;
    std::atomic<bool> callback_entered{false};
    std::atomic<bool> release_callback{false};
    std::exception_ptr dispatch_failure;
    scheduler sched(2);
    release_guard release{control};
    sched.start();
    sched.go(record_observation(handle.wait_until(
        std::chrono::steady_clock::now() + elio::test::scaled_sec(10),
        cancellation.get_token()), result));
    const bool installed = wait_for([&] { return control.entered.load(std::memory_order_acquire); });
    // Newest callbacks dispatch first. This gate holds the dispatcher after
    // it has claimed the observer callback but before it can invoke it.
    auto external = cancellation.get_token().on_cancel([&] {
        callback_entered.store(true, std::memory_order_release);
        while (!release_callback.load(std::memory_order_acquire)) {
            release_callback.wait(false, std::memory_order_acquire);
        }
    });
    std::thread dispatcher([&] {
        try { cancellation.cancel(); }
        catch (...) { dispatch_failure = std::current_exception(); }
    });
    const bool held = wait_for([&] { return callback_entered.load(std::memory_order_acquire); });
    state->set_value(17);
    const bool returned_while_held = wait_for([&] { return result.done.load(std::memory_order_acquire); });
    release_callback.store(true, std::memory_order_release);
    release_callback.notify_all();
    dispatcher.join();
    control.expire.set();
    control.settle.set();
    const bool drained = sched.shutdown(elio::test::scaled_ms(5000));
    REQUIRE(drained);
    REQUIRE(installed);
    REQUIRE(held);
    REQUIRE(returned_while_held);
    REQUIRE_FALSE(dispatch_failure);
    REQUIRE_FALSE(result.failure);
    REQUIRE(result.value == join_wait_outcome::completed);
    REQUIRE(result.returns.load(std::memory_order_acquire) == 1);
    REQUIRE_FALSE(handle.is_cancellation_requested());
    REQUIRE(handle.await_resume() == 17);
}

TEST_CASE("join timer admission failure cannot replace an established outcome",
          "[task][join_handle][join_wait][exception][regression]") {
    const int winner = GENERATE(0, 1, 2);
    const bool throw_setup = GENERATE(false, true);
    auto state = std::make_shared<join_state<int>>();
    join_handle<int> handle(state);
    elio::coro::cancel_source cancellation;
    admission_control control{state, &cancellation, winner, throw_setup};
    admission_hook_guard hook(control);
    observation_result result;
    scheduler sched(1);
    sched.start();
    sched.go(record_observation(handle.wait_until(
        std::chrono::steady_clock::now() + elio::test::scaled_sec(10),
        cancellation.get_token()), result));
    const bool returned = wait_for([&] { return result.done.load(std::memory_order_acquire); });
    if (winner != 1) state->set_value(23);
    const bool drained = sched.shutdown(elio::test::scaled_ms(5000));
    REQUIRE(drained);
    REQUIRE(returned);
    REQUIRE(result.returns.load(std::memory_order_acquire) == 1);
    REQUIRE_FALSE(handle.is_cancellation_requested());
    if (winner == 0) {
        REQUIRE(result.failure);
        if (throw_setup) REQUIRE_THROWS_AS(std::rethrow_exception(result.failure), std::bad_alloc);
        else REQUIRE_THROWS_AS(std::rethrow_exception(result.failure), std::logic_error);
        REQUIRE_FALSE(result.value);
    } else {
        REQUIRE_FALSE(result.failure);
        REQUIRE(result.value == (winner == 1 ? join_wait_outcome::completed :
                                             join_wait_outcome::cancelled));
    }
    REQUIRE(handle.await_resume() == 23);
}

TEST_CASE("join observation installation cannot lose concurrent child completion",
          "[task][join_handle][join_wait][race][regression]") {
    auto state = std::make_shared<join_state<int>>();
    join_handle<int> handle(state);
    timer_control control;
    observation_result result;
    scheduler sched(1);
    release_guard release{control};
    auto& pause = elio::coro::detail::pause_join_observer_install_for_test;
    pause.store(true, std::memory_order_release);
    sched.start();
    sched.go(record_observation(handle.wait(), result));
    const bool installing = wait_for([&] {
        return elio::coro::detail::join_observer_install_paused_for_test.load(std::memory_order_acquire);
    });
    state->set_value(29);
    pause.store(false, std::memory_order_release);
    pause.notify_all();
    const bool returned = wait_for([&] { return result.done.load(std::memory_order_acquire); });
    const bool drained = sched.shutdown(elio::test::scaled_ms(5000));
    REQUIRE(drained);
    REQUIRE(installing);
    REQUIRE(returned);
    REQUIRE_FALSE(result.failure);
    REQUIRE(result.value == join_wait_outcome::completed);
    REQUIRE(result.returns.load(std::memory_order_acquire) == 1);
    REQUIRE(handle.await_resume() == 29);
}

TEST_CASE("join readiness drains its private timer before returning",
          "[task][join_handle][join_wait][lifecycle][regression]") {
    auto state = std::make_shared<join_state<int>>();
    join_handle<int> handle(state);
    timer_control control;
    control.settle.reset();
    timer_hook_guard hooks(control);
    observation_result result;
    scheduler sched(1);
    release_guard release{control};
    sched.start();
    sched.go(record_observation(handle.wait_until(
        std::chrono::steady_clock::now() + elio::test::scaled_sec(10)), result));
    const bool entered = wait_for([&] { return control.entered.load(std::memory_order_acquire); });
    state->set_value(31);
    const bool stopped = wait_for([&] { return control.cancelled.load(std::memory_order_acquire); });
    // A marker runs on the only worker while the timer remains parked. A
    // cancel-only cleanup would already have let the observer return.
    std::atomic<bool> marker{false};
    sched.go([&]() -> task<void> { marker.store(true, std::memory_order_release); co_return; });
    const bool marked = wait_for([&] { return marker.load(std::memory_order_acquire); });
    const bool returned_before_settlement = result.done.load(std::memory_order_acquire);
    control.settle.set();
    const bool returned = wait_for([&] { return result.done.load(std::memory_order_acquire); });
    const bool drained = sched.shutdown(elio::test::scaled_ms(5000));
    REQUIRE(drained);
    REQUIRE(entered);
    REQUIRE(stopped);
    REQUIRE(marked);
    REQUIRE_FALSE(returned_before_settlement);
    REQUIRE(returned);
    REQUIRE_FALSE(result.failure);
    REQUIRE(result.value == join_wait_outcome::completed);
    REQUIRE(control.settled.load(std::memory_order_acquire));
    REQUIRE(handle.await_resume() == 31);
}

TEST_CASE("join timer frame drain and exceptional cleanup preserve ownership",
          "[task][join_handle][join_wait][lifecycle][exception][regression]") {
    const bool fail_allocation = GENERATE(false, true);
    auto state = std::make_shared<join_state<int>>();
    join_handle<int> handle(state);
    timer_control control;
    timer_hook_guard timer_hooks(control);
    destruction_setup_control setup;
    setup.fail_allocation = fail_allocation;
    destruction_setup_hook_guard setup_hooks(setup);
    observation_result result;
    std::atomic<bool> marker{false};
    auto& pause = elio::coro::detail::pause_before_detached_frame_destroy_for_test;
    struct frame_release_guard {
        std::atomic<bool>& pause;
        ~frame_release_guard() {
            pause.store(false, std::memory_order_release);
            pause.notify_all();
        }
    };
    scheduler sched(2);
    release_guard release_timer{control};
    frame_release_guard release_frame{pause};
    pause.store(true, std::memory_order_release);
    sched.start();
    sched.go(record_observation(handle.wait_until(
        std::chrono::steady_clock::now() + elio::test::scaled_sec(10)), result));
    const bool entered = wait_for([&] { return control.entered.load(std::memory_order_acquire); });
    auto lifetime = state->result_observer_for_test();
    state->set_value(47);
    const bool frame_paused = wait_for([] {
        return elio::coro::detail::detached_frame_destroy_paused_for_test.load(
            std::memory_order_acquire);
    });
    const bool cleanup_started = wait_for([&] {
        return setup.entered.load(std::memory_order_acquire);
    });
    sched.go([&]() -> task<void> {
        marker.store(true, std::memory_order_release);
        co_return;
    });
    const bool marked = wait_for([&] { return marker.load(std::memory_order_acquire); });
    bool exceptional_returned = false;
    if (fail_allocation) {
        exceptional_returned = wait_for([&] { return result.done.load(std::memory_order_acquire); });
    }
    const bool returned_before_destroy = result.done.load(std::memory_order_acquire);
    const bool timer_retains_state = !lifetime.expired();
    bool completion_won = false;
    if (auto node = lifetime.lock()) {
        completion_won = node->result() == join_wait_outcome::completed;
    }
    pause.store(false, std::memory_order_release);
    pause.notify_all();
    const bool returned = wait_for([&] { return result.done.load(std::memory_order_acquire); });
    const bool drained = sched.shutdown(elio::test::scaled_ms(5000));
    REQUIRE(drained);
    REQUIRE(entered);
    REQUIRE(frame_paused);
    REQUIRE(cleanup_started);
    REQUIRE(marked);
    REQUIRE(timer_retains_state);
    REQUIRE(completion_won);
    REQUIRE(returned);
    REQUIRE(result.returns.load(std::memory_order_acquire) == 1);
    REQUIRE(control.cancelled.load(std::memory_order_acquire));
    REQUIRE(control.settled.load(std::memory_order_acquire));
    REQUIRE(lifetime.expired());
    REQUIRE_FALSE(handle.is_cancellation_requested());
    REQUIRE(handle.await_resume() == 47);
    if (fail_allocation) {
        REQUIRE(exceptional_returned);
        REQUIRE(returned_before_destroy);
        REQUIRE(result.failure);
        REQUIRE_THROWS_AS(std::rethrow_exception(result.failure), std::bad_alloc);
        REQUIRE_FALSE(result.value);
    } else {
        REQUIRE_FALSE(returned_before_destroy);
        REQUIRE_FALSE(result.failure);
        REQUIRE(result.value == join_wait_outcome::completed);
    }
}

TEST_CASE("join result waits reject both directions of concurrent registration",
          "[task][join_handle][join_wait][contract][regression]") {
    const bool bounded_first = GENERATE(false, true);
    auto state = std::make_shared<join_state<int>>();
    join_handle<int> handle(state);
    observation_result bounded;
    std::exception_ptr direct_failure;
    int direct_value = 0;
    std::atomic<bool> direct_done{false};
    scheduler sched(1);
    sched.start();
    auto start_direct = [&] {
        sched.go([&]() -> task<void> {
            try {
                auto& result_handle = handle;
                direct_value = co_await result_handle;
            } catch (...) { direct_failure = std::current_exception(); }
            direct_done.store(true, std::memory_order_release);
        });
    };
    auto start_bounded = [&] { sched.go(record_observation(handle.wait(), bounded)); };
    if (bounded_first) start_bounded();
    else start_direct();
    const unsigned first_mode = bounded_first ? join_state<int>::bounded_observer :
                                               join_state<int>::direct_observer;
    const bool registered = wait_for([&] {
        return (state->result_observer_flags_for_test() &
                join_state<int>::observer_mask) == first_mode;
    });
    if (bounded_first) start_direct();
    else start_bounded();
    const bool rejected = wait_for([&] {
        return bounded_first ? direct_done.load(std::memory_order_acquire) :
                               bounded.done.load(std::memory_order_acquire);
    });
    sched.go([&]() -> task<void> { state->set_value(37); co_return; });
    const bool finished = wait_for([&] {
        return direct_done.load(std::memory_order_acquire) &&
               bounded.done.load(std::memory_order_acquire);
    });
    const bool drained = sched.shutdown(elio::test::scaled_ms(5000));
    REQUIRE(drained);
    REQUIRE(registered);
    REQUIRE(rejected);
    REQUIRE(finished);
    if (bounded_first) {
        REQUIRE(direct_failure);
        REQUIRE_THROWS_AS(std::rethrow_exception(direct_failure), std::logic_error);
        REQUIRE_FALSE(bounded.failure);
        REQUIRE(bounded.value == join_wait_outcome::completed);
        REQUIRE(handle.await_resume() == 37);
    } else {
        REQUIRE(bounded.failure);
        REQUIRE_THROWS_AS(std::rethrow_exception(bounded.failure), std::logic_error);
        REQUIRE_FALSE(direct_failure);
        REQUIRE(direct_value == 37);
    }
}

TEST_CASE("duplicate direct join rejection stays local while the first awaiter resumes",
          "[task][join_handle][join_wait][contract][regression]") {
    const bool void_result = GENERATE(false, true);
    if (void_result) check_duplicate_direct_awaiters<void>();
    else check_duplicate_direct_awaiters<int>();
}

TEST_CASE("join readiness factory retains state after the original handle is discarded",
          "[task][join_handle][join_wait][lifecycle]") {
    std::weak_ptr<join_state<void>> weak;
    auto operation = [&] {
        auto state = std::make_shared<join_state<void>>();
        weak = state;
        join_handle<void> handle(state);
        return handle.wait();
    }();
    const bool pinned_before_execution = !weak.expired();
    observation_result result;
    scheduler sched(1);
    sched.start();
    sched.go(record_observation(std::move(operation), result));
    auto state = weak.lock();
    if (state) state->set_value();
    const bool returned = wait_for([&] { return result.done.load(std::memory_order_acquire); });
    const bool drained = sched.shutdown(elio::test::scaled_ms(5000));
    REQUIRE(drained);
    REQUIRE(pinned_before_execution);
    REQUIRE(state);
    REQUIRE(returned);
    REQUIRE_FALSE(result.failure);
    REQUIRE(result.value == join_wait_outcome::completed);
    state.reset();
    REQUIRE(weak.expired());
}

TEST_CASE("join readiness reports owned timer failures without consuming the child",
          "[task][join_handle][join_wait][exception]") {
    auto state = std::make_shared<join_state<void>>();
    join_handle<void> handle(state);
    timer_control control;
    control.throw_on_entry = true;
    timer_hook_guard hooks(control);
    observation_result result;
    scheduler sched(1);
    release_guard release{control};
    sched.start();
    sched.go(record_observation(handle.wait_until(
        std::chrono::steady_clock::now() + elio::test::scaled_sec(10)), result));
    const bool returned = wait_for([&] { return result.done.load(std::memory_order_acquire); });
    const bool pending_after_failure = !handle.is_ready();
    state->set_value();
    const bool drained = sched.shutdown(elio::test::scaled_ms(5000));
    REQUIRE(drained);
    REQUIRE(returned);
    REQUIRE(pending_after_failure);
    REQUIRE(result.failure);
    REQUIRE_THROWS_AS(std::rethrow_exception(result.failure), std::runtime_error);
    REQUIRE_FALSE(result.value);
    REQUIRE_FALSE(handle.is_cancellation_requested());
    REQUIRE_NOTHROW(handle.await_resume());
}

TEST_CASE("join readiness deadline uses either real I/O timer backend",
          "[task][join_handle][join_wait][timer]") {
    const auto backend = GENERATE(elio::io::io_context::backend_type::auto_detect,
                                 elio::io::io_context::backend_type::epoll);
    auto& override_backend = elio::runtime::detail::worker_io_backend_for_test;
    const auto previous = override_backend.exchange(backend);
    struct backend_guard {
        elio::io::io_context::backend_type previous;
        ~backend_guard() {
            elio::runtime::detail::worker_io_backend_for_test.store(previous);
        }
    } restore{previous};
    auto state = std::make_shared<join_state<void>>();
    join_handle<void> handle(state);
    observation_result result;
    scheduler sched(1);
    sched.start();
    sched.go(record_observation(handle.wait_until(
        std::chrono::steady_clock::now() + elio::test::scaled_ms(20)), result));
    const bool returned = wait_for([&] { return result.done.load(std::memory_order_acquire); });
    const bool pending_after_timeout = !handle.is_ready();
    state->set_value();
    const bool drained = sched.shutdown(elio::test::scaled_ms(5000));
    REQUIRE(drained);
    REQUIRE(returned);
    REQUIRE(pending_after_timeout);
    REQUIRE_FALSE(result.failure);
    REQUIRE(result.value == join_wait_outcome::timed_out);
    REQUIRE_FALSE(handle.is_cancellation_requested());
    REQUIRE_NOTHROW(handle.await_resume());
}

TEST_CASE("join result observation does not imply final frame destruction",
          "[task][join_handle][join_wait][lifecycle]") {
    scheduler sched(1);
    struct frame_pause_guard {
        frame_pause_guard() {
            elio::coro::detail::pause_before_detached_frame_destroy_for_test.store(true);
        }
        void release() {
            auto& pause = elio::coro::detail::pause_before_detached_frame_destroy_for_test;
            pause.store(false, std::memory_order_release);
            pause.notify_all();
        }
        ~frame_pause_guard() { release(); }
    } pause;
    sched.start();
    auto child = []() -> task<int> { co_return 41; };
    auto handle = sched.go_joinable(child);
    const bool frame_held = wait_for([&] {
        return elio::coro::detail::detached_frame_destroy_paused_for_test.load(std::memory_order_acquire);
    });
    std::optional<join_wait_outcome> observed;
    if (frame_held) observed = run_immediately(handle.wait());
    const bool destroyed_while_held = handle.is_destroyed();
    pause.release();
    const bool drained = sched.shutdown(elio::test::scaled_ms(5000));
    REQUIRE(drained);
    REQUIRE(frame_held);
    REQUIRE(observed == join_wait_outcome::completed);
    REQUIRE_FALSE(destroyed_while_held);
    REQUIRE(handle.is_destroyed());
    REQUIRE(handle.await_resume() == 41);
}

TEST_CASE("join observer abandonment invalidates a selected unclaimed wake",
          "[task][join_handle][join_wait][lifecycle][regression]") {
    scheduler sched(1);
    auto node = std::make_shared<elio::coro::detail::join_observation>(&sched);
    std::atomic<bool> returned{false};
    std::optional<task<void>> operation(raw_node_observer(node, returned));
    auto handle = elio::coro::detail::task_access::handle(*operation);
    handle.resume();
    sched.start();
    auto& pause = elio::coro::detail::pause_before_completion_wake_claim_for_test;
    pause.store(true, std::memory_order_release);
    std::thread producer([&] { node->notify(elio::coro::detail::join_observation::outcome::completed); });
    const bool selected = wait_for([&] {
        return elio::coro::detail::completion_wake_claim_paused_for_test.load(std::memory_order_acquire);
    });
    if (selected) operation.reset();
    pause.store(false, std::memory_order_release);
    pause.notify_all();
    producer.join();
    if (!selected) {
        (void)wait_for([&] { return returned.load(std::memory_order_acquire); });
    }
    const bool drained = sched.shutdown(elio::test::scaled_ms(5000));
    REQUIRE(drained);
    REQUIRE(selected);
    REQUIRE_FALSE(returned.load(std::memory_order_acquire));
}

TEST_CASE("join observer timers remain admissible during graceful drain",
          "[task][join_handle][join_wait][shutdown][regression]") {
    auto state = std::make_shared<join_state<void>>();
    join_handle<void> handle(state);
    timer_control control;
    timer_hook_guard hooks(control);
    elio::sync::event begin;
    observation_result result;
    scheduler sched(1);
    release_guard release{control};
    elio::runtime::detail::graceful_admission_closed_for_test.store(false);
    sched.start();
    sched.go([&]() -> task<void> {
        co_await begin.wait();
        co_await record_observation(handle.wait_until(
            std::chrono::steady_clock::now() + elio::test::scaled_sec(10)), result);
    });
    bool shutdown_succeeded = false;
    std::thread shutdown([&] { shutdown_succeeded = sched.shutdown(elio::test::scaled_sec(5)); });
    const bool draining = wait_for([&] {
        return elio::runtime::detail::graceful_admission_closed_for_test.load(std::memory_order_acquire);
    });
    begin.set();
    const bool admitted = wait_for([&] { return control.entered.load(std::memory_order_acquire); });
    control.expire.set();
    control.settle.set();
    const bool returned = wait_for([&] { return result.done.load(std::memory_order_acquire); });
    state->set_value();
    shutdown.join();
    REQUIRE(shutdown_succeeded);
    REQUIRE(draining);
    REQUIRE(admitted);
    REQUIRE(returned);
    REQUIRE_FALSE(result.failure);
    REQUIRE(result.value == join_wait_outcome::timed_out);
    REQUIRE_FALSE(handle.is_cancellation_requested());
    REQUIRE_NOTHROW(handle.await_resume());
}

TEST_CASE("join readiness resumes in its observing scheduler domain",
          "[task][join_handle][join_wait][scheduler][regression]") {
    auto state = std::make_shared<join_state<void>>();
    join_handle<void> handle(state);
    observation_result result;
    scheduler producer(1);
    scheduler observer(1);
    producer.start();
    observer.start();
    const auto installed = elio::coro::detail::join_observer_installed_for_test.load();
    observer.go(record_observation(handle.wait(), result));
    const bool registered = wait_for([&] {
        return elio::coro::detail::join_observer_installed_for_test.load(std::memory_order_acquire) > installed;
    });
    producer.go([&]() -> task<void> { state->set_value(); co_return; });
    const bool returned = wait_for([&] { return result.done.load(std::memory_order_acquire); });
    const bool producer_drained = producer.shutdown(elio::test::scaled_ms(5000));
    const bool observer_drained = observer.shutdown(elio::test::scaled_ms(5000));
    REQUIRE(producer_drained);
    REQUIRE(observer_drained);
    REQUIRE(registered);
    REQUIRE(returned);
    REQUIRE_FALSE(result.failure);
    REQUIRE(result.value == join_wait_outcome::completed);
    REQUIRE(result.domain == &observer);
    REQUIRE_NOTHROW(handle.await_resume());
}

TEST_CASE("join readiness rejects another bounded pending observation",
          "[task][join_handle][join_wait][contract][regression]") {
    auto state = std::make_shared<join_state<void>>();
    join_handle<void> handle(state);
    elio::coro::cancel_source cancellation;
    observation_result first;
    observation_result second;
    scheduler sched(1);
    sched.start();
    sched.go(record_observation(handle.wait(cancellation.get_token()), first));
    const bool registered = wait_for([&] {
        return (state->result_observer_flags_for_test() &
                join_state<void>::observer_mask) == join_state<void>::bounded_observer;
    });
    sched.go(record_observation(handle.wait(), second));
    const bool rejected = wait_for([&] { return second.done.load(std::memory_order_acquire); });
    cancellation.cancel();
    const bool departed = wait_for([&] { return first.done.load(std::memory_order_acquire); });
    state->set_value();
    const bool drained = sched.shutdown(elio::test::scaled_ms(5000));
    REQUIRE(drained);
    REQUIRE(registered);
    REQUIRE(rejected);
    REQUIRE(departed);
    REQUIRE_FALSE(first.failure);
    REQUIRE(first.value == join_wait_outcome::cancelled);
    REQUIRE(second.failure);
    REQUIRE_THROWS_AS(std::rethrow_exception(second.failure), std::logic_error);
    REQUIRE_FALSE(handle.is_cancellation_requested());
    REQUIRE_NOTHROW(handle.await_resume());
}
