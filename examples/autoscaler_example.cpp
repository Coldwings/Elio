#include <elio/elio.hpp>
#include <elio/runtime/autoscaler.hpp>
#include <iostream>
#include <atomic>
#include <chrono>
#include <random>

using namespace elio;

// Task that simulates work with random duration
coro::task<void> workload_task(std::atomic<int>& counter) {
    // Simulate variable work duration (10-100ms)
    static thread_local std::mt19937 rng(std::hash<std::thread::id>{}(std::this_thread::get_id()));
    std::uniform_int_distribution<int> dist(10, 100);
    std::this_thread::sleep_for(std::chrono::milliseconds(dist(rng)));

    counter.fetch_add(1, std::memory_order_relaxed);
    co_return;
}

int main() {
    log::logger::instance().set_level(log::level::warning);

    std::cout << "=== Elio Autoscaler Example ===" << std::endl;
    std::cout << "Demonstrating automatic worker thread scaling" << std::endl;
    std::cout << std::endl;

    // Configure autoscaler
    elio::runtime::autoscaler_config config;
    config.tick_interval = std::chrono::milliseconds(200);
    config.overload_threshold = 20;
    config.idle_threshold = 5;
    config.idle_delay = std::chrono::seconds(3);
    config.min_workers = 2;
    config.max_workers = 8;

    // Create scheduler with minimum workers
    runtime::scheduler sched(config.min_workers);
    sched.start();

    // Create and start autoscaler with default triggers
    elio::runtime::autoscaler<runtime::scheduler> autoscaler(config);
    autoscaler.start(&sched);

    std::cout << "Initial workers: " << sched.num_threads() << std::endl;
    std::cout << std::endl;

    // Phase 1: High load - demonstrate scale-up
    {
        std::atomic<int> completed{0};

        // Submit heavy workload
        for (int i = 0; i < 2000; ++i) {
            sched.spawn(workload_task(completed).release());
        }

        std::cout << "Phase 1: High load - expecting scale-up..." << std::endl;
        std::cout << "----------------------------------------" << std::endl;

        // Monitor autoscaler for 5 seconds
        for (int i = 0; i < 25; ++i) {
            std::this_thread::sleep_for(config.tick_interval);

            size_t workers = sched.num_threads();
            size_t pending = sched.pending_tasks();

            if (i % 2 == 0) {
                std::cout << "  Workers: " << workers
                          << ", Pending: " << pending
                          << ", Completed: " << completed.load() << std::endl;
            }
        }
    }

    std::cout << std::endl;

    // Phase 2: Even higher load
    {
        std::atomic<int> completed2{0};

        // Submit even heavier workload
        for (int i = 0; i < 3000; ++i) {
            sched.spawn(workload_task(completed2).release());
        }

        std::cout << "Phase 2: Higher load - expecting more scale-up..." << std::endl;
        std::cout << "-------------------------------------------" << std::endl;

        for (int i = 0; i < 25; ++i) {
            std::this_thread::sleep_for(config.tick_interval);

            size_t pending = sched.pending_tasks();

            if (i % 2 == 0) {
                std::cout << "  Workers: " << sched.num_threads()
                          << ", Pending: " << pending << std::endl;
            }
        }
    }

    std::cout << std::endl;

    // Phase 3: Low load - wait for scale-down
    {
        std::cout << "Phase 3: Low load - waiting for scale-down..." << std::endl;
        std::cout << "------------------------------------------" << std::endl;

        // Wait longer for idle_delay to trigger scale-down
        for (int i = 0; i < 30; ++i) {
            std::this_thread::sleep_for(config.tick_interval);

            size_t workers = sched.num_threads();
            size_t pending = sched.pending_tasks();

            if (i % 2 == 0) {
                std::cout << "  Workers: " << workers
                          << ", Pending: " << pending << std::endl;
            }
        }
    }

    std::cout << std::endl;

    // Stop autoscaler
    autoscaler.stop();

    std::cout << "Final workers: " << sched.num_threads() << std::endl;

    // Shutdown
    sched.shutdown();

    std::cout << std::endl;
    std::cout << "=== Example completed ===" << std::endl;
    std::cout << "Autoscaler automatically adjusted worker count based on load!" << std::endl;

    return 0;
}
