#include <elio/elio.hpp>
#include <elio/runtime/autoscaler.hpp>
#include <iostream>
#include <atomic>
#include <chrono>
#include <functional>
#include <random>
#include <string_view>
#include <thread>
#include <vector>

using namespace elio;
using namespace std::chrono_literals;

namespace {

struct example_options {
    int phase1_tasks = 400;
    int phase2_tasks = 600;
    int load_samples = 10;
    int idle_samples = 20;
    std::chrono::milliseconds tick_interval{100};
    std::chrono::seconds idle_delay{1};
    std::chrono::milliseconds min_work{5};
    std::chrono::milliseconds max_work{20};
    size_t max_workers = 8;
};

void print_usage(std::string_view program) {
    std::cout << "Usage: " << program << " [--smoke]\n\n"
              << "  --smoke  Run a short termination check for CI\n";
}

bool parse_options(int argc, char** argv, example_options& options) {
    for (int i = 1; i < argc; ++i) {
        const std::string_view argument = argv[i];
        if (argument == "--help") {
            print_usage(argv[0]);
            return false;
        }
        if (argument == "--smoke") {
            options.phase1_tasks = 40;
            options.phase2_tasks = 60;
            options.load_samples = 4;
            options.idle_samples = 10;
            options.tick_interval = 20ms;
            options.idle_delay = 0s;
            options.min_work = 1ms;
            options.max_work = 3ms;
            options.max_workers = 4;
            continue;
        }

        std::cerr << "Unknown option: " << argument << '\n';
        print_usage(argv[0]);
        return false;
    }
    return true;
}

// Task that simulates work with random duration
coro::task<void> workload_task(
        std::atomic<int>& counter,
        std::chrono::milliseconds min_work,
        std::chrono::milliseconds max_work) {
    static thread_local std::mt19937 rng(std::hash<std::thread::id>{}(std::this_thread::get_id()));
    std::uniform_int_distribution<int> dist(
        static_cast<int>(min_work.count()),
        static_cast<int>(max_work.count()));
    std::this_thread::sleep_for(std::chrono::milliseconds(dist(rng)));

    counter.fetch_add(1, std::memory_order_relaxed);
    co_return;
}

bool run_load_phase(runtime::scheduler& sched,
                    const example_options& options,
                    int task_count,
                    std::string_view title,
                    std::string_view separator) {
    std::atomic<int> completed{0};
    std::vector<coro::join_handle<void>> tasks;
    tasks.reserve(static_cast<size_t>(task_count));

    try {
        for (int i = 0; i < task_count; ++i) {
            tasks.push_back(sched.go_joinable(workload_task(
                completed, options.min_work, options.max_work)));
        }

        std::cout << title << std::endl;
        std::cout << separator << std::endl;

        for (int i = 0; i < options.load_samples; ++i) {
            std::this_thread::sleep_for(options.tick_interval);

            if (i % 2 == 0) {
                std::cout << "  Workers: " << sched.num_threads()
                          << ", Pending: " << sched.pending_tasks()
                          << ", Completed: " << completed.load() << std::endl;
            }
        }
    } catch (...) {
        for (const auto& task : tasks) {
            task.wait_destroyed();
        }
        throw;
    }

    for (const auto& task : tasks) {
        task.wait_destroyed();
    }

    const int final_completed = completed.load(std::memory_order_acquire);
    const size_t final_pending = sched.pending_tasks();
    std::cout << "  Drained: Pending: " << final_pending
              << ", Completed: " << final_completed << std::endl;
    return final_completed == task_count && final_pending == 0;
}

} // namespace

int main(int argc, char** argv) {
    example_options options;
    if (!parse_options(argc, argv, options)) {
        return argc == 2 && std::string_view(argv[1]) == "--help" ? 0 : 2;
    }

    log::logger::instance().set_level(log::level::warning);

    std::cout << "=== Elio Autoscaler Example ===" << std::endl;
    std::cout << "Demonstrating automatic worker thread scaling" << std::endl;
    std::cout << std::endl;

    // Configure autoscaler
    elio::runtime::autoscaler_config config;
    config.tick_interval = options.tick_interval;
    config.overload_threshold = 20;
    config.idle_threshold = 5;
    config.idle_delay = options.idle_delay;
    config.min_workers = 2;
    config.max_workers = options.max_workers;

    // Create scheduler with minimum workers
    runtime::scheduler sched(config.min_workers);
    sched.start();

    // Create and start autoscaler with default triggers
    elio::runtime::autoscaler<runtime::scheduler> autoscaler(config);
    autoscaler.start(&sched);

    std::cout << "Initial workers: " << sched.num_threads() << std::endl;
    std::cout << std::endl;

    if (!run_load_phase(
            sched, options, options.phase1_tasks,
            "Phase 1: High load - expecting scale-up...",
            "----------------------------------------")) {
        std::cerr << "Phase 1 failed to drain its submitted tasks" << std::endl;
        autoscaler.stop();
        sched.shutdown();
        return 1;
    }

    std::cout << std::endl;

    if (!run_load_phase(
            sched, options, options.phase2_tasks,
            "Phase 2: Higher load - expecting more scale-up...",
            "-------------------------------------------")) {
        std::cerr << "Phase 2 failed to drain its submitted tasks" << std::endl;
        autoscaler.stop();
        sched.shutdown();
        return 1;
    }

    std::cout << std::endl;

    // Phase 3: Low load - wait for scale-down
    {
        std::cout << "Phase 3: Low load - waiting for scale-down..." << std::endl;
        std::cout << "------------------------------------------" << std::endl;

        // Wait longer for idle_delay to trigger scale-down
        for (int i = 0; i < options.idle_samples; ++i) {
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
