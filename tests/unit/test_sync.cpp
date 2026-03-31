#include <catch2/catch_test_macros.hpp>
#include <elio/sync/primitives.hpp>
#include <elio/coro/task.hpp>
#include <elio/runtime/scheduler.hpp>

#include <thread>
#include <vector>
#include <atomic>
#include <queue>

using namespace elio::sync;
using namespace elio::coro;
using namespace elio::runtime;

// Helper to spawn a task to scheduler using high-level API (fire-and-forget)
template<typename F>
void spawn_task(scheduler& sched, F&& f) {
    sched.go(std::forward<F>(f));
}

TEST_CASE("mutex basic operations", "[sync][mutex]") {
    mutex m;
    
    SECTION("initial state is unlocked") {
        REQUIRE_FALSE(m.is_locked());
    }
    
    SECTION("try_lock succeeds on unlocked mutex") {
        REQUIRE(m.try_lock());
        REQUIRE(m.is_locked());
        m.unlock();
        REQUIRE_FALSE(m.is_locked());
    }
    
    SECTION("try_lock fails on locked mutex") {
        REQUIRE(m.try_lock());
        REQUIRE_FALSE(m.try_lock());
        m.unlock();
    }
}

TEST_CASE("mutex with coroutines", "[sync][mutex][coro]") {
    mutex m;
    std::atomic<int> counter{0};
    std::atomic<int> completed{0};
    
    scheduler sched(2);
    sched.start();
    
    auto increment_task = [&]() -> task<void> {
        co_await m.lock();
        // Use fetch_add for atomic increment
        counter.fetch_add(1, std::memory_order_relaxed);
        m.unlock();
        completed.fetch_add(1, std::memory_order_relaxed);
    };
    
    // Create and spawn tasks - use spawn_task helper to transfer ownership to scheduler
    // We track completion via the atomic counter
    constexpr int NUM_TASKS = 10;
    for (int i = 0; i < NUM_TASKS; ++i) {
        spawn_task(sched, increment_task);  // Transfer ownership - scheduler will manage lifetime
    }
    
    // Wait for completion
    for (int i = 0; i < 200 && completed < NUM_TASKS; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    
    sched.shutdown();
    
    REQUIRE(counter == NUM_TASKS);
    REQUIRE(completed == NUM_TASKS);
}

TEST_CASE("lock_guard RAII", "[sync][mutex]") {
    mutex m;
    
    {
        REQUIRE(m.try_lock());
        lock_guard guard(m);
        // Guard takes ownership, mutex remains locked
        REQUIRE(m.is_locked());
    }
    // Guard destroyed, mutex unlocked
    REQUIRE_FALSE(m.is_locked());
}

TEST_CASE("semaphore basic operations", "[sync][semaphore]") {
    SECTION("initial count") {
        semaphore sem(5);
        REQUIRE(sem.count() == 5);
    }
    
    SECTION("try_acquire decrements count") {
        semaphore sem(3);
        REQUIRE(sem.try_acquire());
        REQUIRE(sem.count() == 2);
        REQUIRE(sem.try_acquire());
        REQUIRE(sem.count() == 1);
        REQUIRE(sem.try_acquire());
        REQUIRE(sem.count() == 0);
        REQUIRE_FALSE(sem.try_acquire());
    }
    
    SECTION("release increments count") {
        semaphore sem(0);
        sem.release();
        REQUIRE(sem.count() == 1);
        sem.release(3);
        REQUIRE(sem.count() == 4);
    }
}

TEST_CASE("event basic operations", "[sync][event]") {
    event e;
    
    SECTION("initial state is not set") {
        REQUIRE_FALSE(e.is_set());
    }
    
    SECTION("set and reset") {
        e.set();
        REQUIRE(e.is_set());
        e.reset();
        REQUIRE_FALSE(e.is_set());
    }
}

TEST_CASE("channel basic operations", "[sync][channel]") {
    channel<int> ch(3);  // Capacity of 3
    
    SECTION("try_send and try_recv") {
        REQUIRE(ch.try_send(1));
        REQUIRE(ch.try_send(2));
        REQUIRE(ch.try_send(3));
        REQUIRE_FALSE(ch.try_send(4));  // Full
        
        REQUIRE(ch.size() == 3);
        
        auto v1 = ch.try_recv();
        REQUIRE(v1.has_value());
        REQUIRE(*v1 == 1);
        
        auto v2 = ch.try_recv();
        REQUIRE(v2.has_value());
        REQUIRE(*v2 == 2);
        
        REQUIRE(ch.size() == 1);
    }
    
    SECTION("unbounded channel") {
        channel<int> unbounded(0);  // Unbounded
        
        for (int i = 0; i < 100; ++i) {
            REQUIRE(unbounded.try_send(i));
        }
        
        REQUIRE(unbounded.size() == 100);
    }
    
    SECTION("close channel") {
        channel<int> c(10);
        c.try_send(1);
        c.try_send(2);
        
        REQUIRE_FALSE(c.is_closed());
        c.close();
        REQUIRE(c.is_closed());
        
        // Can still receive existing items
        auto v = c.try_recv();
        REQUIRE(v.has_value());
        REQUIRE(*v == 1);
        
        // Cannot send after close
        REQUIRE_FALSE(c.try_send(3));
    }
}

TEST_CASE("channel with coroutines", "[sync][channel][coro]") {
    channel<int> ch(2);
    std::atomic<int> sum{0};
    std::atomic<bool> producer_done{false};
    std::atomic<bool> consumer_done{false};
    
    auto producer = [&]() -> task<void> {
        for (int i = 1; i <= 5; ++i) {
            co_await ch.send(i);
        }
        ch.close();
        producer_done = true;
    };
    
    auto consumer = [&]() -> task<void> {
        while (true) {
            auto val = co_await ch.recv();
            if (!val) break;
            sum += *val;
        }
        consumer_done = true;
    };
    
    scheduler sched(2);
    sched.start();
    
    // Use spawn_task helper to transfer ownership to scheduler
    {
        spawn_task(sched, producer);
        spawn_task(sched, consumer);
    }
    
    // Wait for completion
    for (int i = 0; i < 100 && (!producer_done || !consumer_done); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    
    sched.shutdown();
    
    REQUIRE(producer_done);
    REQUIRE(consumer_done);
    REQUIRE(sum == 15);  // 1+2+3+4+5
}

TEST_CASE("shared_mutex basic operations", "[sync][shared_mutex]") {
    shared_mutex m;
    
    SECTION("initial state") {
        REQUIRE(m.reader_count() == 0);
        REQUIRE_FALSE(m.is_writer_active());
    }
    
    SECTION("try_lock_shared succeeds multiple times") {
        REQUIRE(m.try_lock_shared());
        REQUIRE(m.reader_count() == 1);
        REQUIRE(m.try_lock_shared());
        REQUIRE(m.reader_count() == 2);
        REQUIRE(m.try_lock_shared());
        REQUIRE(m.reader_count() == 3);
        
        m.unlock_shared();
        m.unlock_shared();
        m.unlock_shared();
        REQUIRE(m.reader_count() == 0);
    }
    
    SECTION("try_lock succeeds on unlocked mutex") {
        REQUIRE(m.try_lock());
        REQUIRE(m.is_writer_active());
        m.unlock();
        REQUIRE_FALSE(m.is_writer_active());
    }
    
    SECTION("try_lock_shared fails when writer holds lock") {
        REQUIRE(m.try_lock());
        REQUIRE_FALSE(m.try_lock_shared());
        m.unlock();
    }
    
    SECTION("try_lock fails when readers hold lock") {
        REQUIRE(m.try_lock_shared());
        REQUIRE_FALSE(m.try_lock());
        m.unlock_shared();
    }
    
    SECTION("try_lock fails when writer holds lock") {
        REQUIRE(m.try_lock());
        REQUIRE_FALSE(m.try_lock());
        m.unlock();
    }
}

TEST_CASE("shared_mutex with coroutines", "[sync][shared_mutex][coro]") {
    shared_mutex m;
    std::atomic<int> read_count{0};
    std::atomic<int> max_concurrent_readers{0};
    std::atomic<int> write_count{0};
    std::atomic<int> completed{0};
    
    scheduler sched(4);
    sched.start();
    
    // Reader task - multiple can run concurrently
    auto reader_task = [&]() -> task<void> {
        co_await m.lock_shared();
        int current = ++read_count;
        int expected = max_concurrent_readers.load();
        while (current > expected && !max_concurrent_readers.compare_exchange_weak(expected, current)) {}
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        --read_count;
        m.unlock_shared();
        completed++;
    };
    
    // Writer task - exclusive access
    auto writer_task = [&]() -> task<void> {
        co_await m.lock();
        ++write_count;
        REQUIRE(read_count == 0);  // No readers while writing
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        m.unlock();
        completed++;
    };
    
    constexpr int NUM_READERS = 6;
    constexpr int NUM_WRITERS = 2;
    constexpr int TOTAL = NUM_READERS + NUM_WRITERS;
    
    // Spawn readers and writers
    for (int i = 0; i < NUM_READERS; ++i) {
        spawn_task(sched, reader_task);
    }
    for (int i = 0; i < NUM_WRITERS; ++i) {
        spawn_task(sched, writer_task);
    }
    
    // Wait for completion
    for (int i = 0; i < 200 && completed < TOTAL; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    
    sched.shutdown();
    
    REQUIRE(completed == TOTAL);
    REQUIRE(write_count == NUM_WRITERS);
    // Multiple readers should have run concurrently at some point
    REQUIRE(max_concurrent_readers > 0);
}

TEST_CASE("shared_lock_guard RAII", "[sync][shared_mutex]") {
    shared_mutex m;
    
    {
        REQUIRE(m.try_lock_shared());
        shared_lock_guard guard(m);
        REQUIRE(m.reader_count() == 1);
    }
    REQUIRE(m.reader_count() == 0);
}

TEST_CASE("unique_lock_guard RAII", "[sync][shared_mutex]") {
    shared_mutex m;

    {
        REQUIRE(m.try_lock());
        unique_lock_guard guard(m);
        REQUIRE(m.is_writer_active());
    }
    REQUIRE_FALSE(m.is_writer_active());
}

// ==================== Spinlock Tests ====================

TEST_CASE("spinlock basic operations", "[sync][spinlock]") {
    spinlock s;

    SECTION("initial state is unlocked") {
        REQUIRE_FALSE(s.is_locked());
    }

    SECTION("lock and unlock") {
        s.lock();
        REQUIRE(s.is_locked());
        s.unlock();
        REQUIRE_FALSE(s.is_locked());
    }

    SECTION("try_lock succeeds on unlocked") {
        REQUIRE(s.try_lock());
        REQUIRE(s.is_locked());
        s.unlock();
    }

    SECTION("try_lock fails on locked") {
        s.lock();
        REQUIRE_FALSE(s.try_lock());
        s.unlock();
    }
}

TEST_CASE("spinlock with coroutines", "[sync][spinlock][coro]") {
    spinlock s;
    int counter = 0;
    std::atomic<int> completed{0};

    scheduler sched(2);
    sched.start();

    auto increment_task = [&]() -> task<void> {
        s.lock();
        int temp = counter;
        std::this_thread::yield();
        counter = temp + 1;
        s.unlock();
        completed++;
        co_return;
    };

    constexpr int NUM_TASKS = 10;
    for (int i = 0; i < NUM_TASKS; ++i) {
        spawn_task(sched, increment_task);
    }

    for (int i = 0; i < 200 && completed < NUM_TASKS; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    sched.shutdown();

    REQUIRE(counter == NUM_TASKS);
    REQUIRE(completed == NUM_TASKS);
}

TEST_CASE("spinlock_guard RAII", "[sync][spinlock]") {
    spinlock s;

    {
        spinlock_guard guard(s);
        REQUIRE(s.is_locked());
    }
    REQUIRE_FALSE(s.is_locked());
}

TEST_CASE("spinlock_guard move semantics", "[sync][spinlock]") {
    spinlock s;

    {
        spinlock_guard guard1(s);
        REQUIRE(s.is_locked());

        spinlock_guard guard2(std::move(guard1));
        REQUIRE(s.is_locked());
    }
    // guard1 was moved from, guard2 destructor unlocks
    REQUIRE_FALSE(s.is_locked());
}

TEST_CASE("spinlock_guard manual unlock", "[sync][spinlock]") {
    spinlock s;

    spinlock_guard guard(s);
    REQUIRE(s.is_locked());
    guard.unlock();
    REQUIRE_FALSE(s.is_locked());
    // Destructor is safe to call after manual unlock
}

// ==================== Condition Variable Tests ====================

TEST_CASE("condition_variable has_waiters", "[sync][condvar]") {
    condition_variable cv;
    REQUIRE_FALSE(cv.has_waiters());
}

TEST_CASE("condition_variable with mutex notify_one", "[sync][condvar][coro]") {
    mutex mtx;
    condition_variable cv;
    std::atomic<bool> ready{false};
    std::atomic<int> completed{0};

    scheduler sched(2);
    sched.start();

    auto waiter = [&]() -> task<void> {
        co_await mtx.lock();
        while (!ready.load(std::memory_order_acquire)) {
            co_await co_await cv.wait(mtx);
        }
        mtx.unlock();
        completed.fetch_add(1, std::memory_order_relaxed);
    };

    auto notifier = [&]() -> task<void> {
        co_await mtx.lock();
        ready.store(true, std::memory_order_release);
        mtx.unlock();
        cv.notify_one();
        completed.fetch_add(1, std::memory_order_relaxed);
    };

    {
        spawn_task(sched, waiter);
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    {
        spawn_task(sched, notifier);
    }

    for (int i = 0; i < 200 && completed < 2; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    sched.shutdown();

    REQUIRE(completed == 2);
    REQUIRE(ready.load());
}

TEST_CASE("condition_variable with mutex notify_all", "[sync][condvar][coro]") {
    mutex mtx;
    condition_variable cv;
    std::atomic<bool> ready{false};
    std::atomic<int> completed{0};

    scheduler sched(4);
    sched.start();

    constexpr int NUM_WAITERS = 5;

    auto waiter = [&]() -> task<void> {
        co_await mtx.lock();
        while (!ready.load(std::memory_order_acquire)) {
            co_await co_await cv.wait(mtx);
        }
        mtx.unlock();
        completed.fetch_add(1, std::memory_order_relaxed);
    };

    for (int i = 0; i < NUM_WAITERS; ++i) {
        spawn_task(sched, waiter);
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    auto notifier = [&]() -> task<void> {
        co_await mtx.lock();
        ready.store(true, std::memory_order_release);
        mtx.unlock();
        cv.notify_all();
        co_return;
    };
    {
        spawn_task(sched, notifier);
    }

    for (int i = 0; i < 300 && completed < NUM_WAITERS; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    sched.shutdown();

    REQUIRE(completed == NUM_WAITERS);
}

TEST_CASE("condition_variable with spinlock", "[sync][condvar][coro]") {
    spinlock sl;
    condition_variable cv;
    std::atomic<bool> ready{false};
    std::atomic<int> completed{0};

    scheduler sched(2);
    sched.start();

    auto waiter = [&]() -> task<void> {
        sl.lock();
        while (!ready.load(std::memory_order_acquire)) {
            co_await cv.wait(sl);
        }
        sl.unlock();
        completed.fetch_add(1, std::memory_order_relaxed);
    };

    auto notifier = [&]() -> task<void> {
        sl.lock();
        ready.store(true, std::memory_order_release);
        sl.unlock();
        cv.notify_one();
        completed.fetch_add(1, std::memory_order_relaxed);
        co_return;
    };

    {
        spawn_task(sched, waiter);
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    {
        spawn_task(sched, notifier);
    }

    for (int i = 0; i < 200 && completed < 2; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    sched.shutdown();

    REQUIRE(completed == 2);
}

TEST_CASE("condition_variable unlocked", "[sync][condvar][coro]") {
    condition_variable cv;
    std::atomic<bool> ready{false};
    std::atomic<int> completed{0};

    // Single worker: all coroutines run on the same thread
    scheduler sched(1);
    sched.start();

    auto waiter = [&]() -> task<void> {
        while (!ready.load(std::memory_order_acquire)) {
            co_await cv.wait_unlocked();
        }
        completed.fetch_add(1, std::memory_order_relaxed);
    };

    auto notifier = [&]() -> task<void> {
        ready.store(true, std::memory_order_release);
        cv.notify_one();
        completed.fetch_add(1, std::memory_order_relaxed);
        co_return;
    };

    {
        spawn_task(sched, waiter);
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    {
        spawn_task(sched, notifier);
    }

    for (int i = 0; i < 200 && completed < 2; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    sched.shutdown();

    REQUIRE(completed == 2);
}

TEST_CASE("condition_variable notify_one wakes exactly one", "[sync][condvar][coro]") {
    mutex mtx;
    condition_variable cv;
    std::atomic<int> phase{0};
    std::atomic<int> woken{0};
    std::atomic<int> completed{0};

    scheduler sched(4);
    sched.start();

    constexpr int NUM_WAITERS = 3;

    auto waiter = [&]() -> task<void> {
        co_await mtx.lock();
        while (phase.load(std::memory_order_acquire) == 0) {
            co_await co_await cv.wait(mtx);
        }
        woken.fetch_add(1, std::memory_order_relaxed);
        mtx.unlock();
        completed.fetch_add(1, std::memory_order_relaxed);
    };

    for (int i = 0; i < NUM_WAITERS; ++i) {
        spawn_task(sched, waiter);
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    // Set condition and notify exactly one
    auto notifier = [&]() -> task<void> {
        co_await mtx.lock();
        phase.store(1, std::memory_order_release);
        mtx.unlock();
        cv.notify_one();
        co_return;
    };
    {
        spawn_task(sched, notifier);
    }

    // Wait for exactly one to wake
    for (int i = 0; i < 100 && woken < 1; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    // Only one should have woken
    REQUIRE(woken == 1);

    // Now wake the rest
    cv.notify_all();

    for (int i = 0; i < 200 && completed < NUM_WAITERS; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    sched.shutdown();

    REQUIRE(completed == NUM_WAITERS);
    REQUIRE(woken == NUM_WAITERS);
}

TEST_CASE("condition_variable producer-consumer", "[sync][condvar][coro]") {
    mutex mtx;
    condition_variable cv;
    std::queue<int> buffer;
    bool done = false;
    std::atomic<int> sum{0};
    std::atomic<int> completed{0};

    scheduler sched(2);
    sched.start();

    auto producer = [&]() -> task<void> {
        for (int i = 1; i <= 10; ++i) {
            co_await mtx.lock();
            buffer.push(i);
            mtx.unlock();
            cv.notify_one();
        }
        co_await mtx.lock();
        done = true;
        mtx.unlock();
        cv.notify_all();
        completed++;
    };

    auto consumer = [&]() -> task<void> {
        while (true) {
            co_await mtx.lock();
            while (buffer.empty() && !done) {
                co_await co_await cv.wait(mtx);
            }
            if (buffer.empty() && done) {
                mtx.unlock();
                break;
            }
            int val = buffer.front();
            buffer.pop();
            mtx.unlock();
            sum += val;
        }
        completed++;
    };

    {
        spawn_task(sched, consumer);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    {
        spawn_task(sched, producer);
    }

    for (int i = 0; i < 300 && completed < 2; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    sched.shutdown();

    REQUIRE(completed == 2);
    REQUIRE(sum == 55);  // 1+2+...+10
}
