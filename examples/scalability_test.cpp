#include <elio/runtime/scheduler.hpp>
#include <elio/coro/task.hpp>
#include <elio/log/macros.hpp>
#include <iostream>
#include <atomic>
#include <chrono>
#include <iomanip>
#include <vector>
#include <algorithm>
#include <numeric>

using namespace elio;
using namespace std::chrono;

int main() {
    log::logger::instance().set_level(log::level::error);

    const int tasks_per_thread = 500;
    const int work_iterations = 100000;  // Make tasks CPU-bound
    const int warmup_iterations = 3;
    const int measure_iterations = 10;

    std::cout << "Scalability Test (tasks_per_thread=" << tasks_per_thread
              << ", work=" << work_iterations << " iterations)\n";
    std::cout << "Warming up " << warmup_iterations << " iterations, measuring "
              << measure_iterations << " iterations\n\n";

    double baseline_throughput = 0;

    for (size_t num_threads : {1, 2, 4, 8}) {
        int batch_size = tasks_per_thread * static_cast<int>(num_threads);
        std::vector<double> throughputs;

        // Run warmup + measurement iterations
        for (int iter = 0; iter < warmup_iterations + measure_iterations; ++iter) {
            runtime::scheduler sched(num_threads);
            sched.start();

            std::atomic<int> completed{0};

            auto task_func = [&]() -> coro::task<void> {
                volatile int sum = 0;
                for (int i = 0; i < work_iterations; ++i) {
                    sum = sum + i * i;
                }
                (void)sum;
                completed.fetch_add(1, std::memory_order_relaxed);
                co_return;
            };

            auto start = high_resolution_clock::now();

            // Distribute tasks evenly via spawn (round-robin)
            for (int i = 0; i < batch_size; ++i) {
                auto t = task_func();
                sched.spawn(t.release());
            }

            while (completed.load(std::memory_order_relaxed) < batch_size) {
                std::this_thread::sleep_for(microseconds(10));
            }

            auto end = high_resolution_clock::now();
            auto us = duration_cast<microseconds>(end - start).count();

            double throughput = (us > 0) ? (batch_size * 1000000.0) / us : 0;

            // Only record after warmup
            if (iter >= warmup_iterations) {
                throughputs.push_back(throughput);
            }

            sched.shutdown();
        }

        // Calculate median throughput (more stable than average)
        std::sort(throughputs.begin(), throughputs.end());
        double median_throughput = throughputs[throughputs.size() / 2];

        if (num_threads == 1) {
            baseline_throughput = median_throughput;
        }

        double speedup = (baseline_throughput > 0) ? median_throughput / baseline_throughput : 0;

        std::cout << "  " << num_threads << " threads: "
                  << std::fixed << std::setprecision(0) << median_throughput << " tasks/sec";
        std::cout << " (speedup: " << std::setprecision(2) << speedup << "x)";
        std::cout << " [min=" << std::setprecision(0) << throughputs.front()
                  << ", max=" << throughputs.back() << "]\n";
    }

    std::cout << "\nExpected scaling: 1x -> 2x -> 4x -> 8x (ideal)\n";

    return 0;
}
