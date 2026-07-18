#include <catch2/catch_test_macros.hpp>
#include <elio/coro/frame.hpp>
#include <elio/coro/task.hpp>
#include <elio/runtime/scheduler.hpp>
#include <memory>

using namespace elio::coro;
using namespace elio::runtime;

namespace {

template<typename T>
auto get_handle(task<T>& value) {
    return elio::coro::detail::task_access::handle(value);
}

task<void> context_noop() {
    co_return;
}

task<int> context_value() {
    co_return 42;
}

} // namespace

TEST_CASE("promise runtime policy is stored in task execution context",
          "[task][execution_context][affinity]") {
    auto value = context_noop();
    auto& promise = get_handle(value).promise();
    auto context = promise.execution_context();

    REQUIRE(context);
    REQUIRE(context->user_affinity() == NO_AFFINITY);
    REQUIRE(context->effective_affinity() == NO_AFFINITY);
    REQUIRE_FALSE(context->is_worker_local());

    promise.set_affinity(3);
    promise.set_worker_local();
    REQUIRE(context->user_affinity() == 3);
    REQUIRE(context->effective_affinity() == 3);
    REQUIRE(context->is_worker_local());

    context->set_user_affinity(5);
    context->set_worker_local(false);
    REQUIRE(promise.affinity() == 5);
    REQUIRE(promise.effective_affinity() == 5);
    REQUIRE_FALSE(promise.is_worker_local());

    promise.clear_affinity();
    REQUIRE(context->user_affinity() == NO_AFFINITY);
}

TEST_CASE("task execution context survives task movement and frame destruction",
          "[task][execution_context][ownership]") {
    std::shared_ptr<task_execution_context> retained;
    std::weak_ptr<task_execution_context> weak;

    {
        auto original = context_noop();
        retained = get_handle(original).promise().execution_context();
        weak = retained;
        retained->set_user_affinity(7);

        auto moved = std::move(original);
        REQUIRE_FALSE(original.valid());
        REQUIRE(get_handle(moved).promise().execution_context() == retained);
    }

    REQUIRE_FALSE(weak.expired());
    REQUIRE(retained->user_affinity() == 7);
    retained.reset();
    REQUIRE(weak.expired());
}

TEST_CASE("frame lookup returns shared task execution context",
          "[task][execution_context][frame][debugger]") {
    auto value = context_noop();
    auto handle = get_handle(value);
    auto context = handle.promise().execution_context();

    REQUIRE(get_execution_context(handle.address()) == context);
    REQUIRE_FALSE(get_execution_context(nullptr));
}

TEST_CASE("join handle shares spawned promise execution context",
          "[task][execution_context][join_handle][scheduler]") {
    REQUIRE_THROWS_AS(
        elio::coro::detail::join_state<void>{
            std::shared_ptr<task_execution_context>{}},
        std::invalid_argument);

    scheduler sched(1);
    sched.start();

    std::shared_ptr<task_execution_context> observed;
    auto factory = [&]() -> task<int> {
        auto* frame = promise_base::current_frame();
        observed = frame ? frame->execution_context() : nullptr;
        return context_value();
    };

    auto joined = sched.go_joinable(factory);
    auto handle_context =
        elio::coro::detail::task_access::get_join_execution_context(joined);
    joined.wait_destroyed();

    REQUIRE(handle_context);
    REQUIRE(observed == handle_context);
    REQUIRE(joined.is_ready());
    REQUIRE(joined.await_resume() == 42);
    REQUIRE(sched.shutdown());
}
