#include <catch2/catch_test_macros.hpp>
#include <elio/coro/frame.hpp>
#include <elio/coro/task.hpp>
#include <elio/io/io_context_identity.hpp>
#include <elio/io/io_operation_guard.hpp>
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

TEST_CASE("I/O pins override but do not rewrite user affinity",
          "[task][execution_context][io][affinity]") {
    auto context = std::make_shared<task_execution_context>();
    auto first_identity =
        std::make_shared<elio::io::detail::io_context_identity>(2, 41);
    auto other_identity =
        std::make_shared<elio::io::detail::io_context_identity>(3, 42);

    context->set_user_affinity(7);
    {
        elio::io::detail::io_operation_guard outer(context, first_identity);
        REQUIRE(context->has_active_io_pin());
        REQUIRE(context->active_io_pin_count() == 1);
        REQUIRE(context->io_owner_worker() == 2);
        REQUIRE(context->io_context_generation() == 41);
        REQUIRE(context->effective_affinity() == 2);
        REQUIRE(first_identity->active_pins.load() == 1);

        context->clear_user_affinity();
        REQUIRE(context->user_affinity() == NO_AFFINITY);
        REQUIRE(context->effective_affinity() == 2);

        {
            elio::io::detail::io_operation_guard nested(context, first_identity);
            REQUIRE(context->active_io_pin_count() == 2);
            REQUIRE(first_identity->active_pins.load() == 2);
            REQUIRE_THROWS_AS(
                elio::io::detail::io_operation_guard(context, other_identity),
                std::logic_error);
            REQUIRE(other_identity->active_pins.load() == 0);
        }

        REQUIRE(context->active_io_pin_count() == 1);
        REQUIRE(first_identity->active_pins.load() == 1);
    }

    REQUIRE_FALSE(context->has_active_io_pin());
    REQUIRE(context->active_io_pin_count() == 0);
    REQUIRE(context->effective_affinity() == NO_AFFINITY);
    REQUIRE(first_identity->active_pins.load() == 0);
}

TEST_CASE("I/O operation guard move and standalone accounting release once",
          "[task][execution_context][io][ownership]") {
    auto context = std::make_shared<task_execution_context>();
    auto worker_identity =
        std::make_shared<elio::io::detail::io_context_identity>(1, 9);

    elio::io::detail::io_operation_guard source(context, worker_identity);
    elio::io::detail::io_operation_guard moved(std::move(source));
    REQUIRE_FALSE(source.active());
    REQUIRE(moved.active());
    REQUIRE(context->active_io_pin_count() == 1);

    moved.release();
    moved.release();
    REQUIRE(context->active_io_pin_count() == 0);
    REQUIRE(worker_identity->active_pins.load() == 0);

    auto standalone_identity =
        std::make_shared<elio::io::detail::io_context_identity>(
            elio::io::detail::NO_IO_CONTEXT_OWNER, 10);
    {
        elio::io::detail::io_operation_guard standalone(
            context, standalone_identity);
        REQUIRE(standalone.active());
        REQUIRE_FALSE(context->has_active_io_pin());
        REQUIRE(standalone_identity->active_pins.load() == 1);
    }
    REQUIRE(standalone_identity->active_pins.load() == 0);
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
