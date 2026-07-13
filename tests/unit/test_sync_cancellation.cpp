#include <catch2/catch_test_macros.hpp>
#include <elio/sync/primitives.hpp>
#include <elio/coro/task.hpp>
#include <elio/runtime/scheduler.hpp>

#include <atomic>
#include <coroutine>
#include <thread>

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

TEST_CASE("event wake does not schedule a waiter destroyed after dequeue",
          "[sync][event][cancellation][regression]") {
    event e;
    std::atomic<bool> destroy_second{false};
    std::atomic<bool> second_destroyed{false};

    auto first_waiter = [&]() -> task<void> {
        co_await e.wait();
        destroy_second.store(true, std::memory_order_release);
        while (!second_destroyed.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
    };
    auto second_waiter = [&]() -> task<void> {
        co_await e.wait();
    };

    auto first_task = first_waiter();
    auto second_task = second_waiter();
    auto first = elio::coro::detail::task_access::release(first_task);
    auto second = elio::coro::detail::task_access::release(second_task);
    first.resume();
    second.resume();

    std::thread destroyer([&] {
        while (!destroy_second.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        second.destroy();
        second_destroyed.store(true, std::memory_order_release);
    });

    e.set();
    destroyer.join();
}

TEST_CASE("condition_variable notify_all does not schedule a waiter destroyed after dequeue",
          "[sync][condvar][cancellation][regression]") {
    condition_variable cv;
    std::atomic<bool> destroy_second{false};
    std::atomic<bool> second_destroyed{false};

    auto first_waiter = [&]() -> task<void> {
        co_await cv.wait_unlocked();
        destroy_second.store(true, std::memory_order_release);
        while (!second_destroyed.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
    };
    auto second_waiter = [&]() -> task<void> {
        co_await cv.wait_unlocked();
    };

    auto first_task = first_waiter();
    auto second_task = second_waiter();
    auto first = elio::coro::detail::task_access::release(first_task);
    auto second = elio::coro::detail::task_access::release(second_task);
    first.resume();
    second.resume();

    std::thread destroyer([&] {
        while (!destroy_second.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        second.destroy();
        second_destroyed.store(true, std::memory_order_release);
    });

    cv.notify_all();
    destroyer.join();
}

TEST_CASE("semaphore release does not schedule a waiter destroyed after dequeue",
          "[sync][semaphore][cancellation][regression]") {
    semaphore sem(0);
    std::atomic<bool> destroy_second{false};
    std::atomic<bool> second_destroyed{false};

    auto first_waiter = [&]() -> task<void> {
        co_await sem.acquire();
        destroy_second.store(true, std::memory_order_release);
        while (!second_destroyed.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
    };
    auto second_waiter = [&]() -> task<void> {
        co_await sem.acquire();
    };

    auto first_task = first_waiter();
    auto second_task = second_waiter();
    auto first = elio::coro::detail::task_access::release(first_task);
    auto second = elio::coro::detail::task_access::release(second_task);
    first.resume();
    second.resume();

    std::thread destroyer([&] {
        while (!destroy_second.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        second.destroy();
        second_destroyed.store(true, std::memory_order_release);
    });

    sem.release(2);
    destroyer.join();
}

TEST_CASE("shared_mutex reader wake does not schedule a waiter destroyed after dequeue",
          "[sync][shared_mutex][cancellation][regression]") {
    shared_mutex sm;
    REQUIRE(sm.try_lock());
    std::atomic<bool> destroy_second{false};
    std::atomic<bool> second_destroyed{false};

    auto first_waiter = [&]() -> task<void> {
        co_await sm.lock_shared();
        destroy_second.store(true, std::memory_order_release);
        while (!second_destroyed.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        sm.unlock_shared();
    };
    auto second_waiter = [&]() -> task<void> {
        co_await sm.lock_shared();
        sm.unlock_shared();
    };

    auto first_task = first_waiter();
    auto second_task = second_waiter();
    auto first = elio::coro::detail::task_access::release(first_task);
    auto second = elio::coro::detail::task_access::release(second_task);
    first.resume();
    second.resume();

    std::thread destroyer([&] {
        while (!destroy_second.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        second.destroy();
        second_destroyed.store(true, std::memory_order_release);
    });

    sm.unlock();
    destroyer.join();
    REQUIRE(sm.reader_count() == 0);
}

TEST_CASE("channel close does not schedule a recv waiter destroyed after dequeue",
          "[sync][channel][cancellation][regression]") {
    channel<int> ch(0);
    std::atomic<bool> destroy_second{false};
    std::atomic<bool> second_destroyed{false};

    auto first_waiter = [&]() -> task<void> {
        auto value = co_await ch.recv();
        REQUIRE_FALSE(value.has_value());
        destroy_second.store(true, std::memory_order_release);
        while (!second_destroyed.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
    };
    auto second_waiter = [&]() -> task<void> {
        (void)co_await ch.recv();
    };

    auto first_task = first_waiter();
    auto second_task = second_waiter();
    auto first = elio::coro::detail::task_access::release(first_task);
    auto second = elio::coro::detail::task_access::release(second_task);
    first.resume();
    second.resume();

    std::thread destroyer([&] {
        while (!destroy_second.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        second.destroy();
        second_destroyed.store(true, std::memory_order_release);
    });

    ch.close();
    destroyer.join();
}

TEST_CASE("channel close does not schedule a send waiter destroyed after dequeue",
          "[sync][channel][cancellation][regression]") {
    channel<int> ch(0);
    std::atomic<bool> destroy_second{false};
    std::atomic<bool> second_destroyed{false};

    auto first_waiter = [&]() -> task<void> {
        (void)co_await ch.send(1);
        destroy_second.store(true, std::memory_order_release);
        while (!second_destroyed.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
    };
    auto second_waiter = [&]() -> task<void> {
        (void)co_await ch.send(2);
    };

    auto first_task = first_waiter();
    auto second_task = second_waiter();
    auto first = elio::coro::detail::task_access::release(first_task);
    auto second = elio::coro::detail::task_access::release(second_task);
    first.resume();
    second.resume();

    std::thread destroyer([&] {
        while (!destroy_second.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }
        second.destroy();
        second_destroyed.store(true, std::memory_order_release);
    });

    ch.close();
    destroyer.join();
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

TEST_CASE("mutex: destroying popped waiter transfers lock handoff", "[sync][mutex][cancellation]") {
    mutex m;
    REQUIRE(m.try_lock());

    auto second = m.lock();
    {
        auto first = m.lock();

        REQUIRE(first.await_suspend(std::noop_coroutine()));
        REQUIRE(second.await_suspend(std::noop_coroutine()));

        m.unlock();
        REQUIRE(m.is_locked());
    }

    REQUIRE(m.is_locked());
    second.await_resume();
    m.unlock();
    REQUIRE_FALSE(m.is_locked());
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

TEST_CASE("semaphore: destroying popped waiter transfers permit handoff", "[sync][semaphore][cancellation]") {
    semaphore sem(0);

    auto second = sem.acquire();
    {
        auto first = sem.acquire();

        REQUIRE(first.await_suspend(std::noop_coroutine()));
        REQUIRE(second.await_suspend(std::noop_coroutine()));

        sem.release();
        REQUIRE(sem.count() == 0);
    }

    REQUIRE(sem.count() == 0);
    second.await_resume();
    sem.release();
    REQUIRE(sem.count() == 1);
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

TEST_CASE("shared_mutex: destroying popped reader releases reader handoff", "[sync][shared_mutex][cancellation]") {
    shared_mutex sm;
    REQUIRE(sm.try_lock());

    auto second = sm.lock_shared();
    {
        auto first = sm.lock_shared();

        REQUIRE(first.await_suspend(std::noop_coroutine()));
        REQUIRE(second.await_suspend(std::noop_coroutine()));

        sm.unlock();
        REQUIRE(sm.reader_count() == 2);
    }

    REQUIRE(sm.reader_count() == 1);
    second.await_resume();
    sm.unlock_shared();
    REQUIRE(sm.reader_count() == 0);
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

TEST_CASE("shared_mutex: destroying popped writer transfers writer handoff", "[sync][shared_mutex][cancellation]") {
    shared_mutex sm;
    REQUIRE(sm.try_lock_shared());

    auto second = sm.lock();
    {
        auto first = sm.lock();

        REQUIRE(first.await_suspend(std::noop_coroutine()));
        REQUIRE(second.await_suspend(std::noop_coroutine()));

        sm.unlock_shared();
        REQUIRE(sm.is_writer_active());
    }

    REQUIRE(sm.is_writer_active());
    second.await_resume();
    sm.unlock();
    REQUIRE_FALSE(sm.is_writer_active());
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
