#include <catch2/catch_test_macros.hpp>
#include <elio/coro/task.hpp>
#include <elio/runtime/scheduler.hpp>
#include <elio/runtime/spawn.hpp>

#include <atomic>
#include <chrono>
#include <coroutine>
#include <exception>
#include <latch>
#include <memory>
#include <stdexcept>
#include <thread>
#include <utility>
#include <vector>

#include "../test_main.cpp"

namespace {

using elio::coro::join_handle;
using elio::coro::task;
using elio::coro::detail::join_destroyed_awaitable;
using elio::coro::detail::join_state;
using elio::runtime::scheduler;
using elio::test::scaled_sec;

template<typename Predicate>
bool wait_until(Predicate&& predicate) {
    const auto deadline = std::chrono::steady_clock::now() + scaled_sec(5);
    while (!predicate() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::yield();
    }
    return predicate();
}

struct observer_frame {
    struct promise_type {
        std::exception_ptr failure;
        observer_frame get_return_object() noexcept {
            return observer_frame{
                std::coroutine_handle<promise_type>::from_promise(*this)};
        }
        std::suspend_always initial_suspend() noexcept { return {}; }
        std::suspend_always final_suspend() noexcept { return {}; }
        void return_void() noexcept {}
        void unhandled_exception() noexcept { failure = std::current_exception(); }
    };

    std::coroutine_handle<promise_type> handle;

    ~observer_frame() { if (handle) handle.destroy(); }
    observer_frame(observer_frame&& other) noexcept
        : handle(std::exchange(other.handle, {})) {}
    explicit observer_frame(std::coroutine_handle<promise_type> h) noexcept
        : handle(h) {}

    void start() { handle.resume(); }
    void abandon() { std::exchange(handle, {}).destroy(); }
};

observer_frame observe_manually(join_destroyed_awaitable observer,
                                 std::atomic<bool>& resumed) {
    co_await std::move(observer);
    resumed.store(true, std::memory_order_release);
}

struct teardown_probe {
    explicit teardown_probe(int& target) noexcept : value(target) {}
    int& value;
    ~teardown_probe() { value = 42; }
};

task<int> owned_result(std::unique_ptr<teardown_probe> probe, bool fail) {
    (void)probe;
    if (fail) throw std::runtime_error("owned result failed");
    co_return 7;
}

task<void> observe_on_worker(join_destroyed_awaitable observer,
                             std::atomic<bool>& resumed,
                             scheduler* expected_scheduler,
                             std::atomic<bool>& correct_domain) {
    co_await std::move(observer);
    correct_domain.store(scheduler::current() == expected_scheduler,
                         std::memory_order_release);
    resumed.store(true, std::memory_order_release);
}

struct destruction_pause_guard {
    ~destruction_pause_guard() { release(); }
    void release() const noexcept {
        using namespace elio::coro::detail;
        pause_before_detached_frame_destroy_for_test.store(
            false, std::memory_order_release);
        pause_before_detached_frame_destroy_for_test.notify_all();
    }
};

} // namespace

TEST_CASE("async destruction observation preserves typed results and errors",
          "[task][join_handle][destroyed_async]") {
    auto state = std::make_shared<join_state<int>>();
    join_handle<int> handle(state);
    SECTION("value") { state->set_value(7); }
    SECTION("exception") {
        state->set_exception(std::make_exception_ptr(std::runtime_error("failure")));
    }
    state->mark_destroyed();
    std::atomic<bool> resumed{false};
    auto observer = observe_manually(handle.wait_destroyed_async(), resumed);
    observer.start();

    REQUIRE(resumed.load(std::memory_order_acquire));
    REQUIRE_FALSE(observer.handle.promise().failure);
    REQUIRE_FALSE(state->has_async_destruction_waiters_for_test());
    if (state->exception_) {
        REQUIRE_THROWS_AS(handle.await_resume(), std::runtime_error);
    } else {
        REQUIRE(handle.await_resume() == 7);
    }
}

TEST_CASE("async destruction observation closes the ready to suspend race",
          "[task][join_handle][destroyed_async][race]") {
    auto state = std::make_shared<join_state<void>>();
    join_handle<void> handle(state);
    auto observer = handle.wait_destroyed_async();
    REQUIRE_FALSE(observer.await_ready());
    state->mark_destroyed();
    REQUIRE_FALSE(observer.await_suspend(std::noop_coroutine()));
    observer.await_resume();
    REQUIRE_FALSE(state->has_async_destruction_waiters_for_test());
}

TEST_CASE("pending async destruction observation requires a scheduler",
          "[task][join_handle][destroyed_async][contract]") {
    REQUIRE(scheduler::current() == nullptr);
    auto state = std::make_shared<join_state<void>>();
    join_handle<void> handle(state);
    std::atomic<bool> resumed{false};
    auto observer = observe_manually(handle.wait_destroyed_async(), resumed);
    observer.start();
    REQUIRE_FALSE(resumed.load(std::memory_order_acquire));
    REQUIRE(observer.handle.promise().failure);
    REQUIRE_THROWS_AS(std::rethrow_exception(observer.handle.promise().failure),
                      std::logic_error);
    REQUIRE_FALSE(state->has_async_destruction_waiters_for_test());
}

TEST_CASE("destruction publication closes concurrent observer installation",
          "[task][join_handle][destroyed_async][race][installation]") {
    using namespace elio::coro::detail;
    auto state = std::make_shared<join_state<void>>();
    join_destroyed_observer_install_paused_for_test.store(false, std::memory_order_release);
    pause_join_destroyed_observer_install_for_test.store(true, std::memory_order_release);
    destruction_waiters* installed = nullptr;
    std::thread installer([&] { installed = state->async_destruction_waiters(); });
    const bool paused = wait_until([&] {
        return join_destroyed_observer_install_paused_for_test.load(std::memory_order_acquire);
    });
    state->mark_destroyed();
    pause_join_destroyed_observer_install_for_test.store(false, std::memory_order_release);
    pause_join_destroyed_observer_install_for_test.notify_all();
    installer.join();
    join_destroyed_observer_install_paused_for_test.store(false, std::memory_order_release);
    REQUIRE(paused);
    REQUIRE(installed == nullptr);
    REQUIRE(state->is_destroyed());
    REQUIRE_FALSE(state->has_async_destruction_waiters_for_test());
    REQUIRE(state->async_destruction_waiters() == nullptr);
}

TEST_CASE("concurrent async observer installation retains one live list",
          "[task][join_handle][destroyed_async][race][installation]") {
    auto state = std::make_shared<join_state<void>>();
    constexpr size_t count = 8;
    std::latch start(1);
    std::vector<elio::coro::detail::destruction_waiters*> installed(count);
    std::vector<std::thread> installers;
    for (size_t i = 0; i < count; ++i) {
        installers.emplace_back([&, i] {
            start.wait();
            installed[i] = state->async_destruction_waiters();
        });
    }
    start.count_down();
    for (auto& installer : installers) installer.join();
    REQUIRE(installed.front() != nullptr);
    for (auto* list : installed) REQUIRE(list == installed.front());
    REQUIRE_FALSE(state->is_destroyed());
    state->mark_destroyed();
    state->wait_destroyed();
    REQUIRE(state->is_destroyed());
    REQUIRE(state->async_destruction_waiters() == installed.front());
}

TEST_CASE("async destruction observers own state independently of join handles",
          "[task][join_handle][destroyed_async][ownership]") {
    scheduler sched(1);
    sched.start();
    auto state = std::make_shared<join_state<void>>();
    std::weak_ptr<join_state<void>> retained_state = state;
    auto observer = [&] {
        join_handle<void> handle(state);
        auto wait = handle.wait_destroyed_async();
        auto moved = std::move(handle);
        return wait;
    }();
    std::atomic<bool> resumed{false};
    auto frame = observe_manually(std::move(observer), resumed);
    frame.start();
    REQUIRE_FALSE(resumed.load(std::memory_order_acquire));
    state.reset();
    REQUIRE_FALSE(retained_state.expired());
    {
        auto publication = retained_state.lock();
        publication->mark_destroyed();
    }
    const bool returned = wait_until([&] {
        return resumed.load(std::memory_order_acquire);
    });
    const bool stopped = sched.shutdown(scaled_sec(5));
    REQUIRE(returned);
    REQUIRE(stopped);
    REQUIRE_FALSE(frame.handle.promise().failure);
    frame.abandon();
    REQUIRE(retained_state.expired());
}

TEST_CASE("async destruction wakes every observer without consuming results",
          "[task][join_handle][destroyed_async][multi_waiter]") {
    scheduler sched(2);
    sched.start();
    auto state = std::make_shared<join_state<int>>();
    join_handle<int> handle(state);
    constexpr size_t count = 8;
    std::atomic<size_t> resumed{0};
    std::vector<join_handle<void>> observers;
    for (size_t i = 0; i < count; ++i) {
        auto wait = handle.wait_destroyed_async();
        observers.push_back(sched.go_joinable(
            [wait = std::move(wait), &resumed]() mutable -> task<void> {
                co_await std::move(wait);
                resumed.fetch_add(1, std::memory_order_release);
            }));
    }
    const bool registered = wait_until([&] {
        return state->async_destruction_waiters()->pending_count_for_test() == count;
    });
    std::atomic<bool> blocking_returned{false};
    const auto prior_waits = elio::coro::detail::join_destroyed_wait_count_for_test.load(
        std::memory_order_acquire);
    std::thread blocking_waiter([&] {
        handle.wait_destroyed();
        blocking_returned.store(true, std::memory_order_release);
    });
    const bool blocking_registered = wait_until([&] {
        return elio::coro::detail::join_destroyed_wait_count_for_test.load(
                   std::memory_order_acquire) > prior_waits;
    });
    const auto before = resumed.load(std::memory_order_acquire);
    state->set_value(7);
    state->mark_destroyed();
    state->mark_destroyed();
    blocking_waiter.join();
    const bool returned = wait_until([&] {
        return resumed.load(std::memory_order_acquire) == count;
    });
    const bool stopped = sched.shutdown(scaled_sec(5));
    REQUIRE(registered);
    REQUIRE(blocking_registered);
    REQUIRE(before == 0);
    REQUIRE(returned);
    REQUIRE(stopped);
    REQUIRE(blocking_returned.load(std::memory_order_acquire));
    REQUIRE(handle.await_resume() == 7);
    REQUIRE(state->async_destruction_waiters()->pending_count_for_test() == 0);
    for (auto& observer : observers) {
        REQUIRE(observer.is_destroyed());
        REQUIRE_NOTHROW(observer.await_resume());
    }
}

TEST_CASE("abandoned async destruction observations cannot receive stale wakes",
          "[task][join_handle][destroyed_async][abandon]") {
    scheduler sched(1);
    sched.start();
    auto state = std::make_shared<join_state<void>>();
    join_handle<void> handle(state);
    std::atomic<bool> resumed{false};
    auto frame = observe_manually(handle.wait_destroyed_async(), resumed);
    frame.start();
    REQUIRE(state->async_destruction_waiters()->pending_count_for_test() == 1);
    frame.abandon();
    REQUIRE(state->async_destruction_waiters()->pending_count_for_test() == 0);
    state->mark_destroyed();
    REQUIRE(sched.shutdown(scaled_sec(5)));
    REQUIRE_FALSE(resumed.load(std::memory_order_acquire));
}

TEST_CASE("async destruction selected wake can be abandoned before claim",
          "[task][join_handle][destroyed_async][abandon][race]") {
    using namespace elio::coro::detail;
    scheduler sched(1);
    sched.start();
    auto state = std::make_shared<join_state<void>>();
    join_handle<void> handle(state);
    std::atomic<bool> resumed{false};
    auto frame = observe_manually(handle.wait_destroyed_async(), resumed);
    frame.start();
    completion_wake_claim_paused_for_test.store(false, std::memory_order_release);
    pause_before_completion_wake_claim_for_test.store(true, std::memory_order_release);
    std::thread producer([&] { state->mark_destroyed(); });
    const bool selected = wait_until([&] {
        return completion_wake_claim_paused_for_test.load(std::memory_order_acquire);
    });
    if (selected) frame.abandon();
    pause_before_completion_wake_claim_for_test.store(false, std::memory_order_release);
    pause_before_completion_wake_claim_for_test.notify_all();
    producer.join();
    const bool stopped = sched.shutdown(scaled_sec(5));
    completion_wake_claim_paused_for_test.store(false, std::memory_order_release);
    REQUIRE(selected);
    REQUIRE(stopped);
    REQUIRE_FALSE(resumed.load(std::memory_order_acquire));
}

TEST_CASE("async destruction does not return at result readiness",
          "[task][join_handle][destroyed_async][lifecycle]") {
    using namespace elio::coro::detail;
    bool fail = false;
    bool wrapped = false;
    SECTION("direct parameters") {
        SECTION("success") {}
        SECTION("exception") { fail = true; }
    }
    SECTION("callable captures") {
        wrapped = true;
        SECTION("success") {}
        SECTION("exception") { fail = true; }
    }
    scheduler sched(2);
    destruction_pause_guard pause;
    detached_frame_destroy_paused_for_test.store(false, std::memory_order_release);
    pause_before_detached_frame_destroy_for_test.store(true, std::memory_order_release);
    sched.start();
    int teardown_value = 0;
    auto child = [&] {
        if (wrapped) {
            return sched.go_joinable_to(
                0, [probe = std::make_unique<teardown_probe>(teardown_value), fail]()
                    -> task<int> {
                    (void)probe;
                    if (fail) throw std::runtime_error("owned result failed");
                    co_return 7;
                });
        }
        return sched.go_joinable_to(
            0, owned_result(std::make_unique<teardown_probe>(teardown_value), fail));
    }();
    const bool paused = wait_until([&] {
        return detached_frame_destroy_paused_for_test.load(std::memory_order_acquire);
    });
    std::atomic<bool> resumed{false};
    std::atomic<bool> domain{false};
    destruction_waiters::registered_count_for_test.store(0, std::memory_order_release);
    auto observer = sched.go_joinable_to(
        1, observe_on_worker(child.wait_destroyed_async(), resumed, &sched, domain));
    const bool registered = wait_until([&] {
        return destruction_waiters::registered_count_for_test.load(
                   std::memory_order_acquire) == 1;
    });
    const bool ready = child.is_ready();
    const bool destroyed = child.is_destroyed();
    const int before = teardown_value;
    const bool returned_before = resumed.load(std::memory_order_acquire);
    pause.release();
    const bool returned = wait_until([&] { return resumed.load(std::memory_order_acquire); });
    const bool stopped = sched.shutdown(scaled_sec(5));
    REQUIRE(paused);
    REQUIRE(registered);
    REQUIRE(ready);
    REQUIRE_FALSE(destroyed);
    REQUIRE(before == 0);
    REQUIRE_FALSE(returned_before);
    REQUIRE(returned);
    REQUIRE(stopped);
    REQUIRE(domain.load(std::memory_order_acquire));
    REQUIRE(teardown_value == 42);
    if (fail) {
        REQUIRE_THROWS_AS(child.await_resume(), std::runtime_error);
    } else {
        REQUIRE(child.await_resume() == 7);
    }
    REQUIRE_NOTHROW(observer.await_resume());
}

TEST_CASE("async destruction observers retain their scheduler domain",
          "[task][join_handle][destroyed_async][scheduler]") {
    scheduler child_scheduler(1);
    scheduler observer_scheduler(1);
    child_scheduler.start();
    observer_scheduler.start();
    auto state = std::make_shared<join_state<void>>();
    join_handle<void> child(state);
    std::atomic<bool> resumed{false};
    std::atomic<bool> correct_domain{false};
    auto observer = observer_scheduler.go_joinable(
        observe_on_worker(child.wait_destroyed_async(), resumed,
                          &observer_scheduler, correct_domain));
    const bool registered = wait_until([&] {
        return state->async_destruction_waiters()->pending_count_for_test() == 1;
    });
    auto publisher = child_scheduler.go_joinable([state]() -> task<void> {
        state->mark_destroyed();
        co_return;
    });
    const bool returned = wait_until([&] { return resumed.load(std::memory_order_acquire); });
    const bool child_stopped = child_scheduler.shutdown(scaled_sec(5));
    const bool observer_stopped = observer_scheduler.shutdown(scaled_sec(5));
    REQUIRE(registered);
    REQUIRE(returned);
    REQUIRE(child_stopped);
    REQUIRE(observer_stopped);
    REQUIRE(correct_domain.load(std::memory_order_acquire));
    REQUIRE_NOTHROW(observer.await_resume());
    REQUIRE_NOTHROW(publisher.await_resume());
}

TEST_CASE("rejected spawn destruction is immediately observable asynchronously",
          "[task][join_handle][destroyed_async][rejection]") {
    scheduler sched(1);
    int teardown_value = 0;
    auto child = sched.go_joinable(owned_result(
        std::make_unique<teardown_probe>(teardown_value), false));
    std::atomic<bool> resumed{false};
    auto observer = observe_manually(child.wait_destroyed_async(), resumed);
    observer.start();
    REQUIRE(resumed.load(std::memory_order_acquire));
    REQUIRE_FALSE(observer.handle.promise().failure);
    REQUIRE(teardown_value == 42);
    REQUIRE_THROWS_AS(child.await_resume(), std::logic_error);
}
