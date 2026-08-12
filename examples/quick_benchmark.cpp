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
constexpr auto SPAWN_BATCH_TIMEOUT = seconds(30);

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

class scheduler_reschedule_awaitable {
public:
    explicit scheduler_reschedule_awaitable(runtime::scheduler* sched)
        : sched_(sched) {}

    [[nodiscard]] bool await_ready() const noexcept { return false; }

    [[nodiscard]] bool await_suspend(
        std::coroutine_handle<> handle) const noexcept {
        return sched_->try_schedule(handle);
    }

    void await_resume() const noexcept {}

private:
    runtime::scheduler* sched_;
};

coro::task<void> measure_scheduler_reschedules(
    runtime::scheduler* sched, int iterations,
    std::atomic<int64_t>* elapsed_ns, std::atomic<bool>* completed) {
    const auto start = steady_clock::now();
    for (int i = 0; i < iterations; ++i) {
        co_await scheduler_reschedule_awaitable{sched};
    }
    elapsed_ns->store(
        duration_cast<nanoseconds>(steady_clock::now() - start).count(),
        std::memory_order_relaxed);
    completed->store(true, std::memory_order_release);
    co_return;
}

coro::task<void> hold_worker_for_external_burst(
    std::atomic<bool>* started, std::atomic<bool>* release) {
    started->store(true, std::memory_order_release);
    while (!release->load(std::memory_order_acquire)) {
        std::this_thread::yield();
    }
    co_return;
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
            sched.go(empty_task);
        }

        if (!sched.wait_for_idle(SPAWN_BATCH_TIMEOUT)) {
            std::cerr << "Timed out waiting for callable-spawn batch"
                      << std::endl;
            std::abort();
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

    std::cout << "Task Spawn (callable): " << std::fixed << std::setprecision(2)
              << stats.avg << " ns/task (min=" << stats.min
              << ", max=" << stats.max << ")" << std::endl;
    std::cout << "  Throughput: " << std::fixed << std::setprecision(0)
              << (total_tasks / total_sec) << " tasks/sec" << std::endl;
}

// Same work with an already-constructed lazy task transferred directly. This
// isolates the callable-wrapper frame and control-state cost.
void benchmark_direct_task_spawn_overhead() {
    const int batch_size = 10000;
    std::vector<double> samples;
    size_t total_tasks = 0;

    auto bench_start = high_resolution_clock::now();

    while (duration_cast<seconds>(high_resolution_clock::now() - bench_start) <
           MIN_BENCH_DURATION) {
        runtime::scheduler sched(4);
        sched.start();

        auto batch_start = high_resolution_clock::now();

        for (int i = 0; i < batch_size; ++i) {
            sched.go(empty_task());
        }

        if (!sched.wait_for_idle(SPAWN_BATCH_TIMEOUT)) {
            std::cerr << "Timed out waiting for direct-spawn batch"
                      << std::endl;
            std::abort();
        }

        auto batch_end = high_resolution_clock::now();
        auto batch_ns = duration_cast<nanoseconds>(
            batch_end - batch_start).count();

        samples.push_back(static_cast<double>(batch_ns) / batch_size);
        total_tasks += batch_size;

        sched.shutdown();
    }

    auto bench_end = high_resolution_clock::now();
    auto total_sec = duration_cast<milliseconds>(
        bench_end - bench_start).count() / 1000.0;

    auto stats = bench_stats::compute(samples);

    std::cout << "Task Spawn (direct task): " << std::fixed
              << std::setprecision(2) << stats.avg
              << " ns/task (min=" << stats.min
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
            sched.go(task_with_await);
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
            sched.go(yield_task);
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

// Measure a suspended coroutine re-enqueuing itself through scheduler routing.
void benchmark_scheduler_reschedule() {
    constexpr int reschedules_per_task = 100000;
    std::vector<double> samples;

    const auto bench_start = steady_clock::now();
    while (duration_cast<seconds>(steady_clock::now() - bench_start) <
           MIN_BENCH_DURATION) {
        runtime::scheduler sched(1);
        sched.start();

        std::atomic<int64_t> elapsed_ns{0};
        std::atomic<bool> completed{false};
        sched.go_to(0, measure_scheduler_reschedules, &sched,
                    reschedules_per_task, &elapsed_ns, &completed);

        while (!completed.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }

        samples.push_back(
            static_cast<double>(elapsed_ns.load(std::memory_order_relaxed)) /
            reschedules_per_task);
        sched.shutdown();
    }

    const auto stats = bench_stats::compute(samples);
    std::cout << "Scheduler reschedule: " << std::fixed
              << std::setprecision(2) << stats.avg
              << " ns/reschedule (min=" << stats.min
              << ", max=" << stats.max << ")" << std::endl;
}

// Measure producer-side enqueue cost when many submissions target one busy
// worker and therefore share one outstanding wake notification.
void benchmark_external_submission_burst() {
    constexpr int submissions_per_burst = 4000;
    std::vector<double> samples;

    const auto bench_start = steady_clock::now();
    while (duration_cast<seconds>(steady_clock::now() - bench_start) <
           MIN_BENCH_DURATION) {
        runtime::scheduler sched(1);
        sched.start();

        std::atomic<bool> holder_started{false};
        std::atomic<bool> release_holder{false};
        sched.go_to(0, hold_worker_for_external_burst,
                    &holder_started, &release_holder);
        while (!holder_started.load(std::memory_order_acquire)) {
            std::this_thread::yield();
        }

        const auto start = steady_clock::now();
        for (int i = 0; i < submissions_per_burst; ++i) {
            sched.spawn_to(0, std::noop_coroutine());
        }
        const auto elapsed = duration_cast<nanoseconds>(
            steady_clock::now() - start).count();
        samples.push_back(
            static_cast<double>(elapsed) / submissions_per_burst);

        release_holder.store(true, std::memory_order_release);
        sched.shutdown();
    }

    const auto stats = bench_stats::compute(samples);
    std::cout << "External submission burst: " << std::fixed
              << std::setprecision(2) << stats.avg
              << " ns/submit (min=" << stats.min
              << ", max=" << stats.max << ")" << std::endl;
}

int main() {
    log::logger::instance().set_level(log::level::error);

    std::cout << "=== Quick Elio Benchmark ("
              << duration_cast<seconds>(MIN_BENCH_DURATION).count() << "s each) ===" << std::endl;

    benchmark_spawn_overhead();
    benchmark_direct_task_spawn_overhead();
    benchmark_context_switch();
    benchmark_yield();
    benchmark_scheduler_reschedule();
    benchmark_external_submission_burst();

    std::cout << "=== Done ===" << std::endl;

    return 0;
}
