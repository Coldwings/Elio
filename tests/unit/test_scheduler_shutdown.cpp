#include <catch2/catch_test_macros.hpp>
#include <elio/runtime/scheduler.hpp>
#include <elio/runtime/async_main.hpp>
#include <elio/coro/task.hpp>
#include <elio/coro/task_group.hpp>
#include <elio/sync/primitives.hpp>
#include <elio/time/timer.hpp>

#include <atomic>
#include <chrono>
#include <memory>
#include <stdexcept>
#include <thread>

#include "../test_main.cpp"  // For scaled_ms / scaled_sec

using namespace elio::runtime;
using namespace elio::coro;
using namespace elio::time;
using namespace elio::test;
using namespace std::chrono_literals;

namespace {

task<void> mark_after_sleep(std::chrono::milliseconds dur, std::atomic<bool>* flag) {
    co_await sleep_for(dur);
    flag->store(true, std::memory_order_release);
}

task<void> mark_after_sleep_counter(std::chrono::milliseconds dur, std::atomic<int>* counter) {
    co_await sleep_for(dur);
    counter->fetch_add(1, std::memory_order_acq_rel);
}

task<void> increment_only(std::atomic<int>* counter) {
    counter->fetch_add(1, std::memory_order_acq_rel);
    co_return;
}

task<void> increment_with_frame_marker(
    std::atomic<int>* counter,
    std::shared_ptr<int> frame_marker) {
    (void)frame_marker;
    counter->fetch_add(1, std::memory_order_acq_rel);
    co_return;
}

task<void> call_shutdown_from_worker(scheduler* sched,
                                     std::atomic<bool>* returned,
                                     std::atomic<bool>* drained) {
    bool result = sched->shutdown(scaled_ms(1000));
    drained->store(result, std::memory_order_release);
    returned->store(true, std::memory_order_release);
    co_return;
}

task<void> call_shutdown_force_from_worker(scheduler* sched,
                                           std::atomic<bool>* returned) {
    sched->shutdown_force();
    returned->store(true, std::memory_order_release);
    co_return;
}

bool wait_for_flag(std::atomic<bool>& flag, std::chrono::milliseconds timeout) {
    auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!flag.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return flag.load(std::memory_order_acquire);
}

// Suspends on an event until the test releases it. Used by tests that need a
// task to remain "in-flight and tracked" for an arbitrary, observer-controlled
// window (replaces timing-fragile sleep-based gates).
task<void> wait_on_event(elio::sync::event* gate, std::atomic<int>* counter) {
    co_await gate->wait();
    counter->fetch_add(1, std::memory_order_acq_rel);
}

task<void> wait_on_event_started(elio::sync::event* gate,
                                 std::atomic<bool>* started,
                                 std::atomic<int>* counter) {
    started->store(true, std::memory_order_release);
    co_await gate->wait();
    counter->fetch_add(1, std::memory_order_acq_rel);
}

task<void> spawn_group_after_event(
    elio::sync::event* gate,
    std::atomic<bool>* started,
    std::atomic<int>* child_runs,
    std::atomic<bool>* joined) {
    started->store(true, std::memory_order_release);
    co_await gate->wait();

    task_group group;
    group.spawn(increment_only, child_runs);
    co_await group.join();
    joined->store(true, std::memory_order_release);
}

task<void> spawn_blocked_group_after_event(
    elio::sync::event* start_gate,
    elio::sync::event* child_gate,
    std::atomic<bool>* started,
    std::atomic<bool>* child_started,
    std::atomic<int>* child_runs) {
    started->store(true, std::memory_order_release);
    co_await start_gate->wait();

    task_group group;
    group.spawn(wait_on_event_started, child_gate, child_started, child_runs);
    co_await group.join();
}

struct external_resume_slot {
    std::atomic<void*> address{nullptr};
};

struct external_suspend_awaiter {
    external_resume_slot* slot;

    [[nodiscard]] bool await_ready() const noexcept { return false; }

    void await_suspend(std::coroutine_handle<> handle) const noexcept {
        slot->address.store(handle.address(), std::memory_order_release);
    }

    void await_resume() const noexcept {}
};

task<void> resuspend_after_timer_cancel(
    std::chrono::milliseconds dur,
    external_resume_slot* slot,
    std::atomic<bool>* cancellation_resumed) {
    co_await sleep_for(dur);
    cancellation_resumed->store(true, std::memory_order_release);
    co_await external_suspend_awaiter{slot};
}

struct scheduler_test_hook_guard {
    ~scheduler_test_hook_guard() {
        elio::runtime::detail::pause_after_graceful_drain_for_test.store(
            false, std::memory_order_release);
        elio::runtime::detail::pause_after_graceful_drain_for_test.notify_all();
        elio::runtime::detail::graceful_drain_paused_for_test.store(
            false, std::memory_order_release);
        elio::runtime::detail::graceful_drain_rechecks_for_test.store(
            0, std::memory_order_release);
        elio::runtime::detail::graceful_admission_closed_for_test.store(
            false, std::memory_order_release);
        elio::runtime::detail::destroying_workers_for_test.store(
            false, std::memory_order_release);
        elio::runtime::detail::tracked_completions_during_worker_destroy_for_test.store(
            0, std::memory_order_release);
        elio::runtime::detail::worker_io_backend_for_test.store(
            elio::io::io_context::backend_type::auto_detect,
            std::memory_order_release);
    }
};

} // namespace

TEST_CASE("shutdown waits for tracked tasks suspended on I/O", "[scheduler][shutdown]") {
    scheduler sched(2);
    sched.start();

    std::atomic<bool> done_a{false};
    std::atomic<bool> done_b{false};
    std::atomic<bool> done_c{false};

    // All three tasks suspend on a timer (sleep_for goes through io_context).
    // Without graceful shutdown, the workers would stop while these are
    // suspended and they would never be resumed.
    sched.go(mark_after_sleep, scaled_ms(100), &done_a);
    sched.go(mark_after_sleep, scaled_ms(120), &done_b);
    sched.go(mark_after_sleep, scaled_ms(80), &done_c);

    // Give the spawns time to start and register their timers.
    std::this_thread::sleep_for(scaled_ms(20));

    bool drained = sched.shutdown(scaled_ms(2000));
    REQUIRE(drained);
    REQUIRE(done_a.load());
    REQUIRE(done_b.load());
    REQUIRE(done_c.load());
}

TEST_CASE("graceful shutdown closes initial admission before draining",
          "[scheduler][shutdown][admission][regression]") {
    scheduler_test_hook_guard hook_guard;
    elio::runtime::detail::graceful_admission_closed_for_test.store(
        false, std::memory_order_release);

    scheduler sched(1);
    sched.start();

    elio::sync::event gate;
    std::atomic<bool> accepted_started{false};
    std::atomic<int> accepted_completed{0};
    std::atomic<int> rejected_bodies{0};
    auto accepted = sched.go_joinable(
        wait_on_event_started, &gate, &accepted_started, &accepted_completed);
    const bool task_started = wait_for_flag(accepted_started, scaled_ms(2000));

    bool drained = false;
    std::thread shutdown_thread([&] {
        drained = sched.shutdown(scaled_ms(5000));
    });

    const bool admission_closed = wait_for_flag(
        elio::runtime::detail::graceful_admission_closed_for_test,
        scaled_ms(2000));

    bool raw_rejected = false;
    bool raw_frame_retained = false;
    bool raw_frame_released = false;
    bool owned_frame_released = false;
    bool joinable_rejected = false;
    bool pinned_joinable_rejected = false;

    if (admission_closed) {
        sched.go(increment_only, &rejected_bodies);
        sched.go_to(0, increment_only, &rejected_bodies);

        auto joined = sched.go_joinable(increment_only, &rejected_bodies);
        auto pinned_joined =
            sched.go_joinable_to(0, increment_only, &rejected_bodies);

        joined.wait_destroyed();
        pinned_joined.wait_destroyed();
        try {
            joined.await_resume();
        } catch (const std::logic_error&) {
            joinable_rejected = true;
        }
        try {
            pinned_joined.await_resume();
        } catch (const std::logic_error&) {
            pinned_joinable_rejected = true;
        }

        auto raw_marker = std::make_shared<int>(1);
        std::weak_ptr<int> raw_marker_observer = raw_marker;
        auto raw_task = increment_with_frame_marker(
            &rejected_bodies, std::move(raw_marker));
        auto raw_handle =
            elio::coro::detail::task_access::release(std::move(raw_task));
        raw_rejected = !sched.try_spawn(raw_handle);
        raw_frame_retained = !raw_marker_observer.expired();
        raw_handle.destroy();
        raw_frame_released = raw_marker_observer.expired();

        auto owned_marker = std::make_shared<int>(1);
        std::weak_ptr<int> owned_marker_observer = owned_marker;
        auto owned_task = increment_with_frame_marker(
            &rejected_bodies, std::move(owned_marker));
        auto owned_handle =
            elio::coro::detail::task_access::release(std::move(owned_task));
        sched.spawn_to(0, owned_handle);
        owned_frame_released = owned_marker_observer.expired();
    }

    gate.set();
    shutdown_thread.join();
    accepted.wait_destroyed();
    REQUIRE_NOTHROW(accepted.await_resume());

    REQUIRE(task_started);
    REQUIRE(admission_closed);
    REQUIRE(drained);
    REQUIRE(accepted_completed.load(std::memory_order_acquire) == 1);
    REQUIRE(rejected_bodies.load(std::memory_order_acquire) == 0);
    REQUIRE(raw_rejected);
    REQUIRE(raw_frame_retained);
    REQUIRE(raw_frame_released);
    REQUIRE(owned_frame_released);
    REQUIRE(joinable_rejected);
    REQUIRE(pinned_joinable_rejected);
}

TEST_CASE("graceful shutdown admits linked children of accepted tasks",
          "[scheduler][shutdown][admission][task_group][regression]") {
    scheduler_test_hook_guard hook_guard;
    elio::runtime::detail::graceful_admission_closed_for_test.store(
        false, std::memory_order_release);

    scheduler sched(1);
    sched.start();

    elio::sync::event gate;
    std::atomic<bool> root_started{false};
    std::atomic<int> child_runs{0};
    std::atomic<bool> group_joined{false};
    auto root = sched.go_joinable(
        spawn_group_after_event, &gate, &root_started, &child_runs,
        &group_joined);
    REQUIRE(wait_for_flag(root_started, scaled_ms(2000)));

    bool drained = false;
    std::thread shutdown_thread([&] {
        drained = sched.shutdown(scaled_ms(5000));
    });
    const bool admission_closed = wait_for_flag(
        elio::runtime::detail::graceful_admission_closed_for_test,
        scaled_ms(2000));

    bool external_group_rejected = false;
    if (admission_closed) {
        task_group external_group(sched);
        external_group.spawn(increment_only, &child_runs);
        external_group_rejected = !external_group.failures().empty();
    }

    gate.set();
    shutdown_thread.join();
    root.wait_destroyed();
    REQUIRE_NOTHROW(root.await_resume());

    REQUIRE(admission_closed);
    REQUIRE(drained);
    REQUIRE(external_group_rejected);
    REQUIRE(group_joined.load(std::memory_order_acquire));
    REQUIRE(child_runs.load(std::memory_order_acquire) == 1);
}

TEST_CASE("graceful shutdown rechecks children admitted after idle observation",
          "[scheduler][shutdown][admission][task_group][regression]") {
    scheduler_test_hook_guard hook_guard;
    elio::runtime::detail::pause_after_graceful_drain_for_test.store(
        true, std::memory_order_release);
    elio::runtime::detail::graceful_drain_paused_for_test.store(
        false, std::memory_order_release);
    elio::runtime::detail::graceful_drain_rechecks_for_test.store(
        0, std::memory_order_release);

    scheduler sched(1);
    sched.start();

    elio::sync::event start_gate;
    elio::sync::event child_gate;
    std::atomic<bool> root_started{false};
    std::atomic<bool> child_started{false};
    std::atomic<int> child_runs{0};

    auto raw_task = spawn_blocked_group_after_event(
        &start_gate, &child_gate, &root_started, &child_started, &child_runs);
    auto raw_handle =
        elio::coro::detail::task_access::release(std::move(raw_task));
    REQUIRE(sched.try_spawn(raw_handle));
    REQUIRE(wait_for_flag(root_started, scaled_ms(2000)));

    bool drained = false;
    std::thread shutdown_thread([&] {
        drained = sched.shutdown(scaled_ms(5000));
    });

    const bool initial_drain_observed = wait_for_flag(
        elio::runtime::detail::graceful_drain_paused_for_test,
        scaled_ms(2000));
    start_gate.set();
    const bool linked_child_started =
        wait_for_flag(child_started, scaled_ms(2000));

    elio::runtime::detail::pause_after_graceful_drain_for_test.store(
        false, std::memory_order_release);
    elio::runtime::detail::pause_after_graceful_drain_for_test.notify_all();

    const auto recheck_deadline =
        std::chrono::steady_clock::now() + scaled_ms(2000);
    while (elio::runtime::detail::graceful_drain_rechecks_for_test.load(
               std::memory_order_acquire) == 0 &&
           std::chrono::steady_clock::now() < recheck_deadline) {
        std::this_thread::sleep_for(1ms);
    }
    const bool linked_child_rechecked =
        elio::runtime::detail::graceful_drain_rechecks_for_test.load(
            std::memory_order_acquire) > 0;

    child_gate.set();
    shutdown_thread.join();

    REQUIRE(initial_drain_observed);
    REQUIRE(linked_child_started);
    REQUIRE(linked_child_rechecked);
    REQUIRE(drained);
    REQUIRE(child_runs.load(std::memory_order_acquire) == 1);
}

TEST_CASE("scheduler destroys worker backends before tracking state",
          "[scheduler][shutdown][lifecycle][io][regression]") {
    scheduler_test_hook_guard hook_guard;
    elio::runtime::detail::worker_io_backend_for_test.store(
        elio::io::io_context::backend_type::epoll,
        std::memory_order_release);
    elio::runtime::detail::tracked_completions_during_worker_destroy_for_test.store(
        0, std::memory_order_release);

    std::atomic<bool> timer_resumed{false};
    auto sched = std::make_unique<scheduler>(1);
    sched->start();
    auto owner = sched->go_joinable(
        mark_after_sleep, scaled_sec(30), &timer_resumed);

    const auto pending_deadline =
        std::chrono::steady_clock::now() + scaled_ms(2000);
    while (sched->get_worker(0)->io_context().pending_count() == 0 &&
           std::chrono::steady_clock::now() < pending_deadline) {
        std::this_thread::sleep_for(1ms);
    }
    const bool timer_was_pending =
        sched->get_worker(0)->io_context().pending_count() > 0;

    sched->shutdown_force();
    sched.reset();
    owner.wait_destroyed();
    REQUIRE_NOTHROW(owner.await_resume());

    const size_t completions_during_worker_destroy =
        elio::runtime::detail::tracked_completions_during_worker_destroy_for_test.load(
            std::memory_order_acquire);
    REQUIRE(timer_was_pending);
    REQUIRE(timer_resumed.load(std::memory_order_acquire));
    REQUIRE(completions_during_worker_destroy == 1);
}

TEST_CASE("tracked completion survives scheduler destruction after resuspension",
          "[scheduler][shutdown][lifecycle][io][regression]") {
    scheduler_test_hook_guard hook_guard;
    elio::runtime::detail::worker_io_backend_for_test.store(
        elio::io::io_context::backend_type::epoll,
        std::memory_order_release);

    external_resume_slot slot;
    std::atomic<bool> cancellation_resumed{false};
    auto sched = std::make_unique<scheduler>(1);
    sched->start();
    auto owner = sched->go_joinable(
        resuspend_after_timer_cancel,
        scaled_sec(30),
        &slot,
        &cancellation_resumed);

    const auto pending_deadline =
        std::chrono::steady_clock::now() + scaled_ms(2000);
    while (sched->get_worker(0)->io_context().pending_count() == 0 &&
           std::chrono::steady_clock::now() < pending_deadline) {
        std::this_thread::sleep_for(1ms);
    }
    const bool timer_was_pending =
        sched->get_worker(0)->io_context().pending_count() > 0;

    sched->shutdown_force();
    sched.reset();

    void* suspended_address =
        slot.address.exchange(nullptr, std::memory_order_acq_rel);
    const bool resuspended_after_backend_cancel =
        cancellation_resumed.load(std::memory_order_acquire) &&
        suspended_address != nullptr;
    if (suspended_address) {
        std::coroutine_handle<>::from_address(suspended_address).resume();
    }

    owner.wait_destroyed();
    REQUIRE_NOTHROW(owner.await_resume());
    REQUIRE(timer_was_pending);
    REQUIRE(resuspended_after_backend_cancel);
}

TEST_CASE("wait_for_idle returns false when tasks exceed timeout",
          "[scheduler][shutdown]") {
    scheduler sched(2);
    sched.start();

    std::atomic<int> done{0};
    sched.go(mark_after_sleep_counter, scaled_ms(300), &done);

    std::this_thread::sleep_for(scaled_ms(10));

    auto t0 = std::chrono::steady_clock::now();
    bool drained = sched.wait_for_idle(scaled_ms(20));
    auto elapsed = std::chrono::steady_clock::now() - t0;

    REQUIRE(!drained);
    REQUIRE(elapsed >= scaled_ms(15));   // honored the timeout
    REQUIRE(elapsed < scaled_ms(200));   // didn't wait the full task duration

    // Let the task complete naturally before shutdown (avoids IO orphaning).
    REQUIRE(sched.shutdown(scaled_ms(2000)));
    REQUIRE(done.load() == 1);
}

TEST_CASE("scheduler deadlines saturate extreme finite timeouts",
          "[scheduler][shutdown][timeout][regression]") {
    using clock = std::chrono::steady_clock;
    const auto extreme = std::chrono::milliseconds::max() - 1ms;

    CHECK(elio::runtime::detail::saturating_steady_deadline(extreme) ==
          clock::time_point::max());
    CHECK(elio::runtime::detail::saturating_steady_deadline(
              std::chrono::milliseconds::max()) ==
          clock::time_point::max());

    const auto before = clock::now();
    const auto nonpositive =
        elio::runtime::detail::saturating_steady_deadline(-1ms);
    const auto after = clock::now();
    CHECK(nonpositive >= before);
    CHECK(nonpositive <= after);

    scheduler sched(1);
    sched.start();
    REQUIRE(sched.wait_for_idle(extreme));
    REQUIRE(sched.shutdown(extreme));
}

TEST_CASE("shutdown_force on idle scheduler is near-immediate",
          "[scheduler][shutdown]") {
    scheduler sched(2);
    sched.start();

    // Run a quick task and wait for it to complete so the scheduler is idle.
    std::atomic<int> count{0};
    sched.go(increment_only, &count);
    REQUIRE(sched.wait_for_idle(scaled_ms(2000)));
    REQUIRE(count.load() == 1);

    auto t0 = std::chrono::steady_clock::now();
    sched.shutdown_force();
    auto elapsed = std::chrono::steady_clock::now() - t0;

    REQUIRE(!sched.is_running());
    REQUIRE(elapsed < scaled_ms(200));
}

TEST_CASE("shutdown from worker task falls back without self-join",
          "[scheduler][shutdown]") {
    scheduler sched(1);
    sched.start();

    std::atomic<bool> returned{false};
    std::atomic<bool> drained{true};
    sched.go(call_shutdown_from_worker, &sched, &returned, &drained);

    REQUIRE(wait_for_flag(returned, scaled_ms(2000)));
    REQUIRE(!drained.load(std::memory_order_acquire));
    REQUIRE(!sched.is_running());
}

TEST_CASE("shutdown_force from worker task does not self-join",
          "[scheduler][shutdown]") {
    scheduler sched(1);
    sched.start();

    std::atomic<bool> returned{false};
    sched.go(call_shutdown_force_from_worker, &sched, &returned);

    REQUIRE(wait_for_flag(returned, scaled_ms(2000)));
    REQUIRE(!sched.is_running());
}

TEST_CASE("shutdown_force from a resize-draining worker does not self-join",
          "[scheduler][shutdown][resize][regression]") {
    scheduler sched(2);
    sched.start();
    std::atomic<bool> worker_started{false};
    std::atomic<bool> shrink_complete{false};

    auto owner = sched.go_joinable_to(1, [&]() -> task<void> {
        worker_started.store(true, std::memory_order_release);
        while (!shrink_complete.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        sched.shutdown_force();
        co_return;
    });

    REQUIRE(wait_for_flag(worker_started, scaled_ms(2000)));
    sched.set_thread_count(1);
    shrink_complete.store(true, std::memory_order_release);

    owner.wait_destroyed();
    REQUIRE_NOTHROW(owner.await_resume());
    REQUIRE(!sched.is_running());
}

TEST_CASE("scheduler destructor waits for worker-initiated teardown",
          "[scheduler][shutdown][lifecycle][regression]") {
    auto sched = std::make_unique<scheduler>(2);
    sched->start();
    auto* raw_scheduler = sched.get();
    std::atomic<bool> teardown_released{false};

    elio::runtime::detail::pause_shutdown_teardown_for_test.store(
        true, std::memory_order_release);
    auto owner = raw_scheduler->go_joinable(
        [raw_scheduler]() -> task<void> {
            raw_scheduler->shutdown_force();
            co_return;
        });

    const bool teardown_paused = wait_for_flag(
        elio::runtime::detail::shutdown_teardown_paused_for_test,
        scaled_ms(2000));

    std::thread releaser([&] {
        std::this_thread::sleep_for(scaled_ms(20));
        teardown_released.store(true, std::memory_order_release);
        elio::runtime::detail::pause_shutdown_teardown_for_test.store(
            false, std::memory_order_release);
        elio::runtime::detail::pause_shutdown_teardown_for_test.notify_all();
    });
    sched.reset();
    const bool released_before_destructor_returned =
        teardown_released.load(std::memory_order_acquire);
    releaser.join();

    owner.wait_destroyed();
    REQUIRE_NOTHROW(owner.await_resume());
    REQUIRE(teardown_paused);
    REQUIRE(released_before_destructor_returned);
}

TEST_CASE("concurrent shutdown waiters serialize the final worker join",
          "[scheduler][shutdown][lifecycle][regression]") {
    scheduler sched(2);
    sched.start();
    std::atomic<int> waiters_returned{0};
    std::atomic<int> waiter_failures{0};
    elio::runtime::detail::shutdown_waiters_for_test.store(
        0, std::memory_order_release);

    elio::runtime::detail::pause_shutdown_teardown_for_test.store(
        true, std::memory_order_release);
    auto owner = sched.go_joinable([&sched]() -> task<void> {
        sched.shutdown_force();
        co_return;
    });

    const bool teardown_paused = wait_for_flag(
        elio::runtime::detail::shutdown_teardown_paused_for_test,
        scaled_ms(2000));

    if (!teardown_paused) {
        elio::runtime::detail::pause_shutdown_teardown_for_test.store(
            false, std::memory_order_release);
        elio::runtime::detail::pause_shutdown_teardown_for_test.notify_all();
        owner.wait_destroyed();
        REQUIRE_NOTHROW(owner.await_resume());
        REQUIRE(teardown_paused);
        return;
    }

    auto wait_for_shutdown = [&] {
        try {
            sched.shutdown_force();
        } catch (...) {
            waiter_failures.fetch_add(1, std::memory_order_relaxed);
        }
        waiters_returned.fetch_add(1, std::memory_order_release);
    };
    std::thread first(wait_for_shutdown);
    std::thread second(wait_for_shutdown);

    const auto waiters_deadline =
        std::chrono::steady_clock::now() + scaled_ms(2000);
    while (elio::runtime::detail::shutdown_waiters_for_test.load(
               std::memory_order_acquire) != 2 &&
           std::chrono::steady_clock::now() < waiters_deadline) {
        std::this_thread::yield();
    }
    const size_t waiters_before_release =
        elio::runtime::detail::shutdown_waiters_for_test.load(
            std::memory_order_acquire);

    elio::runtime::detail::pause_shutdown_teardown_for_test.store(
        false, std::memory_order_release);
    elio::runtime::detail::pause_shutdown_teardown_for_test.notify_all();
    first.join();
    second.join();

    owner.wait_destroyed();
    REQUIRE_NOTHROW(owner.await_resume());
    REQUIRE(teardown_paused);
    REQUIRE(waiters_before_release == 2);
    REQUIRE(waiters_returned.load(std::memory_order_acquire) == 2);
    REQUIRE(waiter_failures.load(std::memory_order_relaxed) == 0);
}

TEST_CASE("scheduler does not restart after shutdown begins",
          "[scheduler][shutdown][lifecycle][regression]") {
    scheduler sched(1);
    sched.start();
    std::atomic<bool> shutdown_returned{false};
    std::atomic<bool> allow_worker_exit{false};

    auto owner = sched.go_joinable([&]() -> task<void> {
        sched.shutdown_force();
        shutdown_returned.store(true, std::memory_order_release);
        while (!allow_worker_exit.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        co_return;
    });

    const bool shutdown_returned_before_release =
        wait_for_flag(shutdown_returned, scaled_ms(2000));
    bool restarted_before_worker_exit = false;

    // The initiating worker is still inside its current coroutine and its
    // std::thread remains joinable here.
    if (shutdown_returned_before_release) {
        sched.start();
        restarted_before_worker_exit = sched.is_running();
    }
    allow_worker_exit.store(true, std::memory_order_release);

    owner.wait_destroyed();
    REQUIRE_NOTHROW(owner.await_resume());
    REQUIRE(shutdown_returned_before_release);
    REQUIRE(!restarted_before_worker_exit);
}

TEST_CASE("shutdown before first start closes the scheduler lifecycle",
          "[scheduler][shutdown][lifecycle][regression]") {
    scheduler sched(1);
    sched.shutdown_force();
    sched.start();
    REQUIRE(!sched.is_running());
}

TEST_CASE("resize is rejected after shutdown teardown begins",
          "[scheduler][shutdown][resize][lifecycle][regression]") {
    scheduler sched(1);
    sched.start();

    elio::runtime::detail::pause_shutdown_teardown_for_test.store(
        true, std::memory_order_release);
    auto owner = sched.go_joinable([&sched]() -> task<void> {
        sched.shutdown_force();
        co_return;
    });

    const bool teardown_paused = wait_for_flag(
        elio::runtime::detail::shutdown_teardown_paused_for_test,
        scaled_ms(2000));
    const size_t count_before_resize = sched.num_threads();
    sched.set_thread_count(2);
    const size_t count_after_resize = sched.num_threads();

    elio::runtime::detail::pause_shutdown_teardown_for_test.store(
        false, std::memory_order_release);
    elio::runtime::detail::pause_shutdown_teardown_for_test.notify_all();

    owner.wait_destroyed();
    REQUIRE_NOTHROW(owner.await_resume());
    REQUIRE(teardown_paused);
    REQUIRE(count_before_resize == 1);
    REQUIRE(count_after_resize == 1);
}

TEST_CASE("draining worker shutdown is handed to an active resize",
          "[scheduler][shutdown][resize][lifecycle][regression]") {
    scheduler sched(2);
    sched.start();
    std::atomic<bool> worker_started{false};
    std::atomic<bool> observe_resize{false};
    std::atomic<bool> worker_observed_resize{false};
    std::atomic<bool> worker_shutdown_returned{false};
    std::atomic<bool> grow_returned{false};
    elio::runtime::detail::resize_waiting_for_draining_worker_for_test.store(
        false, std::memory_order_release);

    auto owner = sched.go_joinable_to(1, [&]() -> task<void> {
        worker_started.store(true, std::memory_order_release);
        const auto deadline =
            std::chrono::steady_clock::now() + scaled_ms(2000);
        while (!observe_resize.load(std::memory_order_acquire) &&
               std::chrono::steady_clock::now() < deadline) {
            std::this_thread::yield();
        }
        while (!elio::runtime::detail::resize_waiting_for_draining_worker_for_test.load(
                   std::memory_order_acquire) &&
               std::chrono::steady_clock::now() < deadline) {
            std::this_thread::yield();
        }
        const bool observed =
            elio::runtime::detail::resize_waiting_for_draining_worker_for_test.load(
                std::memory_order_acquire);
        worker_observed_resize.store(observed, std::memory_order_release);
        if (observed) {
            sched.shutdown_force();
            worker_shutdown_returned.store(true, std::memory_order_release);
        }
        co_return;
    });

    const bool started = wait_for_flag(worker_started, scaled_ms(2000));
    if (started) {
        sched.set_thread_count(1);
    }
    observe_resize.store(true, std::memory_order_release);

    std::thread grower([&] {
        sched.set_thread_count(2);
        grow_returned.store(true, std::memory_order_release);
    });
    grower.join();

    owner.wait_destroyed();
    REQUIRE_NOTHROW(owner.await_resume());
    REQUIRE(started);
    REQUIRE(worker_observed_resize.load(std::memory_order_acquire));
    REQUIRE(worker_shutdown_returned.load(std::memory_order_acquire));
    REQUIRE(grow_returned.load(std::memory_order_acquire));
    REQUIRE(!sched.is_running());
}

TEST_CASE("force shutdown interrupts grow waiting on a draining worker",
          "[scheduler][shutdown][resize][lifecycle][regression]") {
    scheduler sched(2);
    sched.start();
    std::atomic<bool> grow_returned{false};
    elio::runtime::detail::resize_waiting_for_draining_worker_for_test.store(
        false, std::memory_order_release);
    elio::runtime::detail::draining_worker_held_for_test.store(
        false, std::memory_order_release);
    elio::runtime::detail::hold_draining_worker_for_test.store(
        true, std::memory_order_release);

    sched.set_thread_count(1);
    const bool worker_held = wait_for_flag(
        elio::runtime::detail::draining_worker_held_for_test,
        scaled_ms(2000));

    std::thread grower([&] {
        sched.set_thread_count(2);
        grow_returned.store(true, std::memory_order_release);
    });

    const bool grow_waiting = wait_for_flag(
        elio::runtime::detail::resize_waiting_for_draining_worker_for_test,
        scaled_ms(2000));
    const auto shutdown_start = std::chrono::steady_clock::now();
    sched.shutdown_force();
    const auto shutdown_elapsed =
        std::chrono::steady_clock::now() - shutdown_start;
    grower.join();
    elio::runtime::detail::hold_draining_worker_for_test.store(
        false, std::memory_order_release);

    REQUIRE(worker_held);
    REQUIRE(grow_waiting);
    REQUIRE(grow_returned.load(std::memory_order_acquire));
    REQUIRE(shutdown_elapsed < scaled_ms(1000));
    REQUIRE(!sched.is_running());
}

TEST_CASE("scheduler destructor clears current after cross-thread shutdown",
          "[scheduler][shutdown][regression]") {
    REQUIRE(scheduler::current() == nullptr);

    {
        scheduler sched(1);
        sched.start();
        REQUIRE(scheduler::current() == &sched);

        std::thread shutdowner([&sched]() {
            sched.shutdown_force();
        });
        shutdowner.join();

        REQUIRE(!sched.is_running());
    }

    REQUIRE(scheduler::current() == nullptr);
}

TEST_CASE("wait_for_idle returns when all tracked tasks complete",
          "[scheduler][shutdown]") {
    scheduler sched(2);
    sched.start();

    std::atomic<int> counter{0};
    constexpr int N = 8;
    for (int i = 0; i < N; ++i) {
        sched.go(mark_after_sleep_counter, scaled_ms(30), &counter);
    }

    REQUIRE(sched.wait_for_idle(scaled_ms(2000)));
    REQUIRE(counter.load() == N);
    REQUIRE(sched.active_tasks() == 0);

    sched.shutdown_force();
}

TEST_CASE("active_tasks counts in-flight tracked tasks",
          "[scheduler][shutdown]") {
    scheduler sched(2);
    sched.start();

    REQUIRE(sched.active_tasks() == 0);

    // Use a manual-reset event as the in-flight gate instead of a timed sleep.
    // The wrapped task suspends on `gate.wait()` until the test signals it,
    // so the "in-flight tracked" window is bounded by us, not by a wall clock —
    // eliminating the flake where a 100ms sleep_for could complete before the
    // polling loop observed active_tasks() >= 1 under load / scaled timing.
    elio::sync::event gate;
    std::atomic<int> done{0};
    sched.go(wait_on_event, &gate, &done);

    // Poll until the wrapper body has started and registered itself in
    // active_tracked_. Generous deadline (the gate keeps the task in-flight).
    auto deadline = std::chrono::steady_clock::now() + scaled_sec(5);
    while (sched.active_tasks() == 0 &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    REQUIRE(sched.active_tasks() >= 1);

    // Release the task and let it complete.
    gate.set();

    REQUIRE(sched.wait_for_idle(scaled_ms(2000)));
    REQUIRE(done.load() == 1);
    REQUIRE(sched.active_tasks() == 0);

    sched.shutdown_force();
}

TEST_CASE("elio::run waits for spawned tasks to complete (graceful by default)",
          "[scheduler][shutdown]") {
    std::atomic<int> children_done{0};

    elio::run([&]() -> task<int> {
        auto* sched = scheduler::current();
        REQUIRE(sched != nullptr);

        // Spawn three child tasks that suspend on timers, then return
        // before they finish. Without graceful shutdown these would be
        // dropped on the floor.
        sched->go(mark_after_sleep_counter, scaled_ms(50), &children_done);
        sched->go(mark_after_sleep_counter, scaled_ms(50), &children_done);
        sched->go(mark_after_sleep_counter, scaled_ms(50), &children_done);

        co_return 0;
    });

    REQUIRE(children_done.load() == 3);
}

TEST_CASE("elio::run shuts down spawned tasks when async_main throws",
          "[scheduler][shutdown]") {
    std::atomic<int> children_done{0};

    run_config config;
    config.num_threads = 1;
    config.shutdown_timeout = scaled_ms(1000);

    REQUIRE_THROWS_AS(
        elio::run([&]() -> task<int> {
            auto* sched = scheduler::current();
            REQUIRE(sched != nullptr);

            sched->go(mark_after_sleep_counter, scaled_ms(50), &children_done);

            throw std::runtime_error("async_main failed");
            co_return 0;
        }, config),
        std::runtime_error);

    REQUIRE(children_done.load() == 1);
}

TEST_CASE("shutdown can be called twice safely (idempotent)",
          "[scheduler][shutdown]") {
    scheduler sched(2);
    sched.start();

    std::atomic<int> counter{0};
    sched.go(mark_after_sleep_counter, scaled_ms(20), &counter);

    REQUIRE(sched.shutdown(scaled_ms(2000)));
    REQUIRE(!sched.is_running());

    // Second call is a no-op.
    REQUIRE(sched.shutdown(scaled_ms(10)));
    REQUIRE(!sched.is_running());

    REQUIRE(counter.load() == 1);
}

TEST_CASE("graceful shutdown coexists with go_joinable",
          "[scheduler][shutdown]") {
    scheduler sched(2);
    sched.start();

    auto handle = sched.go_joinable([]() -> task<int> {
        co_await sleep_for(50ms);
        co_return 42;
    });

    // Call shutdown without explicitly joining the handle.
    REQUIRE(sched.shutdown(scaled_ms(2000)));
    // The joinable wrapper completed via the same RAII tracking, so its
    // result is now ready even without an explicit await.
    REQUIRE(handle.is_ready());
}
