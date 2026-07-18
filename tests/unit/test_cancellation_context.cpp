#include <catch2/catch_test_macros.hpp>
#include <elio/coro/this_coro.hpp>
#include <elio/runtime/scheduler.hpp>
#include <elio/sync/primitives.hpp>
#include <elio/time/timer.hpp>

#include <atomic>
#include <chrono>
#include <latch>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>

#include "../test_main.cpp"

using namespace elio::coro;
using namespace elio::runtime;
using namespace elio::test;

namespace {

bool wait_for_flag(const std::atomic<bool>& flag) {
    const auto deadline = std::chrono::steady_clock::now() + scaled_sec(5);
    while (!flag.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return flag.load(std::memory_order_acquire);
}

template<typename T>
bool wait_until_ready(const join_handle<T>& handle) {
    const auto deadline = std::chrono::steady_clock::now() + scaled_sec(5);
    while (!handle.is_ready() &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return handle.is_ready();
}

task<int> wait_for_runtime_cancellation(std::atomic<bool>* started,
                                        std::atomic<bool>* observed) {
    auto token = this_coro::cancel_token();
    started->store(true, std::memory_order_release);
    while (!token.is_cancelled()) {
        co_await elio::time::yield();
    }
    observed->store(true, std::memory_order_release);
    co_return 17;
}

task<int> nested_runtime_cancellation(std::atomic<bool>* started,
                                      std::atomic<bool>* observed) {
    co_return co_await wait_for_runtime_cancellation(started, observed);
}

task<void> wait_for_explicit_cancellation(cancel_token explicit_token,
                                          std::atomic<bool>* started,
                                          std::atomic<bool>* runtime_cancelled) {
    auto runtime_token = this_coro::cancel_token();
    started->store(true, std::memory_order_release);
    while (!explicit_token.is_cancelled()) {
        co_await elio::time::yield();
    }
    runtime_cancelled->store(
        runtime_token.is_cancelled(), std::memory_order_release);
}

task<void> throwing_cancellation_callback(std::atomic<bool>* started) {
    auto token = this_coro::cancel_token();
    auto registration = token.on_cancel([] {
        throw std::runtime_error("runtime cancellation callback failed");
    });
    started->store(true, std::memory_order_release);
    while (!token.is_cancelled()) {
        co_await elio::time::yield();
    }
}

task<int> complete_during_cancellation(std::atomic<bool>* started,
                                       std::atomic<bool>* release) {
    auto token = this_coro::cancel_token();
    started->store(true, std::memory_order_release);
    while (!release->load(std::memory_order_acquire)) {
        co_await elio::time::yield();
    }
    co_return token.is_cancelled() ? 1 : 0;
}

task<void> capture_cancellation_then_suspend(cancel_token* observed) {
    *observed = this_coro::cancel_token();
    co_await std::suspend_always{};
}

task<void> await_cancellation_capture(cancel_token* observed) {
    co_await capture_cancellation_then_suspend(observed);
}

task<void> unused_runtime_frame() {
    co_return;
}

task<void> spawn_independent_child(
    scheduler* sched,
    std::optional<join_handle<int>>* child_handle,
    std::atomic<bool>* child_started,
    std::atomic<bool>* child_observed,
    std::atomic<bool>* parent_started,
    elio::sync::event* release_parent) {
    child_handle->emplace(sched->go_joinable(
        wait_for_runtime_cancellation, child_started, child_observed));
    parent_started->store(true, std::memory_order_release);
    co_await release_parent->wait();
}

} // namespace

TEST_CASE("this_coro cancellation token is empty outside runtime execution",
          "[task][cancellation_context][this_coro]") {
    auto token = this_coro::cancel_token();
    REQUIRE_FALSE(token.is_cancelled());
    REQUIRE(token);
}

TEST_CASE("lazy task cancellation binds to the actual Elio awaiter",
          "[task][cancellation_context][lifetime]") {
    cancel_token observed;
    auto parent = await_cancellation_capture(&observed);
    auto unrelated = unused_runtime_frame();
    auto parent_handle = elio::coro::detail::task_access::handle(parent);
    auto unrelated_handle =
        elio::coro::detail::task_access::handle(unrelated);

    promise_base::set_current_frame(&unrelated_handle.promise());
    parent_handle.resume();
    promise_base::set_current_frame(nullptr);

    REQUIRE_FALSE(observed.is_cancelled());
    parent_handle.promise().execution_context()->request_cancel();
    REQUIRE(observed.is_cancelled());
}

TEST_CASE("join handle cancellation reaches a nested lazy task chain",
          "[task][spawn][join_handle][cancellation_context]") {
    scheduler sched(2);
    sched.start();

    std::atomic<bool> started{false};
    std::atomic<bool> observed{false};
    auto joined = sched.go_joinable(
        nested_runtime_cancellation, &started, &observed);

    const bool did_start = wait_for_flag(started);
    if (!did_start) {
        sched.shutdown_force();
    }
    REQUIRE(did_start);
    REQUIRE_FALSE(joined.is_cancellation_requested());

    joined.request_cancel();
    const bool ready = wait_until_ready(joined);
    if (!ready) {
        sched.shutdown_force();
    }
    REQUIRE(ready);

    joined.wait_destroyed();
    REQUIRE(joined.await_resume() == 17);
    REQUIRE(observed.load(std::memory_order_acquire));
    REQUIRE(joined.is_cancellation_requested());
    REQUIRE(sched.shutdown());
}

TEST_CASE("separate join handles own independent cancellation contexts",
          "[task][spawn][join_handle][cancellation_context]") {
    scheduler sched(2);
    sched.start();

    std::atomic<bool> first_started{false};
    std::atomic<bool> first_observed{false};
    std::atomic<bool> second_started{false};
    std::atomic<bool> second_observed{false};
    auto first = sched.go_joinable(
        wait_for_runtime_cancellation, &first_started, &first_observed);
    auto second = sched.go_joinable(
        wait_for_runtime_cancellation, &second_started, &second_observed);

    const bool both_started =
        wait_for_flag(first_started) && wait_for_flag(second_started);
    if (!both_started) {
        sched.shutdown_force();
    }
    REQUIRE(both_started);

    first.request_cancel();
    const bool first_ready = wait_until_ready(first);
    REQUIRE_FALSE(second.is_cancellation_requested());
    REQUIRE_FALSE(second_observed.load(std::memory_order_acquire));

    second.request_cancel();
    const bool second_ready = wait_until_ready(second);
    if (!first_ready || !second_ready) {
        sched.shutdown_force();
    }
    REQUIRE(first_ready);
    REQUIRE(second_ready);

    first.wait_destroyed();
    second.wait_destroyed();
    REQUIRE(first.await_resume() == 17);
    REQUIRE(second.await_resume() == 17);
    REQUIRE(first_observed.load(std::memory_order_acquire));
    REQUIRE(second_observed.load(std::memory_order_acquire));
    REQUIRE(sched.shutdown());
}

TEST_CASE("runtime cancellation does not cross an independent spawn boundary",
          "[task][spawn][join_handle][cancellation_context]") {
    scheduler sched(2);
    sched.start();

    std::optional<join_handle<int>> child;
    std::atomic<bool> child_started{false};
    std::atomic<bool> child_observed{false};
    std::atomic<bool> parent_started{false};
    elio::sync::event release_parent;
    auto parent = sched.go_joinable(
        spawn_independent_child, &sched, &child, &child_started,
        &child_observed, &parent_started, &release_parent);

    const bool both_started =
        wait_for_flag(parent_started) && wait_for_flag(child_started);
    if (!both_started) {
        release_parent.set();
        if (child) child->request_cancel();
        sched.shutdown_force();
    }
    REQUIRE(both_started);
    REQUIRE(child.has_value());

    parent.request_cancel();
    REQUIRE(parent.is_cancellation_requested());
    REQUIRE_FALSE(parent.is_ready());
    REQUIRE_FALSE(child->is_cancellation_requested());
    REQUIRE_FALSE(child_observed.load(std::memory_order_acquire));

    child->request_cancel();
    const bool child_ready = wait_until_ready(*child);
    if (!child_ready) {
        release_parent.set();
        sched.shutdown_force();
    }
    REQUIRE(child_ready);
    child->wait_destroyed();
    REQUIRE(child->await_resume() == 17);
    REQUIRE(child_observed.load(std::memory_order_acquire));

    release_parent.set();
    const bool parent_ready = wait_until_ready(parent);
    if (!parent_ready) {
        sched.shutdown_force();
    }
    REQUIRE(parent_ready);
    parent.wait_destroyed();
    parent.await_resume();
    REQUIRE(sched.shutdown());
}

TEST_CASE("explicit tokens remain independent cancellation boundaries",
          "[task][spawn][cancel_token][cancellation_context]") {
    scheduler sched(1);
    sched.start();

    cancel_source explicit_source;
    std::atomic<bool> started{false};
    std::atomic<bool> runtime_cancelled{true};
    auto joined = sched.go_joinable(
        wait_for_explicit_cancellation, explicit_source.get_token(),
        &started, &runtime_cancelled);

    const bool did_start = wait_for_flag(started);
    if (!did_start) {
        sched.shutdown_force();
    }
    REQUIRE(did_start);

    explicit_source.cancel();
    const bool ready = wait_until_ready(joined);
    if (!ready) {
        sched.shutdown_force();
    }
    REQUIRE(ready);

    joined.wait_destroyed();
    joined.await_resume();
    REQUIRE_FALSE(joined.is_cancellation_requested());
    REQUIRE_FALSE(runtime_cancelled.load(std::memory_order_acquire));
    REQUIRE(sched.shutdown());
}

TEST_CASE("runtime cancellation does not request an explicit token",
          "[task][spawn][cancel_token][cancellation_context]") {
    scheduler sched(1);
    sched.start();

    cancel_source explicit_source;
    std::atomic<bool> started{false};
    std::atomic<bool> runtime_cancelled{false};
    auto joined = sched.go_joinable(
        wait_for_explicit_cancellation, explicit_source.get_token(),
        &started, &runtime_cancelled);

    const bool did_start = wait_for_flag(started);
    if (!did_start) {
        sched.shutdown_force();
    }
    REQUIRE(did_start);

    joined.request_cancel();
    REQUIRE(joined.is_cancellation_requested());
    REQUIRE_FALSE(explicit_source.is_cancelled());
    REQUIRE_FALSE(joined.is_ready());

    explicit_source.cancel();
    const bool ready = wait_until_ready(joined);
    if (!ready) {
        sched.shutdown_force();
    }
    REQUIRE(ready);

    joined.wait_destroyed();
    joined.await_resume();
    REQUIRE(runtime_cancelled.load(std::memory_order_acquire));
    REQUIRE(sched.shutdown());
}

TEST_CASE("join handle cancellation remains valid after frame destruction",
          "[task][spawn][join_handle][cancellation_context][lifetime]") {
    scheduler sched(1);
    sched.start();

    auto joined = sched.go_joinable([]() -> task<int> { co_return 42; });
    const bool ready = wait_until_ready(joined);
    if (!ready) {
        sched.shutdown_force();
    }
    REQUIRE(ready);

    joined.wait_destroyed();
    REQUIRE(joined.await_resume() == 42);
    joined.request_cancel();
    REQUIRE(joined.is_cancellation_requested());
    REQUIRE(sched.shutdown());
}

TEST_CASE("join handle cancellation preserves callback exception semantics",
          "[task][spawn][join_handle][cancellation_context][exception]") {
    scheduler sched(1);
    sched.start();

    std::atomic<bool> started{false};
    auto joined = sched.go_joinable(
        throwing_cancellation_callback, &started);
    const bool did_start = wait_for_flag(started);
    if (!did_start) {
        sched.shutdown_force();
    }
    REQUIRE(did_start);

    std::string exception_message;
    try {
        joined.request_cancel();
    } catch (const std::runtime_error& e) {
        exception_message = e.what();
    }
    REQUIRE(exception_message == "runtime cancellation callback failed");
    const bool ready = wait_until_ready(joined);
    if (!ready) {
        sched.shutdown_force();
    }
    REQUIRE(ready);

    joined.wait_destroyed();
    joined.await_resume();
    REQUIRE(joined.is_cancellation_requested());
    REQUIRE(sched.shutdown());
}

TEST_CASE("join cancellation races safely with child completion and teardown",
          "[task][spawn][join_handle][cancellation_context][thread][lifetime]") {
    scheduler sched(2);
    sched.start();

    constexpr int iterations = 128;
    for (int i = 0; i < iterations; ++i) {
        std::atomic<bool> started{false};
        std::atomic<bool> release{false};
        auto joined = sched.go_joinable(
            complete_during_cancellation, &started, &release);

        REQUIRE(wait_for_flag(started));
        std::latch race_start(1);
        std::thread requester([&] {
            race_start.wait();
            joined.request_cancel();
        });

        release.store(true, std::memory_order_release);
        race_start.count_down();
        const bool ready = wait_until_ready(joined);
        requester.join();
        REQUIRE(ready);

        joined.wait_destroyed();
        const int result = joined.await_resume();
        REQUIRE((result == 0 || result == 1));
        REQUIRE(joined.is_cancellation_requested());
    }

    REQUIRE(sched.shutdown());
}
