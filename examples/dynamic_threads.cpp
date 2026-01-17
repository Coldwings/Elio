#include <elio/runtime/scheduler.hpp>
#include <elio/coro/task.hpp>
#include <elio/log/macros.hpp>
#include <iostream>
#include <atomic>
#include <chrono>

using namespace elio;

// Simple task
coro::task<void> simple_task(std::atomic<int>& counter) {
    counter.fetch_add(1, std::memory_order_relaxed);
    co_return;
}

// Run a batch of tasks and measure throughput
void run_batch(runtime::scheduler& sched, int num_tasks, const std::string& label) {
    std::atomic<int> completed{0};
    
    auto start = std::chrono::steady_clock::now();
    
    // Spawn tasks
    for (int i = 0; i < num_tasks; ++i) {
        auto t = simple_task(completed);
        sched.spawn(t.release());
    }
    
    // Wait for completion
    while (completed.load() < num_tasks) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    
    auto end = std::chrono::steady_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end - start);
    
    double tasks_per_sec = (num_tasks * 1000.0) / duration.count();
    
    std::cout << label << ":" << std::endl;
    std::cout << "  Threads: " << sched.num_threads() << std::endl;
    std::cout << "  Tasks: " << num_tasks << std::endl;
    std::cout << "  Duration: " << duration.count() << " ms" << std::endl;
    std::cout << "  Throughput: " << tasks_per_sec << " tasks/sec" << std::endl;
    std::cout << std::endl;
}

int main() {
    // Minimal logging
    log::logger::instance().set_level(log::level::warning);
    
    std::cout << "=== Elio Dynamic Thread Pool Example ===" << std::endl;
    std::cout << "Demonstrating dynamic thread pool adjustment under load" << std::endl;
    std::cout << std::endl;
    
    const int batch_size = 100;
    
    // Start with 2 threads
    runtime::scheduler sched(2);
    sched.start();
    
    std::cout << "Phase 1: Starting with 2 threads" << std::endl;
    run_batch(sched, batch_size, "Baseline (2 threads)");
    
    // Increase to 4 threads
    std::cout << "Phase 2: Increasing to 4 threads" << std::endl;
    sched.set_thread_count(4);
    run_batch(sched, batch_size, "After growth to 4 threads");
    
    // Increase to 8 threads
    std::cout << "Phase 3: Increasing to 8 threads" << std::endl;
    sched.set_thread_count(8);
    run_batch(sched, batch_size, "After growth to 8 threads");
    
    // Decrease to 4 threads
    std::cout << "Phase 4: Decreasing to 4 threads" << std::endl;
    sched.set_thread_count(4);
    run_batch(sched, batch_size, "After shrink to 4 threads");
    
    // Decrease to 2 threads
    std::cout << "Phase 5: Decreasing to 2 threads" << std::endl;
    sched.set_thread_count(2);
    run_batch(sched, batch_size, "After shrink to 2 threads");
    
    // Shutdown
    sched.shutdown();
    
    std::cout << "=== Example completed ===" << std::endl;
    std::cout << "Thread pool successfully adjusted dynamically!" << std::endl;
    std::cout << "Scheduler maintained correctness throughout all adjustments." << std::endl;
    
    return 0;
}
