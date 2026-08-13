#include <catch2/catch_test_macros.hpp>
#include <elio/coro/frame.hpp>
#include <elio/coro/task.hpp>
#include <elio/coro/this_coro.hpp>
#include <elio/io/io_context_identity.hpp>
#include <elio/io/io_operation_guard.hpp>
#include <elio/runtime/scheduler.hpp>
#include <elio/runtime/spawn.hpp>
#include <atomic>
#include <chrono>
#include <coroutine>
#include <memory>
#include <optional>
#include <thread>

using namespace elio::coro;
using namespace elio::runtime;

namespace {

template<typename T>
auto get_handle(task<T>& value) {
    return elio::coro::detail::task_access::handle(value);
}

template<typename T>
void resume_in_frame(task<T>& value) {
    auto handle = get_handle(value);
    elio::coro::detail::frame_context_scope scope(
        std::addressof(handle.promise()));
    handle.resume();
}

task<void> context_noop() {
    co_return;
}

task<int> context_value() {
    co_return 42;
}

task<void> record_current_context(
    std::shared_ptr<task_execution_context>* observed) {
    auto* frame = promise_base::current_frame();
    *observed = frame ? frame->execution_context() : nullptr;
    co_return;
}

task<void> record_direct_contexts(
    std::shared_ptr<task_execution_context>* child) {
    co_await record_current_context(child);
}

task<void> direct_context_chain(size_t remaining) {
    if (remaining != 0) {
        co_await direct_context_chain(remaining - 1);
    }
}

task<void> create_unstarted_child(
    std::optional<task<void>>* output,
    std::shared_ptr<task_execution_context>* child_context) {
    output->emplace(record_current_context(child_context));
    co_return;
}

task<void> retain_frame_marker(std::shared_ptr<int> marker) {
    (void)marker;
    co_return;
}

task<void> create_unstarted_marked_child(
    std::optional<task<void>>* output, std::shared_ptr<int> marker) {
    output->emplace(retain_frame_marker(std::move(marker)));
    co_return;
}

task<void> create_unstarted_direct_chain(
    std::optional<task<void>>* output,
    std::shared_ptr<task_execution_context>* nested_context) {
    output->emplace(record_direct_contexts(nested_context));
    co_return;
}

task<void> await_moved_child(
    std::optional<task<void>>* input) {
    co_await std::move(input->value());
}

task<void> spawn_recorded_child(
    std::shared_ptr<task_execution_context>* parent_context,
    std::shared_ptr<task_execution_context>* child_context,
    std::shared_ptr<task_execution_context>* join_context) {
    *parent_context = promise_base::current_frame()->execution_context();
    auto child = record_current_context(child_context);
    auto joined = elio::spawn(std::move(child));
    *join_context =
        elio::coro::detail::task_access::get_join_execution_context(joined);
    co_await joined;
}

task<void> capture_child_control(cancel_token* token,
                                 std::weak_ptr<task_execution_context>* weak) {
    auto* frame = promise_base::current_frame();
    auto context = frame->execution_context();
    *token = context->get_cancel_token();
    *weak = context;
    co_return;
}

task<void> await_captured_child(
    cancel_token* token, std::weak_ptr<task_execution_context>* weak) {
    co_await capture_child_control(token, weak);
}

task<void> retain_completed_named_child(
    cancel_token* token, std::weak_ptr<task_execution_context>* weak,
    bool* child_completed) {
    auto child = capture_child_control(token, weak);
    co_await child;
    *child_completed = true;
    co_await std::suspend_always{};
}

task<void> capture_suspended_child(
    cancel_token* token, std::weak_ptr<task_execution_context>* weak) {
    auto* frame = promise_base::current_frame();
    auto context = frame->execution_context();
    *token = context->get_cancel_token();
    *weak = context;
    co_await std::suspend_always{};
}

task<void> await_suspended_child(
    cancel_token* token, std::weak_ptr<task_execution_context>* weak) {
    co_await capture_suspended_child(token, weak);
}

struct capture_handle_awaitable final {
    std::coroutine_handle<>* output;

    bool await_ready() const noexcept { return false; }
    void await_suspend(std::coroutine_handle<> handle) const noexcept {
        *output = handle;
    }
    void await_resume() const noexcept {}
};

task<void> expose_child_token_and_suspend(
    std::coroutine_handle<>* child_handle, cancel_token* child_token) {
    *child_token = this_coro::cancel_token();
    co_await capture_handle_awaitable{child_handle};
}

bool wait_for_flag(const std::atomic<bool>& flag) {
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!flag.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return flag.load(std::memory_order_acquire);
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

TEST_CASE("runtime task control state uses one shared allocation",
          "[task][execution_context][cancellation][allocation]") {
    elio::coro::detail::separate_cancel_state_allocations_for_test.store(
        0, std::memory_order_relaxed);

    std::weak_ptr<task_execution_context> weak_context;
    cancel_token retained_token;
    {
        auto value = context_noop();
        auto context = get_handle(value).promise().execution_context();
        weak_context = context;
        retained_token = context->get_cancel_token();

        REQUIRE(context);
        REQUIRE_FALSE(retained_token.is_cancelled());
        REQUIRE(
            elio::coro::detail::separate_cancel_state_allocations_for_test.load(
                std::memory_order_relaxed) == 0);
    }

    // The aliasing token shares the task-context control block, so the
    // embedded cancel_state remains valid after the coroutine frame is gone.
    REQUIRE_FALSE(weak_context.expired());
    REQUIRE_FALSE(retained_token.is_cancelled());
    retained_token = {};
    REQUIRE(weak_context.expired());

    // Source-compatible standalone construction keeps its independent state.
    auto standalone = std::make_shared<task_execution_context>();
    auto standalone_token = standalone->get_cancel_token();
    REQUIRE(
        elio::coro::detail::separate_cancel_state_allocations_for_test.load(
            std::memory_order_relaxed) == 1);
    elio::coro::detail::join_state<void> default_join_state;
    REQUIRE_FALSE(default_join_state.is_completed());
    REQUIRE(
        elio::coro::detail::separate_cancel_state_allocations_for_test.load(
            std::memory_order_relaxed) == 1);
    standalone->request_cancel();
    REQUIRE(standalone_token.is_cancelled());
}

TEST_CASE("direct Elio awaits share one execution context allocation",
          "[task][execution_context][cancellation][allocation]") {
    elio::coro::detail::task_execution_context_allocations_for_test.store(
        0, std::memory_order_relaxed);

    std::shared_ptr<task_execution_context> parent_context;
    std::shared_ptr<task_execution_context> child_context;
    auto root = record_direct_contexts(&child_context);
    parent_context = get_handle(root).promise().execution_context();

    REQUIRE(
        elio::coro::detail::task_execution_context_allocations_for_test.load(
            std::memory_order_relaxed) == 1);
    resume_in_frame(root);

    REQUIRE(get_handle(root).done());
    REQUIRE(parent_context);
    REQUIRE(child_context == parent_context);
    REQUIRE(
        elio::coro::detail::task_execution_context_allocations_for_test.load(
            std::memory_order_relaxed) == 1);

    elio::coro::detail::task_execution_context_allocations_for_test.store(
        0, std::memory_order_relaxed);
    auto chain = direct_context_chain(8);
    resume_in_frame(chain);
    REQUIRE(get_handle(chain).done());
    REQUIRE(
        elio::coro::detail::task_execution_context_allocations_for_test.load(
            std::memory_order_relaxed) == 1);
}

TEST_CASE("moved lazy task binds to its actual Elio awaiter context",
          "[task][execution_context][ownership][allocation]") {
    elio::coro::detail::task_execution_context_allocations_for_test.store(
        0, std::memory_order_relaxed);

    std::optional<task<void>> deferred_child;
    std::shared_ptr<task_execution_context> child_context;
    auto producer = create_unstarted_child(&deferred_child, &child_context);
    auto producer_context = get_handle(producer).promise().execution_context();
    resume_in_frame(producer);
    REQUIRE(get_handle(producer).done());
    REQUIRE(deferred_child.has_value());

    auto consumer = await_moved_child(&deferred_child);
    auto awaiter_context = get_handle(consumer).promise().execution_context();
    resume_in_frame(consumer);

    REQUIRE(get_handle(consumer).done());
    REQUIRE(awaiter_context);
    REQUIRE(child_context == awaiter_context);
    REQUIRE(child_context != producer_context);
    REQUIRE(
        elio::coro::detail::task_execution_context_allocations_for_test.load(
            std::memory_order_relaxed) == 2);
}

TEST_CASE("raw-resumed deferred task materializes a root context on direct await",
          "[task][execution_context][ownership][allocation]") {
    elio::coro::detail::task_execution_context_allocations_for_test.store(
        0, std::memory_order_relaxed);

    std::optional<task<void>> deferred_root;
    std::shared_ptr<task_execution_context> nested_context;
    auto producer = create_unstarted_direct_chain(
        &deferred_root, &nested_context);
    resume_in_frame(producer);
    REQUIRE(get_handle(producer).done());
    REQUIRE(deferred_root.has_value());
    REQUIRE_FALSE(
        get_handle(deferred_root.value()).promise().execution_context());

    resume_in_frame(deferred_root.value());

    REQUIRE(get_handle(deferred_root.value()).done());
    auto root_context =
        get_handle(deferred_root.value()).promise().execution_context();
    REQUIRE(root_context);
    REQUIRE(nested_context == root_context);
    REQUIRE(
        elio::coro::detail::task_execution_context_allocations_for_test.load(
            std::memory_order_relaxed) == 2);
}

TEST_CASE("raw scheduler spawn keeps exception-safe frame ownership",
          "[task][execution_context][scheduler][ownership][allocation]") {
    scheduler sched(1);
    sched.start();

    std::optional<task<void>> borrowed_child;
    auto borrowed_marker = std::make_shared<int>(1);
    std::weak_ptr<int> borrowed_observer = borrowed_marker;
    auto borrowed_producer = create_unstarted_marked_child(
        &borrowed_child, std::move(borrowed_marker));
    resume_in_frame(borrowed_producer);
    REQUIRE(borrowed_child.has_value());
    REQUIRE_FALSE(get_handle(borrowed_child.value())
                      .promise()
                      .execution_context());

    auto borrowed_handle = elio::coro::detail::task_access::release(
        std::move(borrowed_child.value()));
    elio::coro::detail::fail_next_task_execution_context_allocation_for_test
        .store(true, std::memory_order_release);
    REQUIRE_THROWS_AS(sched.try_spawn(borrowed_handle), std::bad_alloc);
    REQUIRE_FALSE(borrowed_observer.expired());
    borrowed_handle.destroy();
    REQUIRE(borrowed_observer.expired());

    std::optional<task<void>> owned_child;
    auto owned_marker = std::make_shared<int>(1);
    std::weak_ptr<int> owned_observer = owned_marker;
    auto owned_producer = create_unstarted_marked_child(
        &owned_child, std::move(owned_marker));
    resume_in_frame(owned_producer);
    REQUIRE(owned_child.has_value());

    auto owned_handle = elio::coro::detail::task_access::release(
        std::move(owned_child.value()));
    elio::coro::detail::fail_next_task_execution_context_allocation_for_test
        .store(true, std::memory_order_release);
    REQUIRE_THROWS_AS(sched.spawn(owned_handle), std::bad_alloc);
    REQUIRE(owned_observer.expired());

    REQUIRE(sched.shutdown());
}

TEST_CASE("spawned children materialize independent execution contexts",
          "[task][execution_context][spawn][allocation]") {
    scheduler sched(1);
    sched.start();
    elio::coro::detail::task_execution_context_allocations_for_test.store(
        0, std::memory_order_relaxed);

    std::shared_ptr<task_execution_context> parent_context;
    std::shared_ptr<task_execution_context> child_context;
    std::shared_ptr<task_execution_context> join_context;
    auto root = spawn_recorded_child(
        &parent_context, &child_context, &join_context);
    auto joined = sched.go_joinable(std::move(root));
    joined.wait_destroyed();
    REQUIRE_NOTHROW(joined.await_resume());

    REQUIRE(parent_context);
    REQUIRE(child_context);
    REQUIRE(parent_context != child_context);
    REQUIRE(join_context == child_context);
    REQUIRE(
        elio::coro::detail::task_execution_context_allocations_for_test.load(
            std::memory_order_relaxed) == 2);
    REQUIRE(sched.shutdown());
}

TEST_CASE("direct await chains share cancellation context without a cycle",
          "[task][execution_context][cancellation][ownership]") {
    cancel_token child_token;
    std::weak_ptr<task_execution_context> weak_parent;
    std::weak_ptr<task_execution_context> weak_child;
    {
        auto parent = await_suspended_child(&child_token, &weak_child);
        auto parent_context = get_handle(parent).promise().execution_context();
        weak_parent = parent_context;

        get_handle(parent).resume();
        REQUIRE_FALSE(get_handle(parent).done());
        REQUIRE_FALSE(weak_child.expired());
        auto child_context = weak_child.lock();
        REQUIRE(child_context == parent_context);

        parent_context->request_cancel();
        REQUIRE(child_token.is_cancelled());
        child_token = {};
        child_context.reset();
    }

    REQUIRE(weak_parent.expired());
    REQUIRE(weak_child.expired());
}

TEST_CASE("escaped direct-child tokens retain the logical vthread context",
          "[task][execution_context][cancellation][ownership]") {
    cancel_token escaped_token;
    std::weak_ptr<task_execution_context> weak_parent;
    std::weak_ptr<task_execution_context> weak_child;
    {
        auto parent = await_captured_child(&escaped_token, &weak_child);
        auto parent_context = get_handle(parent).promise().execution_context();
        weak_parent = parent_context;

        get_handle(parent).resume();
        REQUIRE(get_handle(parent).done());
        REQUIRE_FALSE(weak_child.expired());

        REQUIRE(weak_child.lock() == parent_context);
        // The transparent child token names the surrounding logical vthread,
        // so cancellation remains observable after the child frame completes.
        parent_context->request_cancel();
        REQUIRE(escaped_token.is_cancelled());
    }

    REQUIRE_FALSE(weak_parent.expired());
    REQUIRE_FALSE(weak_child.expired());

    escaped_token = {};
    REQUIRE(weak_parent.expired());
    REQUIRE(weak_child.expired());
}

TEST_CASE("completed named direct children keep logical vthread tokens valid",
          "[task][execution_context][cancellation][ownership]") {
    cancel_token escaped_token;
    std::weak_ptr<task_execution_context> weak_parent;
    std::weak_ptr<task_execution_context> weak_child;
    bool child_completed = false;
    {
        auto parent = retain_completed_named_child(
            &escaped_token, &weak_child, &child_completed);
        auto parent_context = get_handle(parent).promise().execution_context();
        weak_parent = parent_context;

        get_handle(parent).resume();
        REQUIRE(child_completed);
        REQUIRE_FALSE(get_handle(parent).done());
        // The named child frame is still owned by the suspended parent.
        REQUIRE_FALSE(weak_child.expired());

        REQUIRE(weak_child.lock() == parent_context);
        // The child frame is transparent: its token remains the parent/root
        // logical-vthread token even after this named child has completed.
        parent_context->request_cancel();
        REQUIRE(escaped_token.is_cancelled());
    }

    REQUIRE_FALSE(weak_parent.expired());
    REQUIRE_FALSE(weak_child.expired());
    escaped_token = {};
    REQUIRE(weak_parent.expired());
    REQUIRE(weak_child.expired());
}

TEST_CASE("child completion never waits for a parent cancellation callback",
          "[task][execution_context][cancellation][thread][ownership]") {
    std::coroutine_handle<> child_handle;
    cancel_token child_token;
    std::atomic<bool> callback_started{false};
    std::atomic<bool> release_callback{false};
    std::atomic<bool> completion_returned{false};

    auto parent_context = std::make_shared<task_execution_context>();
    auto child = expose_child_token_and_suspend(&child_handle, &child_token);
    get_handle(child).promise().link_parent_cancellation(
        parent_context->get_cancel_token());
    resume_in_frame(child);
    // Raw test-only resume bypasses the scheduler's frame-context guard.
    promise_base::set_current_frame(nullptr);
    REQUIRE(child_handle);

    // The test owns this ordinary registration outside the child frame so
    // child completion isolates the task-parent link's non-blocking teardown.
    auto blocking_registration = child_token.on_cancel([&] {
        callback_started.store(true, std::memory_order_release);
        while (!release_callback.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
    });

    std::thread cancel_request([&] { parent_context->request_cancel(); });
    REQUIRE(wait_for_flag(callback_started));

    std::thread complete_child([&] {
        child_handle.resume();
        completion_returned.store(true, std::memory_order_release);
    });
    const bool completed_without_wait = wait_for_flag(completion_returned);

    // On failure, release the callback so both threads can be joined and the
    // test reports normally instead of hanging the entire suite.
    release_callback.store(true, std::memory_order_release);
    complete_child.join();
    cancel_request.join();

    REQUIRE(completed_without_wait);
    REQUIRE(get_handle(child).done());
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
