#include <elio/runtime/scheduler.hpp>
#include <elio/coro/task.hpp>
#include <elio/time/timer.hpp>
#include <elio/log/macros.hpp>
#include <iostream>
#include <atomic>
#include <chrono>
#include <iomanip>
#include <vector>
#include <algorithm>
#include <numeric>
#include <cmath>

using namespace elio;
using namespace std::chrono;

// Minimum benchmark duration
constexpr auto MIN_BENCH_DURATION = seconds(10);

// Statistics helper
struct bench_stats {
    double avg;
    double min;
    double max;
    double stddev;
    size_t samples;
    double total_ops;
    double total_time_sec;
    
    static bench_stats compute(const std::vector<double>& data) {
        bench_stats s{};
        if (data.empty()) return s;
        
        s.samples = data.size();
        s.min = *std::min_element(data.begin(), data.end());
        s.max = *std::max_element(data.begin(), data.end());
        s.avg = std::accumulate(data.begin(), data.end(), 0.0) / data.size();
        
        double variance = 0.0;
        for (double v : data) {
            variance += (v - s.avg) * (v - s.avg);
        }
        s.stddev = std::sqrt(variance / data.size());
        
        return s;
    }
};

// Empty coroutine for measuring spawn overhead
coro::task<void> empty_task() {
    co_return;
}

// Coroutine that does minimal work
coro::task<int> compute_task(int value) {
    co_return value * 2;
}

// Time-based spawn overhead benchmark
void benchmark_spawn_overhead() {
    const int batch_size = 10000;
    std::vector<double> samples;  // ns per task for each batch
    size_t total_tasks = 0;
    
    std::cout << "  Running for at least " << duration_cast<seconds>(MIN_BENCH_DURATION).count() 
              << " seconds..." << std::endl;
    
    auto bench_start = high_resolution_clock::now();
    
    while (duration_cast<seconds>(high_resolution_clock::now() - bench_start) < MIN_BENCH_DURATION) {
        runtime::scheduler sched(4);
        sched.start();
        
        auto batch_start = high_resolution_clock::now();
        
        for (int i = 0; i < batch_size; ++i) {
            auto t = empty_task();
            sched.spawn(t.release());
        }
        
        // Wait for all to complete
        while (sched.pending_tasks() > 0) {
            std::this_thread::sleep_for(microseconds(1));
        }
        
        auto batch_end = high_resolution_clock::now();
        auto batch_ns = duration_cast<nanoseconds>(batch_end - batch_start).count();
        
        samples.push_back(static_cast<double>(batch_ns) / batch_size);
        total_tasks += batch_size;
        
        sched.shutdown();
    }
    
    auto bench_end = high_resolution_clock::now();
    auto total_sec = duration_cast<milliseconds>(bench_end - bench_start).count() / 1000.0;
    
    auto stats = bench_stats::compute(samples);
    
    std::cout << std::endl;
    std::cout << "Task Spawn Overhead Results:" << std::endl;
    std::cout << "  Duration:    " << std::fixed << std::setprecision(2) << total_sec << " seconds" << std::endl;
    std::cout << "  Total tasks: " << total_tasks << std::endl;
    std::cout << "  Batches:     " << samples.size() << " x " << batch_size << " tasks" << std::endl;
    std::cout << "  Average:     " << std::fixed << std::setprecision(2) << stats.avg << " ns/task" << std::endl;
    std::cout << "  Min:         " << std::fixed << std::setprecision(2) << stats.min << " ns/task" << std::endl;
    std::cout << "  Max:         " << std::fixed << std::setprecision(2) << stats.max << " ns/task" << std::endl;
    std::cout << "  StdDev:      " << std::fixed << std::setprecision(2) << stats.stddev << " ns" << std::endl;
    std::cout << "  Throughput:  " << std::fixed << std::setprecision(0) 
              << (total_tasks / total_sec) << " tasks/sec" << std::endl;
    std::cout << std::endl;
}

// Time-based context switch benchmark
void benchmark_context_switch() {
    const int batch_size = 5000;
    const int awaits_per_task = 10;
    std::vector<double> samples;  // ns per switch for each batch
    size_t total_switches = 0;
    
    std::cout << "  Running for at least " << duration_cast<seconds>(MIN_BENCH_DURATION).count() 
              << " seconds..." << std::endl;
    
    auto bench_start = high_resolution_clock::now();
    
    while (duration_cast<seconds>(high_resolution_clock::now() - bench_start) < MIN_BENCH_DURATION) {
        runtime::scheduler sched(4);
        sched.start();
        
        std::atomic<int> completed{0};
        
        auto task_with_await = [&]() -> coro::task<void> {
            for (int i = 0; i < awaits_per_task; ++i) {
                int value = co_await compute_task(i);
                (void)value;
            }
            completed.fetch_add(1, std::memory_order_relaxed);
            co_return;
        };
        
        auto batch_start = high_resolution_clock::now();
        
        for (int i = 0; i < batch_size; ++i) {
            auto t = task_with_await();
            sched.spawn(t.release());
        }
        
        while (completed.load(std::memory_order_relaxed) < batch_size) {
            std::this_thread::sleep_for(microseconds(1));
        }
        
        auto batch_end = high_resolution_clock::now();
        auto batch_ns = duration_cast<nanoseconds>(batch_end - batch_start).count();
        
        int batch_switches = batch_size * awaits_per_task;
        samples.push_back(static_cast<double>(batch_ns) / batch_switches);
        total_switches += batch_switches;
        
        sched.shutdown();
    }
    
    auto bench_end = high_resolution_clock::now();
    auto total_sec = duration_cast<milliseconds>(bench_end - bench_start).count() / 1000.0;
    
    auto stats = bench_stats::compute(samples);
    
    std::cout << std::endl;
    std::cout << "Context Switch Performance Results:" << std::endl;
    std::cout << "  Duration:       " << std::fixed << std::setprecision(2) << total_sec << " seconds" << std::endl;
    std::cout << "  Total switches: " << total_switches << std::endl;
    std::cout << "  Batches:        " << samples.size() << " x " << (batch_size * awaits_per_task) << " switches" << std::endl;
    std::cout << "  Average:        " << std::fixed << std::setprecision(2) << stats.avg << " ns/switch" << std::endl;
    std::cout << "  Min:            " << std::fixed << std::setprecision(2) << stats.min << " ns/switch" << std::endl;
    std::cout << "  Max:            " << std::fixed << std::setprecision(2) << stats.max << " ns/switch" << std::endl;
    std::cout << "  StdDev:         " << std::fixed << std::setprecision(2) << stats.stddev << " ns" << std::endl;
    std::cout << "  Throughput:     " << std::fixed << std::setprecision(0) 
              << (total_switches / total_sec) << " switches/sec" << std::endl;
    std::cout << std::endl;
}

// Time-based yield benchmark - simulates vthreads calling yield
// Uses single worker thread to measure pure yield overhead without work-stealing contention
void benchmark_yield() {
    const int yields_per_vthread = 1000;
    
    std::cout << "Yield Benchmark (single worker, each vthread count runs for " 
              << duration_cast<seconds>(MIN_BENCH_DURATION).count() << "+ seconds):" << std::endl;
    std::cout << std::endl;
    
    for (int num_vthreads : {2, 100, 1000}) {
        std::vector<double> samples;  // ns per yield for each batch
        size_t total_yields = 0;
        
        std::cout << "  " << num_vthreads << " vthreads: " << std::flush;
        
        auto bench_start = steady_clock::now();
        
        while (duration_cast<seconds>(steady_clock::now() - bench_start) < MIN_BENCH_DURATION) {
            runtime::scheduler sched(1);  // Single worker thread
            sched.start();
            
            std::atomic<int> completed{0};
            std::atomic<int64_t> end_time_ns{0};  // Last task records end timestamp
            
            // Each vthread yields multiple times
            auto yield_task = [&]() -> coro::task<void> {
                for (int i = 0; i < yields_per_vthread; ++i) {
                    co_await time::yield();
                }
                // Last task to complete records the end timestamp
                if (completed.fetch_add(1, std::memory_order_acq_rel) == num_vthreads - 1) {
                    end_time_ns.store(
                        duration_cast<nanoseconds>(steady_clock::now().time_since_epoch()).count(),
                        std::memory_order_release);
                }
                co_return;
            };
            
            // Capture start time in main thread
            auto start_time_ns = duration_cast<nanoseconds>(
                steady_clock::now().time_since_epoch()).count();
            
            // Spawn all vthreads
            for (int i = 0; i < num_vthreads; ++i) {
                auto t = yield_task();
                sched.spawn(t.release());
            }
            
            // Wait for end_time_ns to be set (spin-wait for accuracy)
            while (end_time_ns.load(std::memory_order_acquire) == 0) {
                // Spin without yielding for accurate measurement
            }
            
            // Calculate duration in main thread
            auto batch_ns = end_time_ns.load(std::memory_order_acquire) - start_time_ns;
            
            int batch_yields = num_vthreads * yields_per_vthread;
            samples.push_back(static_cast<double>(batch_ns) / batch_yields);
            total_yields += batch_yields;
            
            sched.shutdown();
        }
        
        auto bench_end = steady_clock::now();
        auto total_sec = duration_cast<milliseconds>(bench_end - bench_start).count() / 1000.0;
        
        auto stats = bench_stats::compute(samples);
        
        std::cout << std::fixed << std::setprecision(1) << total_sec << "s, "
                  << total_yields << " yields, "
                  << std::setprecision(2) << "avg=" << stats.avg << " ns/yield "
                  << "(min=" << stats.min << ", max=" << stats.max 
                  << ", stddev=" << stats.stddev << ")" << std::endl;
    }
    
    std::cout << std::endl;
}

// Time-based work stealing benchmark
void benchmark_work_stealing() {
    const int batch_size = 1000;
    std::vector<double> throughput_samples;
    std::vector<size_t> total_per_worker(4, 0);
    size_t total_tasks = 0;
    
    std::cout << "  Running for at least " << duration_cast<seconds>(MIN_BENCH_DURATION).count() 
              << " seconds with 4 workers..." << std::endl;
    
    auto bench_start = high_resolution_clock::now();
    
    while (duration_cast<seconds>(high_resolution_clock::now() - bench_start) < MIN_BENCH_DURATION) {
        runtime::scheduler sched(4);
        sched.start();
        
        std::atomic<int> completed{0};
        
        // Record initial per-worker task counts
        std::vector<size_t> initial_counts(4);
        for (size_t i = 0; i < 4; ++i) {
            initial_counts[i] = sched.worker_tasks_executed(i);
        }
        
        auto heavy_task = [&]() -> coro::task<void> {
            volatile int sum = 0;
            for (int i = 0; i < 10000; ++i) {
                sum = sum + i * i;
            }
            (void)sum;
            completed.fetch_add(1, std::memory_order_relaxed);
            co_return;
        };
        
        auto batch_start = high_resolution_clock::now();
        
        // Spawn ALL tasks to worker 0 to test work stealing
        for (int i = 0; i < batch_size; ++i) {
            auto t = heavy_task();
            sched.spawn_to(0, t.release());
        }
        
        while (completed.load(std::memory_order_relaxed) < batch_size) {
            std::this_thread::sleep_for(microseconds(1));
        }
        
        auto batch_end = high_resolution_clock::now();
        auto batch_us = duration_cast<microseconds>(batch_end - batch_start).count();
        
        double throughput = (batch_us > 0) ? (batch_size * 1000000.0) / batch_us : 0;
        throughput_samples.push_back(throughput);
        total_tasks += batch_size;
        
        // Accumulate per-worker counts
        for (size_t i = 0; i < 4; ++i) {
            total_per_worker[i] += sched.worker_tasks_executed(i) - initial_counts[i];
        }
        
        sched.shutdown();
    }
    
    auto bench_end = high_resolution_clock::now();
    auto total_sec = duration_cast<milliseconds>(bench_end - bench_start).count() / 1000.0;
    
    auto stats = bench_stats::compute(throughput_samples);
    
    std::cout << std::endl;
    std::cout << "Work Stealing Performance Results:" << std::endl;
    std::cout << "  Duration:           " << std::fixed << std::setprecision(2) << total_sec << " seconds" << std::endl;
    std::cout << "  Total tasks:        " << total_tasks << std::endl;
    std::cout << "  Batches:            " << throughput_samples.size() << std::endl;
    std::cout << "  Avg throughput:     " << std::fixed << std::setprecision(0) << stats.avg << " tasks/sec" << std::endl;
    std::cout << "  Min throughput:     " << std::fixed << std::setprecision(0) << stats.min << " tasks/sec" << std::endl;
    std::cout << "  Max throughput:     " << std::fixed << std::setprecision(0) << stats.max << " tasks/sec" << std::endl;
    std::cout << "  StdDev:             " << std::fixed << std::setprecision(0) << stats.stddev << " tasks/sec" << std::endl;
    std::cout << "  Per-worker totals (all spawned to worker 0):" << std::endl;
    for (size_t i = 0; i < 4; ++i) {
        double pct = (total_tasks > 0) ? (100.0 * total_per_worker[i] / total_tasks) : 0;
        std::cout << "    Worker " << i << ": " << total_per_worker[i] 
                  << " tasks (" << std::fixed << std::setprecision(1) << pct << "%)" << std::endl;
    }
    std::cout << std::endl;
}

// Time-based scalability benchmark
// This benchmark tests parallel CPU-bound workloads.
// Each task performs a larger computation to ensure work dominates over scheduling overhead.
void benchmark_scalability() {
    std::cout << "Scalability Benchmark (each thread count runs for "
              << duration_cast<seconds>(MIN_BENCH_DURATION).count() << "+ seconds):" << std::endl;
    std::cout << std::endl;

    const int tasks_per_thread = 500;   // Tasks per worker thread
    const int work_iterations = 100000; // More work per task to make CPU-bound

    for (size_t num_threads : {1, 2, 4, 8}) {
        std::vector<double> throughput_samples;
        size_t total_tasks = 0;
        int batch_size = tasks_per_thread * static_cast<int>(num_threads);

        std::cout << "  " << num_threads << " thread(s): " << std::flush;

        auto bench_start = high_resolution_clock::now();

        while (duration_cast<seconds>(high_resolution_clock::now() - bench_start) < MIN_BENCH_DURATION) {
            runtime::scheduler sched(num_threads);
            sched.start();

            std::atomic<int> completed{0};

            auto task_func = [&]() -> coro::task<void> {
                // Larger CPU-bound work to minimize scheduling overhead ratio
                volatile int sum = 0;
                for (int i = 0; i < work_iterations; ++i) {
                    sum = sum + i * i;
                }
                (void)sum;
                completed.fetch_add(1, std::memory_order_relaxed);
                co_return;
            };

            auto batch_start = high_resolution_clock::now();

            // Distribute tasks evenly across workers for true parallel scaling test
            for (int i = 0; i < batch_size; ++i) {
                auto t = task_func();
                sched.spawn(t.release());  // Round-robin distribution
            }

            while (completed.load(std::memory_order_relaxed) < batch_size) {
                std::this_thread::sleep_for(microseconds(1));
            }

            auto batch_end = high_resolution_clock::now();
            auto batch_us = duration_cast<microseconds>(batch_end - batch_start).count();

            double throughput = (batch_us > 0) ? (batch_size * 1000000.0) / batch_us : 0;
            throughput_samples.push_back(throughput);
            total_tasks += batch_size;

            sched.shutdown();
        }

        auto bench_end = high_resolution_clock::now();
        auto total_sec = duration_cast<milliseconds>(bench_end - bench_start).count() / 1000.0;

        auto stats = bench_stats::compute(throughput_samples);

        std::cout << std::fixed << std::setprecision(1) << total_sec << "s, "
                  << total_tasks << " tasks, "
                  << std::setprecision(0) << "avg=" << stats.avg << " tasks/sec "
                  << "(min=" << stats.min << ", max=" << stats.max
                  << ", stddev=" << stats.stddev << ")" << std::endl;
    }

    std::cout << std::endl;
}

int main() {
    // Minimal logging for benchmarks
    log::logger::instance().set_level(log::level::error);
    
    std::cout << "=== Elio Performance Benchmark ===" << std::endl;
    std::cout << "Each benchmark runs for at least " 
              << duration_cast<seconds>(MIN_BENCH_DURATION).count() << " seconds" << std::endl;
    std::cout << std::endl;
    
    // Run benchmarks
    {
        std::cout << "--- Benchmark 1: Task Spawn Overhead ---" << std::endl;
        benchmark_spawn_overhead();
    }
    
    {
        std::cout << "--- Benchmark 2: Context Switch Performance ---" << std::endl;
        benchmark_context_switch();
    }
    
    {
        std::cout << "--- Benchmark 3: Yield Performance ---" << std::endl;
        benchmark_yield();
    }
    
    {
        std::cout << "--- Benchmark 4: Work Stealing Performance ---" << std::endl;
        benchmark_work_stealing();
    }
    
    std::cout << "--- Benchmark 5: Scalability ---" << std::endl;
    benchmark_scalability();
    
    std::cout << "=== Benchmarks completed ===" << std::endl;
    
    return 0;
}
