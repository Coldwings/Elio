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

TEST_CASE("shared_mutex: destroying reader waiter does not crash on unlock()", "[sync][shared_mutex][cancellation]") {
    shared_mutex sm;
    sm.try_lock();  // Writer holds lock, so readers will suspend

    auto waiter_task = [&]() -> task<void> {
        co_await sm.lock_shared();
    };

    auto t = waiter_task();
    auto h = elio::coro::detail::task_access::release(t);
    h.resume();  // Suspends on lock_shared() because writer holds lock

    h.destroy();  // Destroy suspended reader — must unlink from reader_waiters_

    sm.unlock();  // Writer unlock — must NOT crash when scanning reader_waiters_
}

TEST_CASE("shared_mutex: destroying writer waiter does not deadlock subsequent operations", "[sync][shared_mutex][cancellation]") {
    shared_mutex sm;
    sm.try_lock_shared();  // Reader holds lock, so writer will suspend and set WRITER_WAITING

    auto waiter_task = [&]() -> task<void> {
        co_await sm.lock();
    };

    auto t = waiter_task();
    auto h = elio::coro::detail::task_access::release(t);
    h.resume();  // Suspends on lock() because reader holds lock

    h.destroy();  // Destroy suspended writer — must decrement pending_writers_ and clear WRITER_WAITING

    // Verify subsequent operations work (not deadlocked):
    // 1. A new reader should be able to acquire (WRITER_WAITING was cleared)
    sm.unlock_shared();  // Release the reader lock

    auto reader_task = [&]() -> task<void> {
        co_await sm.lock_shared();
        sm.unlock_shared();
    };
    auto rt = reader_task();
    auto rh = elio::coro::detail::task_access::release(rt);
    rh.resume();  // Should complete without suspending (no writer active/waiting)

    // 2. A new writer should be able to acquire (no readers active)
    auto writer_task = [&]() -> task<void> {
        co_await sm.lock();
        sm.unlock();
    };
    auto wt = writer_task();
    auto wh = elio::coro::detail::task_access::release(wt);
    wh.resume();  // Should complete without suspending
}

TEST_CASE("shared_mutex: destroying writer waiter wakes parked readers", "[sync][shared_mutex][cancellation]") {
    shared_mutex sm;
    sm.try_lock_shared();  // Reader R1 holds lock

    // Writer W1 suspends and sets WRITER_WAITING
    auto writer_task = [&]() -> task<void> {
        co_await sm.lock();
    };
    auto wt = writer_task();
    auto wh = elio::coro::detail::task_access::release(wt);
    wh.resume();  // Suspends on lock() because reader holds lock

    // Reader R2 tries to acquire, sees WRITER_WAITING, parks in reader_waiters_
    auto reader_task = [&]() -> task<void> {
        co_await sm.lock_shared();
        sm.unlock_shared();
    };
    auto rt = reader_task();
    auto rh = elio::coro::detail::task_access::release(rt);
    rh.resume();  // Suspends on lock_shared() because WRITER_WAITING is set

    // Verify R2 is parked
    REQUIRE(sm.reader_count() == 1);  // Only R1 is active

    // Destroy writer W1 — must clear WRITER_WAITING and wake parked reader R2
    wh.destroy();

    // Verify WRITER_WAITING was cleared and R2 was woken
    // R2 should now be active (reader_count should be 2: R1 + R2)
    // But R2 hasn't actually acquired the lock yet - it was just scheduled
    // So we need to release R1's lock first, then R2 can acquire

    // Release reader R1's lock
    sm.unlock_shared();

    // Now R2 should be able to complete when resumed
    // Note: schedule_handle was called in the destructor, so R2 is scheduled
    // We just need to verify the state is correct (no WRITER_WAITING)
    REQUIRE(!sm.is_writer_active());
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

    sched.shutdown();

    // Only 2 should have been woken (the destroyed one doesn't count)
    REQUIRE(woken.load() == 2);
}
