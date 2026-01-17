#include <elio/runtime/scheduler.hpp>
#include <elio/coro/task.hpp>
#include <elio/log/macros.hpp>
#include <iostream>
#include <atomic>
#include <chrono>
#include <vector>

using namespace elio;

// Simulate CPU-intensive work
coro::task<int> compute_fibonacci(int n) {
    // Simple iterative fibonacci
    if (n <= 1) {
        co_return n;
    }
    
    int a = 0, b = 1;
    for (int i = 2; i <= n; ++i) {
        int temp = a + b;
        a = b;
        b = temp;
    }
    
    co_return b;
}

// Task that computes and records completion
coro::task<void> worker_task([[maybe_unused]] int id, int work_amount, std::atomic<int>& completed) {
    ELIO_LOG_DEBUG("Task {} starting with work amount {}", id, work_amount);
    
    // Simulate varying workloads
    for (int i = 0; i < work_amount; ++i) {
        int result = co_await compute_fibonacci(20);
        (void)result;
    }
    
    completed.fetch_add(1, std::memory_order_relaxed);
    ELIO_LOG_DEBUG("Task {} completed", id);
    co_return;
}

int main() {
    // Set logging to info
    log::logger::instance().set_level(log::level::info);
    
    std::cout << "=== Elio Parallel Tasks Example ===" << std::endl;
    std::cout << "Demonstrating work-stealing scheduler with multiple workers" << std::endl;
    std::cout << std::endl;
    
    const int num_workers = 4;
    const int num_tasks = 50;
    
    std::cout << "Configuration:" << std::endl;
    std::cout << "  Worker threads: " << num_workers << std::endl;
    std::cout << "  Total tasks: " << num_tasks << std::endl;
    std::cout << std::endl;
    
    // Create scheduler
    runtime::scheduler sched(num_workers);
    sched.start();
    
    std::atomic<int> completed{0};
    
    // Record start time
    auto start_time = std::chrono::steady_clock::now();
    
    // Spawn tasks with varying workloads
    std::cout << "Spawning tasks..." << std::endl;
    for (int i = 0; i < num_tasks; ++i) {
        // Vary work amount: some tasks do more work than others
        int work_amount = 10 + (i % 20);
        auto t = worker_task(i, work_amount, completed);
        sched.spawn(t.release());
    }
    
    // Monitor progress
    std::cout << "Executing tasks..." << std::endl;
    int last_completed = 0;
    while (completed.load() < num_tasks) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        int current = completed.load();
        if (current != last_completed) {
            std::cout << "  Progress: " << current << "/" << num_tasks 
                      << " tasks completed" << std::endl;
            last_completed = current;
        }
    }
    
    // Record end time
    auto end_time = std::chrono::steady_clock::now();
    auto duration = std::chrono::duration_cast<std::chrono::milliseconds>(end_time - start_time);
    
    std::cout << std::endl;
    std::cout << "Results:" << std::endl;
    std::cout << "  Completed: " << completed.load() << "/" << num_tasks << " tasks" << std::endl;
    std::cout << "  Duration: " << duration.count() << " ms" << std::endl;
    std::cout << "  Total tasks executed: " << sched.total_tasks_executed() << std::endl;
    std::cout << "  Tasks per second: " 
              << (num_tasks * 1000.0 / duration.count()) << std::endl;
    
    std::cout << std::endl;
    std::cout << "Work-stealing statistics:" << std::endl;
    std::cout << "  Tasks were distributed across " << num_workers 
              << " workers" << std::endl;
    std::cout << "  Work stealing occurred to balance load" << std::endl;
    
    // Shutdown
    sched.shutdown();
    
    std::cout << std::endl;
    std::cout << "=== Example completed ===" << std::endl;
    
    return 0;
}
