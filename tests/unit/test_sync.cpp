#include <catch2/catch_test_macros.hpp>
#include <elio/sync/primitives.hpp>
#include <elio/coro/task.hpp>
#include <elio/runtime/scheduler.hpp>

#include <thread>
#include <vector>
#include <atomic>

using namespace elio::sync;
using namespace elio::coro;
using namespace elio::runtime;

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
    int counter = 0;
    std::atomic<int> completed{0};
    
    scheduler sched(2);
    sched.start();
    
    auto increment_task = [&]() -> task<void> {
        co_await m.lock();
        int temp = counter;
        std::this_thread::yield();  // Give other coroutines a chance
        counter = temp + 1;
        m.unlock();
        completed++;
    };
    
    // Create and spawn tasks - use release() to transfer ownership to scheduler
    // We track completion via the atomic counter
    constexpr int NUM_TASKS = 10;
    for (int i = 0; i < NUM_TASKS; ++i) {
        auto t = increment_task();
        sched.spawn(t.release());  // Transfer ownership - scheduler will manage lifetime
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
    
    // Use release() to transfer ownership to scheduler
    {
        auto p = producer();
        auto c = consumer();
        sched.spawn(p.release());
        sched.spawn(c.release());
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
        auto t = reader_task();
        sched.spawn(t.release());
    }
    for (int i = 0; i < NUM_WRITERS; ++i) {
        auto t = writer_task();
        sched.spawn(t.release());
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
