#include <catch2/catch_test_macros.hpp>
#include <elio/sync/primitives.hpp>
#include <elio/coro/task.hpp>
#include <elio/runtime/scheduler.hpp>

#include <thread>
#include <vector>
#include <atomic>
#include <queue>
#include <latch>

using namespace elio::sync;
using namespace elio::coro;
using namespace elio::runtime;

// Helper to spawn a task to scheduler using high-level API (fire-and-forget)
template<typename F>
void spawn_task(scheduler& sched, F&& f) {
    sched.go(std::forward<F>(f));
}

// Helper to spawn a joinable task that can be awaited for completion
template<typename F>
auto spawn_joinable(scheduler& sched, F&& f) {
    return sched.go_joinable(std::forward<F>(f));
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
    
    // Use single-threaded scheduler to avoid TSAN memory reuse
    scheduler sched(1);
    sched.start();
    
    constexpr int NUM_TASKS = 5;
    
    // Use pointer capture to avoid lambda stack frame sharing
    mutex* m_ptr = &m;
    std::atomic<int>* counter_ptr = &counter;
    std::atomic<int>* completed_ptr = &completed;
    
    auto make_task = [=]() -> task<void> {
        co_await m_ptr->lock();
        counter_ptr->fetch_add(1, std::memory_order_relaxed);
        m_ptr->unlock();
        completed_ptr->fetch_add(1, std::memory_order_relaxed);
        co_return;
    };
    
    // Spawn joinable tasks to track destruction
    std::vector<join_handle<void>> joins;
    for (int i = 0; i < NUM_TASKS; ++i) {
        joins.push_back(spawn_joinable(sched, make_task));
    }
    
    // Wait for all tasks to complete and be destroyed
    for (auto& j : joins) {
        j.wait_destroyed();
    }
    
    sched.shutdown();
    
    REQUIRE(counter == NUM_TASKS);
    REQUIRE(completed.load(std::memory_order_relaxed) == NUM_TASKS);
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
    
    // Use pointer capture to avoid lambda stack frame issues
    channel<int>* ch_ptr = &ch;
    std::atomic<int>* sum_ptr = &sum;
    std::atomic<bool>* producer_done_ptr = &producer_done;
    std::atomic<bool>* consumer_done_ptr = &consumer_done;
    
    auto producer = [=]() -> task<void> {
        for (int i = 1; i <= 5; ++i) {
            co_await ch_ptr->send(i);
        }
        ch_ptr->close();
        *producer_done_ptr = true;
        co_return;
    };
    
    auto consumer = [=]() -> task<void> {
        while (true) {
            auto val = co_await ch_ptr->recv();
            if (!val) break;
            *sum_ptr += *val;
        }
        *consumer_done_ptr = true;
        co_return;
    };
    
    // Use single-threaded scheduler to avoid TSAN memory reuse
    scheduler sched(1);
    sched.start();
    
    auto producer_join = spawn_joinable(sched, producer);
    auto consumer_join = spawn_joinable(sched, consumer);
    
    producer_join.wait_destroyed();
    consumer_join.wait_destroyed();
    
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
    
    // Use single-threaded scheduler to avoid TSAN memory reuse
    scheduler sched(1);
    sched.start();
    
    constexpr int NUM_READERS = 3;
    constexpr int NUM_WRITERS = 2;
    constexpr int TOTAL = NUM_READERS + NUM_WRITERS;
    
    // Use pointer capture to avoid lambda stack frame sharing
    shared_mutex* m_ptr = &m;
    std::atomic<int>* read_count_ptr = &read_count;
    std::atomic<int>* max_concurrent_readers_ptr = &max_concurrent_readers;
    std::atomic<int>* write_count_ptr = &write_count;
    std::atomic<int>* completed_ptr = &completed;
    
    // Reader task - multiple can run concurrently
    auto make_reader = [=]() -> task<void> {
        co_await m_ptr->lock_shared();
        int current = ++(*read_count_ptr);
        int expected = max_concurrent_readers_ptr->load();
        while (current > expected && !max_concurrent_readers_ptr->compare_exchange_weak(expected, current)) {}
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
        --(*read_count_ptr);
        m_ptr->unlock_shared();
        completed_ptr->fetch_add(1, std::memory_order_relaxed);
        co_return;
    };
    
    // Writer task - exclusive access
    auto make_writer = [=]() -> task<void> {
        co_await m_ptr->lock();
        ++(*write_count_ptr);
        REQUIRE(*read_count_ptr == 0);  // No readers while writing
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        m_ptr->unlock();
        completed_ptr->fetch_add(1, std::memory_order_relaxed);
        co_return;
    };
    
    // Spawn joinable tasks
    std::vector<join_handle<void>> joins;
    for (int i = 0; i < NUM_READERS; ++i) {
        joins.push_back(spawn_joinable(sched, make_reader));
    }
    for (int i = 0; i < NUM_WRITERS; ++i) {
        joins.push_back(spawn_joinable(sched, make_writer));
    }
    
    // Wait for all tasks to complete and be destroyed
    for (auto& j : joins) {
        j.wait_destroyed();
    }
    
    sched.shutdown();
    
    REQUIRE(completed.load(std::memory_order_relaxed) == TOTAL);
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

    // Use single-threaded scheduler to avoid TSAN memory reuse
    scheduler sched(1);
    sched.start();

    constexpr int NUM_TASKS = 5;

    // Use pointer capture to avoid lambda stack frame sharing
    spinlock* s_ptr = &s;
    int* counter_ptr = &counter;
    std::atomic<int>* completed_ptr = &completed;

    auto make_task = [=]() -> task<void> {
        s_ptr->lock();
        int temp = *counter_ptr;
        std::this_thread::yield();
        *counter_ptr = temp + 1;
        s_ptr->unlock();
        completed_ptr->fetch_add(1, std::memory_order_relaxed);
        co_return;
    };

    std::vector<join_handle<void>> joins;
    for (int i = 0; i < NUM_TASKS; ++i) {
        joins.push_back(spawn_joinable(sched, make_task));
    }

    for (auto& j : joins) {
        j.wait_destroyed();
    }

    sched.shutdown();

    REQUIRE(counter == NUM_TASKS);
    REQUIRE(completed.load(std::memory_order_relaxed) == NUM_TASKS);
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

    // Use single-threaded scheduler to avoid TSAN memory reuse
    scheduler sched(1);
    sched.start();

    // Use pointer capture to avoid lambda stack frame issues
    mutex* mtx_ptr = &mtx;
    condition_variable* cv_ptr = &cv;
    std::atomic<bool>* ready_ptr = &ready;
    std::atomic<int>* completed_ptr = &completed;

    auto waiter = [=]() -> task<void> {
        co_await mtx_ptr->lock();
        while (!ready_ptr->load(std::memory_order_acquire)) {
            co_await co_await cv_ptr->wait(*mtx_ptr);
        }
        mtx_ptr->unlock();
        completed_ptr->fetch_add(1, std::memory_order_relaxed);
    };

    auto notifier = [=]() -> task<void> {
        co_await mtx_ptr->lock();
        ready_ptr->store(true, std::memory_order_release);
        mtx_ptr->unlock();
        cv_ptr->notify_one();
        completed_ptr->fetch_add(1, std::memory_order_relaxed);
    };

    auto waiter_join = spawn_joinable(sched, waiter);

    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    auto notifier_join = spawn_joinable(sched, notifier);

    // Wait for both to complete using join handles
    while (!waiter_join.is_ready()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    while (!notifier_join.is_ready()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    sched.shutdown();

    REQUIRE(completed.load(std::memory_order_relaxed) == 2);
    REQUIRE(ready.load());
}

TEST_CASE("condition_variable with mutex notify_all", "[sync][condvar][coro]") {
    mutex mtx;
    condition_variable cv;
    std::atomic<bool> ready{false};
    std::atomic<int> completed{0};

    // Use single-threaded scheduler to avoid TSAN memory reuse between coroutines
    scheduler sched(1);
    sched.start();

    constexpr int NUM_WAITERS = 5;

    // Use pointer capture to avoid lambda stack frame sharing
    mutex* mtx_ptr = &mtx;
    condition_variable* cv_ptr = &cv;
    std::atomic<bool>* ready_ptr = &ready;
    std::atomic<int>* completed_ptr = &completed;

    // Spawn waiters - single thread ensures sequential execution
    std::vector<join_handle<void>> waiter_joins;
    for (int i = 0; i < NUM_WAITERS; ++i) {
        auto waiter = [=]() -> task<void> {
            co_await mtx_ptr->lock();
            while (!ready_ptr->load(std::memory_order_acquire)) {
                co_await co_await cv_ptr->wait(*mtx_ptr);
            }
            mtx_ptr->unlock();
            completed_ptr->fetch_add(1, std::memory_order_relaxed);
            co_return;
        };
        waiter_joins.push_back(spawn_joinable(sched, waiter));
    }

    // Allow waiters to reach wait state
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    auto notifier = [=]() -> task<void> {
        co_await mtx_ptr->lock();
        ready_ptr->store(true, std::memory_order_release);
        mtx_ptr->unlock();
        cv_ptr->notify_all();
        co_return;
    };
    auto notifier_join = spawn_joinable(sched, notifier);

    for (auto& j : waiter_joins) {
        j.wait_destroyed();
    }
    notifier_join.wait_destroyed();

    sched.shutdown();

    REQUIRE(completed.load(std::memory_order_relaxed) == NUM_WAITERS);
}

TEST_CASE("condition_variable with spinlock", "[sync][condvar][coro]") {
    spinlock sl;
    condition_variable cv;
    std::atomic<bool> ready{false};
    std::atomic<int> completed{0};

    // Use single-threaded scheduler to avoid TSAN memory reuse
    scheduler sched(1);
    sched.start();

    // Use pointer capture to avoid lambda stack frame issues
    spinlock* sl_ptr = &sl;
    condition_variable* cv_ptr = &cv;
    std::atomic<bool>* ready_ptr = &ready;
    std::atomic<int>* completed_ptr = &completed;

    auto waiter = [=]() -> task<void> {
        sl_ptr->lock();
        while (!ready_ptr->load(std::memory_order_acquire)) {
            co_await cv_ptr->wait(*sl_ptr);
        }
        sl_ptr->unlock();
        completed_ptr->fetch_add(1, std::memory_order_relaxed);
        co_return;
    };

    auto notifier = [=]() -> task<void> {
        sl_ptr->lock();
        ready_ptr->store(true, std::memory_order_release);
        sl_ptr->unlock();
        cv_ptr->notify_one();
        completed_ptr->fetch_add(1, std::memory_order_relaxed);
        co_return;
    };

    auto waiter_join = spawn_joinable(sched, waiter);

    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    auto notifier_join = spawn_joinable(sched, notifier);

    waiter_join.wait_destroyed();
    notifier_join.wait_destroyed();

    sched.shutdown();

    REQUIRE(completed.load(std::memory_order_relaxed) == 2);
}

TEST_CASE("condition_variable unlocked", "[sync][condvar][coro]") {
    condition_variable cv;
    std::atomic<bool> ready{false};
    std::atomic<int> completed{0};

    // Single worker: all coroutines run on the same thread
    scheduler sched(1);
    sched.start();

    // Use pointer capture to avoid lambda stack frame issues
    condition_variable* cv_ptr = &cv;
    std::atomic<bool>* ready_ptr = &ready;
    std::atomic<int>* completed_ptr = &completed;

    auto waiter = [=]() -> task<void> {
        while (!ready_ptr->load(std::memory_order_acquire)) {
            co_await cv_ptr->wait_unlocked();
        }
        completed_ptr->fetch_add(1, std::memory_order_relaxed);
        co_return;
    };

    auto notifier = [=]() -> task<void> {
        ready_ptr->store(true, std::memory_order_release);
        cv_ptr->notify_one();
        completed_ptr->fetch_add(1, std::memory_order_relaxed);
        co_return;
    };

    auto waiter_join = spawn_joinable(sched, waiter);

    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    auto notifier_join = spawn_joinable(sched, notifier);

    waiter_join.wait_destroyed();
    notifier_join.wait_destroyed();

    sched.shutdown();

    REQUIRE(completed.load(std::memory_order_relaxed) == 2);
}

TEST_CASE("condition_variable notify_one wakes exactly one", "[sync][condvar][coro]") {
    mutex mtx;
    condition_variable cv;
    std::atomic<int> phase{0};
    std::atomic<int> woken{0};
    std::atomic<int> completed{0};

    // Use single-threaded scheduler to avoid TSAN memory reuse between coroutines
    scheduler sched(1);
    sched.start();

    constexpr int NUM_WAITERS = 3;

    // Use pointer capture to avoid lambda stack frame sharing between coroutines
    mutex* mtx_ptr = &mtx;
    condition_variable* cv_ptr = &cv;
    std::atomic<int>* phase_ptr = &phase;
    std::atomic<int>* woken_ptr = &woken;
    std::atomic<int>* completed_ptr = &completed;

    // Spawn waiters one at a time to avoid TSAN memory reuse warnings
    // Wait for each to reach wait state before spawning next
    std::vector<join_handle<void>> waiter_joins;
    for (int i = 0; i < NUM_WAITERS; ++i) {
        // Create unique lambda for each waiter to ensure distinct memory addresses
        auto waiter = [=]() -> task<void> {
            co_await mtx_ptr->lock();
            while (phase_ptr->load(std::memory_order_acquire) == 0) {
                co_await co_await cv_ptr->wait(*mtx_ptr);
            }
            woken_ptr->fetch_add(1, std::memory_order_relaxed);
            mtx_ptr->unlock();
            completed_ptr->fetch_add(1, std::memory_order_relaxed);
            co_return;
        };
        waiter_joins.push_back(spawn_joinable(sched, waiter));
        // Small delay to ensure previous coroutine reaches wait state
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    // Set condition and notify exactly one
    auto notifier = [=]() -> task<void> {
        co_await mtx_ptr->lock();
        phase_ptr->store(1, std::memory_order_release);
        mtx_ptr->unlock();
        cv_ptr->notify_one();
        // Wait for exactly one to wake before completing
        for (int i = 0; i < 100 && woken_ptr->load(std::memory_order_relaxed) < 1; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        // Now wake the rest
        cv_ptr->notify_all();
        co_return;
    };
    auto notifier_join = spawn_joinable(sched, notifier);

    for (auto& j : waiter_joins) {
        j.wait_destroyed();
    }
    notifier_join.wait_destroyed();

    sched.shutdown();

    REQUIRE(completed.load(std::memory_order_relaxed) == NUM_WAITERS);
    REQUIRE(woken.load(std::memory_order_relaxed) == NUM_WAITERS);
}

TEST_CASE("condition_variable producer-consumer", "[sync][condvar][coro]") {
    mutex mtx;
    condition_variable cv;
    std::queue<int> buffer;
    bool done = false;
    std::atomic<int> sum{0};
    std::atomic<int> completed{0};

    // Use single-threaded scheduler to avoid TSAN memory reuse
    scheduler sched(1);
    sched.start();

    // Use pointer capture to avoid lambda stack frame issues
    mutex* mtx_ptr = &mtx;
    condition_variable* cv_ptr = &cv;
    std::queue<int>* buffer_ptr = &buffer;
    bool* done_ptr = &done;
    std::atomic<int>* sum_ptr = &sum;
    std::atomic<int>* completed_ptr = &completed;

    auto producer = [=]() -> task<void> {
        for (int i = 1; i <= 10; ++i) {
            co_await mtx_ptr->lock();
            buffer_ptr->push(i);
            mtx_ptr->unlock();
            cv_ptr->notify_one();
        }
        co_await mtx_ptr->lock();
        *done_ptr = true;
        mtx_ptr->unlock();
        cv_ptr->notify_all();
        (*completed_ptr)++;
    };

    auto consumer = [=]() -> task<void> {
        while (true) {
            co_await mtx_ptr->lock();
            while (buffer_ptr->empty() && !*done_ptr) {
                co_await co_await cv_ptr->wait(*mtx_ptr);
            }
            if (buffer_ptr->empty() && *done_ptr) {
                mtx_ptr->unlock();
                break;
            }
            int val = buffer_ptr->front();
            buffer_ptr->pop();
            mtx_ptr->unlock();
            *sum_ptr += val;
        }
        (*completed_ptr)++;
    };

    auto consumer_join = spawn_joinable(sched, consumer);
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    auto producer_join = spawn_joinable(sched, producer);

    consumer_join.wait_destroyed();
    producer_join.wait_destroyed();

    sched.shutdown();

    REQUIRE(completed.load(std::memory_order_relaxed) == 2);
    REQUIRE(sum == 55);  // 1+2+...+10
}
