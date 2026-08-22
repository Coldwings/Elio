#include <catch2/catch_test_macros.hpp>
#include <elio/time/timer.hpp>
#include <elio/coro/task.hpp>
#include <elio/runtime/affinity.hpp>
#include <elio/runtime/scheduler.hpp>

#include "../test_main.cpp"

#include <chrono>
#include <atomic>
#include <exception>
#include <limits>
#include <optional>
#include <string_view>
#include <utility>

using namespace elio::time;
using namespace elio::coro;
using namespace elio::runtime;
using namespace std::chrono_literals;

// Helper to access handle from task
template<typename T>
auto get_handle(task<T>& t) {
    return elio::coro::detail::task_access::handle(t);
}

// Helper to spawn a task to scheduler using high-level API (fire-and-forget)
template<typename F>
void spawn_task(scheduler& sched, F&& f) {
    sched.go(std::forward<F>(f));
}

namespace {

template<typename T>
struct join_completion {
    bool ready = false;
    bool destroyed = false;
    std::optional<T> value;
    std::exception_ptr exception;
};

template<typename T>
join_completion<T> collect_join_completion(join_handle<T>& handle) {
    join_completion<T> result;
    result.ready = handle.is_ready();
    result.destroyed = handle.is_destroyed();
    if (!result.ready || !result.destroyed) return result;

    try {
        result.value.emplace(handle.await_resume());
    } catch (...) {
        result.exception = std::current_exception();
    }
    return result;
}

void rethrow_join_exception(const std::exception_ptr& exception) {
    if (exception) std::rethrow_exception(exception);
}

} // namespace

TEST_CASE("sleep_for basic", "[time][sleep]") {
    struct observation {
        std::chrono::steady_clock::duration elapsed;
    };

    auto sleep_task = []() -> task<observation> {
        auto start = std::chrono::steady_clock::now();
        co_await sleep_for(50ms);
        auto elapsed = std::chrono::steady_clock::now() - start;
        co_return observation{elapsed};
    };

    scheduler sched(1);
    sched.start();

    auto handle = sched.go_joinable(sleep_task);
    const bool drained = sched.shutdown(elio::test::scaled_sec(5));
    auto completion = collect_join_completion(handle);

    REQUIRE(drained);
    REQUIRE(completion.ready);
    REQUIRE(completion.destroyed);
    REQUIRE_NOTHROW(rethrow_join_exception(completion.exception));
    REQUIRE(completion.value.has_value());
    // Timer expiry must not resume the task before its deadline.
    REQUIRE(completion.value->elapsed >= 50ms);
}

TEST_CASE("sleep_for zero duration", "[time][sleep]") {
    std::atomic<bool> completed{false};
    
    auto sleep_task = [&]() -> task<void> {
        co_await sleep_for(0ms);  // Should complete immediately
        completed = true;
    };
    
    auto t = sleep_task();
    get_handle(t).resume();
    
    REQUIRE(completed);
}

TEST_CASE("sleep_for prepare fallback rejection preserves its awaiting task",
          "[time][sleep][shutdown][affinity]") {
    scheduler sched(1);
    sched.start();
    sched.get_blocking_pool()->shutdown();

    std::atomic<bool> completed{false};
    std::atomic<bool> rejected{false};
    std::atomic<size_t> before_worker{NO_AFFINITY};
    std::atomic<size_t> after_worker{NO_AFFINITY};
    std::atomic<size_t> task_pins_after_rejection{
        std::numeric_limits<size_t>::max()};
    std::atomic<size_t> context_pins_after_rejection{
        std::numeric_limits<size_t>::max()};
    auto sleep_task = [&]() -> task<void> {
        before_worker.store(elio::runtime::current_worker_id(),
                            std::memory_order_release);
        elio::time::detail::reject_next_timeout_prepare_for_test();
        try {
            co_await sleep_for(1s);
        } catch (const std::runtime_error& ex) {
            rejected.store(
                std::string_view(ex.what()).starts_with("sleep_for rejected:"),
                std::memory_order_release);
        }
        after_worker.store(elio::runtime::current_worker_id(),
                           std::memory_order_release);
        auto* frame = promise_base::current_frame();
        auto* worker = worker_thread::current();
        task_pins_after_rejection.store(
            frame ? frame->active_io_pin_count() :
                    std::numeric_limits<size_t>::max(),
            std::memory_order_release);
        context_pins_after_rejection.store(
            worker ? worker->io_context().active_pin_count() :
                     std::numeric_limits<size_t>::max(),
            std::memory_order_release);
        completed.store(true, std::memory_order_release);
    };

    spawn_task(sched, sleep_task);
    for (int i = 0; i < 100 && !completed.load(std::memory_order_acquire); ++i) {
        std::this_thread::sleep_for(10ms);
    }
    REQUIRE(sched.shutdown(5s));

    REQUIRE(completed.load(std::memory_order_acquire));
    REQUIRE(rejected.load(std::memory_order_acquire));
    REQUIRE(before_worker.load(std::memory_order_acquire) == 0);
    REQUIRE(after_worker.load(std::memory_order_acquire) == 0);
    REQUIRE(task_pins_after_rejection.load(std::memory_order_acquire) == 0);
    REQUIRE(context_pins_after_rejection.load(std::memory_order_acquire) == 0);
}

TEST_CASE("yield execution", "[time][yield]") {
    std::atomic<int> counter{0};
    std::atomic<int> completed{0};
    
    auto yield_task = [&]() -> task<void> {
        for (int i = 0; i < 3; ++i) {
            counter++;
            co_await yield();
        }
        completed++;
    };
    
    scheduler sched(2);
    sched.start();
    
    {
        spawn_task(sched, yield_task);
        spawn_task(sched, yield_task);
    }
    
    // Wait for completion
    for (int i = 0; i < 50 && completed < 2; ++i) {
        std::this_thread::sleep_for(10ms);
    }
    
    sched.shutdown();
    
    REQUIRE(completed == 2);
    REQUIRE(counter == 6);  // Each task increments 3 times
}

TEST_CASE("multiple sleeps sequential", "[time][sleep]") {
    struct observation {
        std::chrono::steady_clock::duration elapsed;
    };

    auto multi_sleep = []() -> task<observation> {
        auto start = std::chrono::steady_clock::now();

        co_await sleep_for(20ms);
        co_await sleep_for(20ms);
        co_await sleep_for(20ms);

        auto elapsed = std::chrono::steady_clock::now() - start;
        co_return observation{elapsed};
    };

    scheduler sched(1);
    sched.start();

    auto handle = sched.go_joinable(multi_sleep);
    const bool drained = sched.shutdown(elio::test::scaled_sec(5));
    auto completion = collect_join_completion(handle);

    REQUIRE(drained);
    REQUIRE(completion.ready);
    REQUIRE(completion.destroyed);
    REQUIRE_NOTHROW(rethrow_join_exception(completion.exception));
    REQUIRE(completion.value.has_value());
    // Should have waited approximately 60ms total.
    REQUIRE(completion.value->elapsed >= 50ms);
}

TEST_CASE("sleep_until", "[time][sleep]") {
    struct observation {
        std::chrono::steady_clock::time_point target;
        std::chrono::steady_clock::time_point resumed_at;
    };

    auto sleep_until_task = []() -> task<observation> {
        auto target = std::chrono::steady_clock::now() + 50ms;
        co_await sleep_until(target);
        co_return observation{target, std::chrono::steady_clock::now()};
    };

    scheduler sched(1);
    sched.start();

    auto handle = sched.go_joinable(sleep_until_task);
    const bool drained = sched.shutdown(elio::test::scaled_sec(5));
    auto completion = collect_join_completion(handle);

    REQUIRE(drained);
    REQUIRE(completion.ready);
    REQUIRE(completion.destroyed);
    REQUIRE_NOTHROW(rethrow_join_exception(completion.exception));
    REQUIRE(completion.value.has_value());
    // Should have waited until approximately the target time.
    REQUIRE(completion.value->resumed_at >= completion.value->target - 10ms);
}

TEST_CASE("sleep_until past time", "[time][sleep]") {
    std::atomic<bool> completed{false};
    
    auto past_sleep = [&]() -> task<void> {
        auto past = std::chrono::steady_clock::now() - 100ms;
        co_await sleep_until(past);  // Should complete immediately
        completed = true;
    };
    
    auto t = past_sleep();
    get_handle(t).resume();
    
    REQUIRE(completed);
}

TEST_CASE("cancellable sleep - normal completion", "[time][sleep][cancel]") {
    std::atomic<bool> completed{false};
    std::atomic<int> result_value{-1};
    
    cancel_source source;
    
    auto sleep_task = [&]() -> task<void> {
        auto result = co_await sleep_for(50ms, source.get_token());
        result_value = (result == cancel_result::completed) ? 1 : 0;
        completed = true;
    };
    
    scheduler sched(1);
    sched.start();
    
    {
        spawn_task(sched, sleep_task);
    }
    
    // Wait for completion without cancelling
    for (int i = 0; i < 100 && !completed; ++i) {
        std::this_thread::sleep_for(10ms);
    }
    
    sched.shutdown();
    
    REQUIRE(completed);
    REQUIRE(result_value == 1);  // Should be completed, not cancelled
}

TEST_CASE("cancellable sleep - cancelled early", "[time][sleep][cancel]") {
    std::atomic<bool> completed{false};
    std::atomic<bool> cancelled{false};
    
    cancel_source source;
    elio::time::detail::cancellable_sleep_registered_for_test.store(
        false, std::memory_order_release);
    
    auto sleep_task = [&]() -> task<void> {
        auto result = co_await sleep_for(1h, source.get_token());
        cancelled.store(result == cancel_result::cancelled,
                        std::memory_order_release);
        completed.store(true, std::memory_order_release);
    };
    
    scheduler sched(1);
    sched.start();
    
    {
        spawn_task(sched, sleep_task);
    }
    
    const auto registration_deadline =
        std::chrono::steady_clock::now() + 5s;
    while (!elio::time::detail::cancellable_sleep_registered_for_test.load(
               std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < registration_deadline) {
        std::this_thread::sleep_for(1ms);
    }
    const bool registered =
        elio::time::detail::cancellable_sleep_registered_for_test.load(
            std::memory_order_acquire);

    source.cancel();

    const auto completion_deadline = std::chrono::steady_clock::now() + 5s;
    while (!completed.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < completion_deadline) {
        std::this_thread::sleep_for(1ms);
    }
    const bool did_complete = completed.load(std::memory_order_acquire);
    const bool drained = sched.shutdown(5s);

    REQUIRE(registered);
    REQUIRE(did_complete);
    REQUIRE(cancelled.load(std::memory_order_acquire));
    REQUIRE(drained);
}

TEST_CASE("timer cancellation preserves cancelling thread virtual stack",
          "[time][sleep][cancel][virtual_stack][ownership]") {
    scheduler sched(1);
    sched.start();

    cancel_source source;
    std::atomic<bool> completed{false};
    std::atomic<bool> cancelled{false};
    elio::time::detail::cancellable_sleep_registered_for_test.store(
        false, std::memory_order_release);

    auto sleep_task = [&]() -> task<void> {
        auto result = co_await sleep_for(5s, source.get_token());
        cancelled.store(result == cancel_result::cancelled,
                        std::memory_order_release);
        completed.store(true, std::memory_order_release);
    };
    spawn_task(sched, sleep_task);

    const auto registration_deadline =
        std::chrono::steady_clock::now() + 5s;
    while (!elio::time::detail::cancellable_sleep_registered_for_test.load(
               std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < registration_deadline) {
        std::this_thread::sleep_for(1ms);
    }
    const bool registered =
        elio::time::detail::cancellable_sleep_registered_for_test.load(
            std::memory_order_acquire);

    bool frame_preserved = false;
    {
        promise_base caller;
        source.cancel();
        frame_preserved = promise_base::current_frame() == &caller;
    }

    const auto completion_deadline = std::chrono::steady_clock::now() + 5s;
    while (!completed.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < completion_deadline) {
        std::this_thread::sleep_for(1ms);
    }
    const bool did_complete = completed.load(std::memory_order_acquire);
    const bool drained = sched.shutdown(5s);

    REQUIRE(registered);
    REQUIRE(frame_preserved);
    REQUIRE(did_complete);
    REQUIRE(cancelled.load(std::memory_order_acquire));
    REQUIRE(drained);
}

TEST_CASE("cancellable sleep - already cancelled token", "[time][sleep][cancel]") {
    bool completed = false;
    cancel_result result_value = cancel_result::completed;
    
    cancel_source source;
    source.cancel();  // Cancel before sleep starts
    
    auto sleep_task = [&]() -> task<void> {
        result_value = co_await sleep_for(500ms, source.get_token());
        completed = true;
    };

    auto t = sleep_task();
    get_handle(t).resume();

    REQUIRE(completed);
    REQUIRE(result_value == cancel_result::cancelled);
}

TEST_CASE("cancellable sleep prepare fallback rejection preserves its awaiting task",
          "[time][sleep][cancel][shutdown][affinity]") {
    scheduler sched(1);
    sched.start();
    sched.get_blocking_pool()->shutdown();

    cancel_source source;
    std::atomic<bool> completed{false};
    std::atomic<bool> rejected{false};
    std::atomic<size_t> before_worker{NO_AFFINITY};
    std::atomic<size_t> after_worker{NO_AFFINITY};
    auto sleep_task = [&]() -> task<void> {
        before_worker.store(elio::runtime::current_worker_id(),
                            std::memory_order_release);
        elio::time::detail::reject_next_timeout_prepare_for_test();
        try {
            (void)co_await sleep_for(1s, source.get_token());
        } catch (const std::runtime_error& ex) {
            rejected.store(
                std::string_view(ex.what()).starts_with("sleep_for rejected:"),
                std::memory_order_release);
        }
        after_worker.store(elio::runtime::current_worker_id(),
                           std::memory_order_release);
        completed.store(true, std::memory_order_release);
    };

    spawn_task(sched, sleep_task);
    for (int i = 0; i < 100 && !completed.load(std::memory_order_acquire); ++i) {
        std::this_thread::sleep_for(10ms);
    }
    REQUIRE(sched.shutdown(5s));

    REQUIRE(completed.load(std::memory_order_acquire));
    REQUIRE(rejected.load(std::memory_order_acquire));
    REQUIRE(before_worker.load(std::memory_order_acquire) == 0);
    REQUIRE(after_worker.load(std::memory_order_acquire) == 0);
}

TEST_CASE("cancel_token basic operations", "[time][cancel]") {
    cancel_source source;
    auto token = source.get_token();
    
    REQUIRE_FALSE(token.is_cancelled());
    REQUIRE_FALSE(source.is_cancelled());
    REQUIRE(static_cast<bool>(token));  // token is truthy when not cancelled
    
    source.cancel();
    
    REQUIRE(token.is_cancelled());
    REQUIRE(source.is_cancelled());
    REQUIRE_FALSE(static_cast<bool>(token));  // token is falsy when cancelled
}

TEST_CASE("cancel_token callback invocation", "[time][cancel]") {
    cancel_source source;
    auto token = source.get_token();
    
    std::atomic<int> callback_count{0};
    
    {
        auto reg1 = token.on_cancel([&]() { callback_count++; });
        auto reg2 = token.on_cancel([&]() { callback_count++; });
        
        REQUIRE(callback_count == 0);
        
        source.cancel();
        
        REQUIRE(callback_count == 2);
    }
    
    // Registering after cancellation should invoke immediately
    auto reg3 = token.on_cancel([&]() { callback_count++; });
    REQUIRE(callback_count == 3);
}

TEST_CASE("cancel_token registration unregister", "[time][cancel]") {
    cancel_source source;
    auto token = source.get_token();
    
    std::atomic<int> callback_count{0};
    
    auto reg = token.on_cancel([&]() { callback_count++; });
    reg.unregister();  // Unregister before cancel
    
    source.cancel();
    
    REQUIRE(callback_count == 0);  // Callback should not have been invoked
}
