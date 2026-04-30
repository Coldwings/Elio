/**
 * Stress test for shared_mutex race conditions
 * 
 * This test reproduces:
 * 1. unlock_shared() arithmetic overflow
 * 2. lock_shared_awaitable TOCTOU race
 * 3. lock_awaitable TOCTOU race
 */

#include <elio/coro/task.hpp>
#include <elio/sync/primitives.hpp>
#include <elio/runtime/scheduler.hpp>
#include <iostream>
#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

using namespace elio::coro;
using namespace elio::sync;
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

struct TestResult {
    int successes = 0;
    int deadlocks = 0;
    int violations = 0;
};

/**
 * Test 1: High-contention stress test
 * Reproduces unlock_shared overflow and TOCTOU races
 */
TestResult test_high_contention(int num_rounds = 100) {
    TestResult result;
    
    for (int round = 0; round < num_rounds; ++round) {
        shared_mutex mutex;
        std::atomic<int> active_readers{0};
        std::atomic<int> active_writers{0};
        std::atomic<int> violations{0};
        std::atomic<int> completed{0};
        
        constexpr int NUM_READERS = 50;
        constexpr int NUM_WRITERS = 5;
        constexpr int TOTAL = NUM_READERS + NUM_WRITERS;
        
        scheduler sched(4);
        sched.start();
        
        // Reader task
        auto reader_task = [&]() -> task<void> {
            co_await mutex.lock_shared();
            
            ++active_readers;
            int writers = active_writers.load();
            
            // Violation: writer active while reader holds lock
            if (writers > 0) {
                violations++;
            }
            
            // Hold lock briefly
            std::this_thread::sleep_for(std::chrono::microseconds(10));
            
            --active_readers;
            mutex.unlock_shared();
            ++completed;
            co_return;
        };
        
        // Writer task
        auto writer_task = [&]() -> task<void> {
            co_await mutex.lock();
            
            int writers = ++active_writers;
            int readers = active_readers.load();
            
            // Violation: readers active while writer holds lock
            if (readers > 0) {
                violations++;
            }
            // Violation: multiple writers
            if (writers > 1) {
                violations++;
            }
            
            // Hold lock briefly
            std::this_thread::sleep_for(std::chrono::microseconds(20));
            
            --active_writers;
            mutex.unlock();
            ++completed;
            co_return;
        };

        // Spawn all tasks
        for (int i = 0; i < NUM_READERS; ++i) {
            spawn_task(sched, reader_task);
        }
        for (int i = 0; i < NUM_WRITERS; ++i) {
            spawn_task(sched, writer_task);
        }
        
        // Wait for completion with timeout
        bool deadlock = false;
        for (int i = 0; i < 500 && completed < TOTAL; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        
        if (completed < TOTAL) {
            deadlock = true;
        }
        
        sched.shutdown();
        
        if (deadlock) {
            result.deadlocks++;
        } else if (violations > 0) {
            result.violations += violations.load();
        } else {
            result.successes++;
        }
        
        if ((round + 1) % 10 == 0) {
            std::cout << "Round " << (round + 1) << "/" << num_rounds 
                      << ": Success=" << result.successes
                      << " Deadlock=" << result.deadlocks
                      << " Violations=" << result.violations << "\r" << std::flush;
        }
    }
    
    std::cout << "\n";
    return result;
}

/**
 * Test 2: unlock_shared overflow specific test
 * Creates scenario where reader_count might be 0 during unlock
 */
TestResult test_unlock_overflow(int num_rounds = 1000) {
    TestResult result;
    
    for (int round = 0; round < num_rounds; ++round) {
        shared_mutex mutex;
        std::atomic<int> completed{0};
        std::atomic<bool> writer_done{false};
        
        constexpr int NUM_READERS = 10;
        
        scheduler sched(2);
        sched.start();
        
        // Writer that repeatedly acquires/releases
        auto writer_task = [&]() -> task<void> {
            for (int i = 0; i < 10; ++i) {
                co_await mutex.lock();
                std::this_thread::sleep_for(std::chrono::microseconds(5));
                mutex.unlock();
            }
            writer_done = true;
            ++completed;
            co_return;
        };

        // Readers that race with writer
        auto reader_task = [&]() -> task<void> {
            while (!writer_done) {
                if (mutex.try_lock_shared()) {
                    mutex.unlock_shared();
                }
            }
            ++completed;
            co_return;
        };
        
        spawn_task(sched, writer_task);
        for (int i = 0; i < NUM_READERS; ++i) {
            spawn_task(sched, reader_task);
        }
        
        // Wait with timeout
        bool deadlock = false;
        for (int i = 0; i < 300 && completed < NUM_READERS + 1; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        
        if (completed < NUM_READERS + 1) {
            deadlock = true;
        }
        
        sched.shutdown();
        
        if (deadlock) {
            result.deadlocks++;
        } else {
            result.successes++;
        }
        
        if ((round + 1) % 100 == 0) {
            std::cout << "Overflow test round " << (round + 1) << "/" << num_rounds 
                      << ": Success=" << result.successes
                      << " Deadlock=" << result.deadlocks << "\r" << std::flush;
        }
    }
    
    std::cout << "\n";
    return result;
}

int main() {
    std::cout << "=== shared_mutex Stress Test ===\n\n";
    
    std::cout << "Test 1: High-contention (100 rounds x 50 readers x 5 writers)\n";
    auto r1 = test_high_contention(100);
    std::cout << "Result: Success=" << r1.successes 
              << ", Deadlock=" << r1.deadlocks 
              << ", Violations=" << r1.violations << "\n\n";
    
    std::cout << "Test 2: unlock_shared overflow (1000 rounds)\n";
    auto r2 = test_unlock_overflow(1000);
    std::cout << "Result: Success=" << r2.successes 
              << ", Deadlock=" << r2.deadlocks << "\n\n";
    
    int total_deadlocks = r1.deadlocks + r2.deadlocks;
    int total_violations = r1.violations + r2.violations;
    
    std::cout << "=== Summary ===\n";
    std::cout << "Total deadlocks: " << total_deadlocks << "\n";
    std::cout << "Total violations: " << total_violations << "\n";
    
    if (total_deadlocks > 0 || total_violations > 0) {
        std::cout << "\nFAILED: Race conditions detected!\n";
        return 1;
    } else {
        std::cout << "\nPASSED: No race conditions detected.\n";
        return 0;
    }
}
