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

TEST_CASE("channel: destroying recv waiter does not crash on send()", "[sync][channel][cancellation]") {
    // Use a buffered channel (capacity 1) so send can complete without a receiver
    channel<int> ch(1);

    auto waiter_task = [&]() -> task<std::optional<int>> {
        co_return co_await ch.recv();
    };

    auto t = waiter_task();
    auto h = elio::coro::detail::task_access::release(t);
    h.resume();  // Suspends on ch.recv()

    h.destroy();

    // send() must NOT crash — the destroyed recv waiter was unlinked from the queue
    auto send_task = ch.send(42);
    auto sh = elio::coro::detail::task_access::release(send_task);
    sh.resume();  // Runs send to completion (fast path into ring buffer)
    // Coroutine self-destructs in final_awaiter (detached), so no manual destroy needed
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

    // Create 3 waiter tasks and suspend them on the event
    auto t1 = make_waiter();
    auto t2 = make_waiter();
    auto t3 = make_waiter();

    auto h1 = elio::coro::detail::task_access::release(t1);
    auto h2 = elio::coro::detail::task_access::release(t2);
    auto h3 = elio::coro::detail::task_access::release(t3);

    h1.resume();  // Suspends on e.wait()
    h2.resume();  // Suspends on e.wait()
    h3.resume();  // Suspends on e.wait()

    // Destroy one waiter — must unlink from the event's waiter list
    h2.destroy();

    // Set should wake only the remaining 2
    e.set();

    // Give the scheduler time to run the woken coroutines
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    sched.shutdown();

    // Only 2 should have been woken (the destroyed one doesn't count)
    REQUIRE(woken.load() == 2);
}
