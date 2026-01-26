/// Elio Thread Affinity Example
///
/// This example demonstrates how to use thread affinity to bind vthreads
/// to specific worker threads. This is useful when:
/// - You need thread-local state that shouldn't be shared
/// - You want to prevent work stealing for certain tasks
/// - You need predictable execution on specific threads
///
/// Build: cmake --build build
/// Run: ./build/thread_affinity

#include <elio/elio.hpp>
#include <iostream>
#include <atomic>
#include <map>
#include <mutex>

using namespace elio;

// Thread-local counter (simulates per-thread state)
thread_local int thread_local_counter = 0;

// Track which worker each task runs on
std::mutex output_mutex;

// Helper to print with worker info
void print_worker_info(const std::string& msg) {
    std::lock_guard<std::mutex> lock(output_mutex);
    std::cout << "[Worker " << current_worker_id() << "] " << msg << std::endl;
}

/// Example 1: Basic affinity - bind a task to a specific worker
coro::task<void> basic_affinity_example() {
    std::cout << "\n=== Example 1: Basic Thread Affinity ===" << std::endl;
    
    print_worker_info("Starting on this worker");
    
    // Bind to worker 0 and migrate there
    co_await set_affinity(0);
    
    print_worker_info("After set_affinity(0) - now on worker 0");
    
    // Yield multiple times - should stay on worker 0
    for (int i = 0; i < 5; ++i) {
        co_await time::yield();
        print_worker_info("After yield #" + std::to_string(i + 1) + " - still on worker 0");
    }
    
    // Clear affinity to allow migration again
    co_await clear_affinity();
    print_worker_info("Affinity cleared - can now migrate");
    
    co_return;
}

/// Example 2: Pin to current worker
coro::task<void> pin_to_current_example() {
    std::cout << "\n=== Example 2: Pin to Current Worker ===" << std::endl;
    
    size_t initial = current_worker_id();
    print_worker_info("Starting here");
    
    // Pin to wherever we are now
    co_await bind_to_current_worker();
    
    print_worker_info("Pinned to current worker");
    
    // Do some work with yields
    for (int i = 0; i < 3; ++i) {
        co_await time::yield();
        if (current_worker_id() != initial) {
            std::cerr << "ERROR: Task migrated unexpectedly!" << std::endl;
        }
    }
    
    print_worker_info("Stayed on same worker throughout");
    co_await clear_affinity();
    
    co_return;
}

/// Example 3: Thread-local state with affinity
coro::task<void> thread_local_state_task(int task_id, size_t target_worker) {
    // Bind to target worker
    co_await set_affinity(target_worker);
    
    // Increment thread-local counter
    thread_local_counter++;
    
    {
        std::lock_guard<std::mutex> lock(output_mutex);
        std::cout << "Task " << task_id << " on worker " << current_worker_id()
                  << ": thread_local_counter = " << thread_local_counter << std::endl;
    }
    
    co_return;
}

coro::task<void> thread_local_state_example() {
    std::cout << "\n=== Example 3: Thread-Local State ===" << std::endl;
    std::cout << "Tasks bound to same worker share thread-local state:" << std::endl;
    
    auto* sched = runtime::scheduler::current();
    size_t num_workers = sched->num_threads();
    
    // Spawn multiple tasks bound to different workers
    for (int i = 0; i < 8; ++i) {
        size_t target = i % std::min(num_workers, size_t(2));  // Distribute across 2 workers
        auto t = thread_local_state_task(i, target);
        sched->spawn(t.release());
    }
    
    // Give tasks time to complete
    co_await time::sleep_for(std::chrono::milliseconds(200));
    
    std::cout << "Notice: Tasks on the same worker see incrementing counter values" << std::endl;
    
    co_return;
}

/// Example 4: Affinity without immediate migration
coro::task<void> deferred_migration_example() {
    std::cout << "\n=== Example 4: Affinity Without Immediate Migration ===" << std::endl;
    
    size_t initial = current_worker_id();
    print_worker_info("Starting here");
    
    // Set affinity but don't migrate yet
    co_await set_affinity(1, false);  // false = don't migrate
    
    print_worker_info("Affinity set to worker 1, but didn't migrate yet");
    
    // Still on original worker
    if (current_worker_id() == initial) {
        print_worker_info("Confirmed: still on original worker");
    }
    
    // Yield will reschedule through the scheduler, respecting affinity
    co_await time::yield();
    
    print_worker_info("After yield - now on target worker 1");
    
    co_await clear_affinity();
    
    co_return;
}

/// Example 5: Multiple workers with different affinities
coro::task<void> worker_task(size_t worker_id, std::atomic<int>& counter) {
    co_await set_affinity(worker_id);
    
    for (int i = 0; i < 10; ++i) {
        counter.fetch_add(1);
        co_await time::yield();
        
        // Verify we're still on correct worker
        if (current_worker_id() != worker_id) {
            std::cerr << "ERROR: Task migrated from worker " << worker_id << std::endl;
        }
    }
    
    co_return;
}

coro::task<void> multi_worker_example() {
    std::cout << "\n=== Example 5: Multiple Workers with Affinity ===" << std::endl;
    
    auto* sched = runtime::scheduler::current();
    size_t num_workers = std::min(sched->num_threads(), size_t(4));
    
    std::vector<std::atomic<int>> counters(num_workers);
    for (auto& c : counters) c.store(0);
    
    // Spawn one task per worker, each bound to its worker
    std::cout << "Spawning " << num_workers << " tasks, one per worker..." << std::endl;
    
    for (size_t i = 0; i < num_workers; ++i) {
        auto t = worker_task(i, counters[i]);
        sched->spawn(t.release());
    }
    
    // Wait for completion
    co_await time::sleep_for(std::chrono::milliseconds(200));
    
    // Report results
    for (size_t i = 0; i < num_workers; ++i) {
        std::cout << "Worker " << i << " processed " << counters[i].load() << " iterations" << std::endl;
    }
    
    co_return;
}

/// Main async entry point
coro::task<int> async_main([[maybe_unused]] int argc, [[maybe_unused]] char* argv[]) {
    log::logger::instance().set_level(log::level::info);
    
    std::cout << "=== Elio Thread Affinity Example ===" << std::endl;
    std::cout << "Demonstrates binding vthreads to specific worker threads" << std::endl;
    
    auto* sched = runtime::scheduler::current();
    std::cout << "\nScheduler has " << sched->num_threads() << " worker threads" << std::endl;
    
    // Run examples
    co_await basic_affinity_example();
    co_await pin_to_current_example();
    co_await thread_local_state_example();
    co_await deferred_migration_example();
    co_await multi_worker_example();
    
    std::cout << "\n=== All examples completed ===" << std::endl;
    
    co_return 0;
}

ELIO_ASYNC_MAIN(async_main)
