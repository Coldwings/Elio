#include <catch2/catch_test_macros.hpp>
#include <elio/sync/primitives.hpp>
#include <elio/coro/task.hpp>
#include <elio/runtime/scheduler.hpp>

#include <atomic>

using namespace elio::sync;
using namespace elio::coro;
using namespace elio::runtime;

TEST_CASE("event: destroying waiter does not crash on set()", "[sync][event][cancellation]") {
    event e;

    auto waiter_task = [&]() -> task<void> {
        co_await e.wait();
    };

    // Create and start a waiting coroutine
    auto t = waiter_task();
    auto h = elio::coro::detail::task_access::release(t);
    h.resume();  // Runs until co_await e.wait() suspends

    // Destroy the waiting coroutine (simulates cancellation/timeout)
    h.destroy();

    // This must NOT crash — the waiter was unlinked from the queue
    e.set();
}

TEST_CASE("mutex: destroying waiter does not crash on unlock()", "[sync][mutex][cancellation]") {
    mutex m;
    m.try_lock();  // Lock the mutex

    auto waiter_task = [&]() -> task<void> {
        co_await m.lock();
    };

    auto t = waiter_task();
    auto h = elio::coro::detail::task_access::release(t);
    h.resume();  // Runs until co_await m.lock() suspends

    h.destroy();
    m.unlock();  // Must NOT crash
}

TEST_CASE("semaphore: destroying waiter does not crash on release()", "[sync][semaphore][cancellation]") {
    semaphore sem(0);  // No permits available

    auto waiter_task = [&]() -> task<void> {
        co_await sem.acquire();
    };

    auto t = waiter_task();
    auto h = elio::coro::detail::task_access::release(t);
    h.resume();

    h.destroy();
    sem.release();  // Must NOT crash
}

TEST_CASE("condition_variable: destroying waiter does not crash on notify()", "[sync][condvar][cancellation]") {
    condition_variable cv;

    // Use wait_unlocked() to avoid needing a mutex lock dance.
    // wait_unlocked() directly enqueues into the cv's waiter list.
    auto waiter_task = [&]() -> task<void> {
        co_await cv.wait_unlocked();
    };

    auto t = waiter_task();
    auto h = elio::coro::detail::task_access::release(t);
    h.resume();  // Suspends on cv.wait_unlocked()

    h.destroy();
    cv.notify_one();  // Must NOT crash
    cv.notify_all();  // Must NOT crash
}

TEST_CASE("channel: destroying recv waiter does not crash on close()", "[sync][channel][cancellation]") {
    // Use a rendezvous channel (capacity 0) so recv always suspends
    channel<int> ch(0);

    auto waiter_task = [&]() -> task<std::optional<int>> {
        co_return co_await ch.recv();
    };

    auto t = waiter_task();
    auto h = elio::coro::detail::task_access::release(t);
    h.resume();  // Suspends on ch.recv()

    h.destroy();
    ch.close();  // Must NOT crash
}

TEST_CASE("channel: destroying send waiter does not crash on close()", "[sync][channel][cancellation]") {
    // Use a rendezvous channel (capacity 0) so send always suspends when no receiver
    channel<int> ch(0);

    auto waiter_task = [&]() -> task<bool> {
        co_return co_await ch.send(42);
    };

    auto t = waiter_task();
    auto h = elio::coro::detail::task_access::release(t);
    h.resume();  // Suspends on ch.send() — no receiver waiting

    h.destroy();
    ch.close();  // Must NOT crash
}

TEST_CASE("event: multiple waiters, destroy one, set wakes remaining", "[sync][event][cancellation]") {
    event e;
    std::atomic<int> woken{0};

    scheduler sched(2);
    sched.start();

    auto make_waiter = [&]() -> task<void> {
        co_await e.wait();
        woken.fetch_add(1);
    };

    // Spawn 3 waiters
    auto j1 = sched.go_joinable(make_waiter);
    auto j2 = sched.go_joinable(make_waiter);
    auto j3 = sched.go_joinable(make_waiter);

    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    // Set should wake all 3
    e.set();

    sched.shutdown();

    // All 3 should have been woken
    REQUIRE(woken.load() == 3);
}
