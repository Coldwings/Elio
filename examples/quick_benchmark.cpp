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

// Minimum benchmark duration - reduced for quick testing
constexpr auto MIN_BENCH_DURATION = seconds(3);

// Statistics helper
struct bench_stats {
    double avg;
    double min;
    double max;
    double stddev;
    size_t samples;

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
    std::vector<double> samples;
    size_t total_tasks = 0;

    auto bench_start = high_resolution_clock::now();

    while (duration_cast<seconds>(high_resolution_clock::now() - bench_start) < MIN_BENCH_DURATION) {
        runtime::scheduler sched(4);
        sched.start();

        auto batch_start = high_resolution_clock::now();

        for (int i = 0; i < batch_size; ++i) {
            auto t = empty_task();
            sched.spawn(t.release());
        }

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

    std::cout << "Task Spawn: " << std::fixed << std::setprecision(2)
              << stats.avg << " ns/task (min=" << stats.min
              << ", max=" << stats.max << ")" << std::endl;
    std::cout << "  Throughput: " << std::fixed << std::setprecision(0)
              << (total_tasks / total_sec) << " tasks/sec" << std::endl;
}

// Time-based context switch benchmark
void benchmark_context_switch() {
    const int batch_size = 5000;
    const int awaits_per_task = 10;
    std::vector<double> samples;
    size_t total_switches = 0;

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

    std::cout << "Context Switch: " << std::fixed << std::setprecision(2)
              << stats.avg << " ns/switch (min=" << stats.min
              << ", max=" << stats.max << ")" << std::endl;
    std::cout << "  Throughput: " << std::fixed << std::setprecision(0)
              << (total_switches / total_sec) << " switches/sec" << std::endl;
}

// Time-based yield benchmark
void benchmark_yield() {
    const int yields_per_vthread = 1000;
    const int num_vthreads = 1000;

    std::vector<double> samples;
    size_t total_yields = 0;

    auto bench_start = steady_clock::now();

    while (duration_cast<seconds>(steady_clock::now() - bench_start) < MIN_BENCH_DURATION) {
        runtime::scheduler sched(1);
        sched.start();

        std::atomic<int> completed{0};
        std::atomic<int64_t> end_time_ns{0};

        auto yield_task = [&]() -> coro::task<void> {
            for (int i = 0; i < yields_per_vthread; ++i) {
                co_await time::yield();
            }
            if (completed.fetch_add(1, std::memory_order_acq_rel) == num_vthreads - 1) {
                end_time_ns.store(
                    duration_cast<nanoseconds>(steady_clock::now().time_since_epoch()).count(),
                    std::memory_order_release);
            }
            co_return;
        };

        auto start_time_ns = duration_cast<nanoseconds>(
            steady_clock::now().time_since_epoch()).count();

        for (int i = 0; i < num_vthreads; ++i) {
            auto t = yield_task();
            sched.spawn(t.release());
        }

        while (end_time_ns.load(std::memory_order_acquire) == 0) {}

        auto batch_ns = end_time_ns.load(std::memory_order_acquire) - start_time_ns;
        int batch_yields = num_vthreads * yields_per_vthread;
        samples.push_back(static_cast<double>(batch_ns) / batch_yields);
        total_yields += batch_yields;

        sched.shutdown();
    }

    auto stats = bench_stats::compute(samples);

    std::cout << "Yield (1000 vthreads): " << std::fixed << std::setprecision(2)
              << stats.avg << " ns/yield (min=" << stats.min
              << ", max=" << stats.max << ")" << std::endl;
}

int main() {
    log::logger::instance().set_level(log::level::error);

    std::cout << "=== Quick Elio Benchmark ("
              << duration_cast<seconds>(MIN_BENCH_DURATION).count() << "s each) ===" << std::endl;

    benchmark_spawn_overhead();
    benchmark_context_switch();
    benchmark_yield();

    std::cout << "=== Done ===" << std::endl;

    return 0;
}
