#include <elio/sync/channel.hpp>
#include <elio/coro/task.hpp>
#include <elio/runtime/scheduler.hpp>
#include <elio/log/macros.hpp>
#include <iostream>
#include <iomanip>
#include <atomic>
#include <chrono>
#include <vector>
#include <algorithm>
#include <numeric>
#include <cmath>
#include <limits>

using namespace elio::sync;
using namespace elio::coro;
using namespace elio::runtime;
using namespace std::chrono;

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

void print_stats(const char* name, const bench_stats& stats, size_t ops, const char* unit) {
    double avg_per_op = stats.avg / ops;
    std::cout << std::setw(35) << std::left << name
              << std::setw(12) << std::right << std::fixed << std::setprecision(2) << stats.avg << " "
              << std::setw(10) << avg_per_op << " "
              << std::setw(10) << stats.min << " "
              << std::setw(10) << stats.max << " "
              << std::setw(10) << stats.stddev << " "
              << unit << "\n";
}

// ---------------------------------------------------------------------------
// 1. Synchronous try_send / try_recv throughput (no coroutines, no scheduler)
// ---------------------------------------------------------------------------

void bench_sync_throughput() {
    std::cout << "\n=== Synchronous try_send/try_recv Throughput ===\n";
    std::cout << std::setw(35) << std::left << "Scenario"
              << std::setw(12) << std::right << "Total (us)"
              << std::setw(10) << "Avg/op"
              << std::setw(10) << "Min (us)"
              << std::setw(10) << "Max (us)"
              << std::setw(10) << "StdDev"
              << " Unit\n";
    std::cout << std::string(97, '-') << "\n";

    constexpr size_t OPS = 100'000;
    constexpr size_t ITERATIONS = 5;

    // Bounded(1) - rendezvous-like
    {
        std::vector<double> times;
        for (size_t iter = 0; iter < ITERATIONS; ++iter) {
            channel<int> ch(1);
            size_t received = 0;
            auto t0 = steady_clock::now();
            for (size_t i = 0; i < OPS; ++i) {
                ch.try_send(static_cast<int>(i));
                auto v = ch.try_recv();
                if (v.has_value()) ++received;
            }
            auto t1 = steady_clock::now();
            times.push_back(duration<double, std::micro>(t1 - t0).count());
        }
        print_stats("bounded(1)", bench_stats::compute(times), OPS, "us");
    }

    // Bounded(64)
    {
        std::vector<double> times;
        for (size_t iter = 0; iter < ITERATIONS; ++iter) {
            channel<int> ch(64);
            size_t received = 0;
            auto t0 = steady_clock::now();
            for (size_t i = 0; i < OPS; ++i) {
                ch.try_send(static_cast<int>(i));
                auto v = ch.try_recv();
                if (v.has_value()) ++received;
            }
            auto t1 = steady_clock::now();
            times.push_back(duration<double, std::micro>(t1 - t0).count());
        }
        print_stats("bounded(64)", bench_stats::compute(times), OPS, "us");
    }

    // Unbounded
    {
        std::vector<double> times;
        for (size_t iter = 0; iter < ITERATIONS; ++iter) {
            auto ch = channel<int>::unbounded();
            size_t received = 0;
            auto t0 = steady_clock::now();
            for (size_t i = 0; i < OPS; ++i) {
                ch.try_send(static_cast<int>(i));
                auto v = ch.try_recv();
                if (v.has_value()) ++received;
            }
            auto t1 = steady_clock::now();
            times.push_back(duration<double, std::micro>(t1 - t0).count());
        }
        print_stats("unbounded", bench_stats::compute(times), OPS, "us");
    }
}

// ---------------------------------------------------------------------------
// 2. Coroutine SPSC throughput
// ---------------------------------------------------------------------------

double bench_spsc_coroutine(size_t capacity, size_t ops) {
    channel<int> ch(capacity);
    channel<int>* ch_ptr = &ch;

    auto producer = [ch_ptr](size_t n) -> task<void> {
        for (size_t i = 0; i < n; ++i) {
            co_await ch_ptr->send(static_cast<int>(i));
        }
        ch_ptr->close();
        co_return;
    };

    auto consumer = [ch_ptr](size_t /*n*/) -> task<void> {
        while (true) {
            auto val = co_await ch_ptr->recv();
            if (!val.has_value()) break;
        }
        co_return;
    };

    scheduler sched(2);
    sched.start();

    auto t0 = steady_clock::now();

    auto p = sched.go_joinable(producer, ops);
    auto c = sched.go_joinable(consumer, ops);

    p.wait_destroyed();
    c.wait_destroyed();
    auto t1 = steady_clock::now();

    sched.shutdown();

    return duration<double, std::micro>(t1 - t0).count();
}

void bench_spsc_throughput() {
    std::cout << "\n=== SPSC Coroutine Throughput (1 producer, 1 consumer) ===\n";
    std::cout << "NOTE: Low performance for rendezvous/bounded(1) is expected due to:\n";
    std::cout << "  - Coarse-grained mutex on every operation (send/recv both acquire same lock)\n";
    std::cout << "  - Rendezvous requires 3 mutex acquisitions per message + context switch\n";
    std::cout << "  - std::queue operations happen under lock\n";
    std::cout << "  - No parallelism to hide contention in SPSC pattern\n";
    std::cout << "  See channel.hpp for implementation details.\n";
    std::cout << std::setw(35) << std::left << "Scenario"
              << std::setw(12) << std::right << "Total (us)"
              << std::setw(10) << "Avg/op"
              << std::setw(10) << "Min (us)"
              << std::setw(10) << "Max (us)"
              << std::setw(10) << "StdDev"
              << " Unit\n";
    std::cout << std::string(97, '-') << "\n";

    constexpr size_t OPS = 50'000;
    constexpr size_t ITERATIONS = 5;

    auto run_bench = [&](const char* name, size_t capacity) {
        std::vector<double> times;
        for (size_t iter = 0; iter < ITERATIONS; ++iter) {
            times.push_back(bench_spsc_coroutine(capacity, OPS));
        }
        print_stats(name, bench_stats::compute(times), OPS, "us");
    };

    run_bench("rendezvous (cap=0)", 0);
    run_bench("bounded(1)", 1);
    run_bench("bounded(64)", 64);
    run_bench("unbounded", std::numeric_limits<size_t>::max());
}

// ---------------------------------------------------------------------------
// 3. Coroutine MPMC throughput
// ---------------------------------------------------------------------------

double bench_mpmc_coroutine(size_t capacity) {
    constexpr size_t NUM_P = 4;
    constexpr size_t NUM_C = 4;
    constexpr size_t OPS_PER_P = 12'500;  // 50k total ops

    channel<int> ch(capacity);
    channel<int>* ch_ptr = &ch;

    std::atomic<size_t> consumed{0};
    std::atomic<size_t>* consumed_ptr = &consumed;

    auto producer = [ch_ptr](size_t n) -> task<void> {
        for (size_t i = 0; i < n; ++i) {
            co_await ch_ptr->send(static_cast<int>(i));
        }
        co_return;
    };

    auto consumer_fn = [ch_ptr, consumed_ptr](size_t /*n*/) -> task<void> {
        while (true) {
            auto val = co_await ch_ptr->recv();
            if (!val.has_value()) break;
            consumed_ptr->fetch_add(1, std::memory_order_relaxed);
        }
        co_return;
    };

    scheduler sched(NUM_P + NUM_C);
    sched.start();

    std::vector<join_handle<void>> joins;
    joins.reserve(NUM_P + NUM_C);

    auto t0 = steady_clock::now();

    for (size_t i = 0; i < NUM_P; ++i) {
        joins.push_back(sched.go_joinable(producer, OPS_PER_P));
    }
    for (size_t i = 0; i < NUM_C; ++i) {
        joins.push_back(sched.go_joinable(consumer_fn, size_t{0}));
    }

    // Wait for all producers to finish, then close the channel
    for (size_t i = 0; i < NUM_P; ++i) {
        joins[i].wait_destroyed();
    }
    ch.close();

    // Wait for consumers to drain
    for (size_t i = NUM_P; i < joins.size(); ++i) {
        joins[i].wait_destroyed();
    }

    auto t1 = steady_clock::now();

    sched.shutdown();

    return duration<double, std::micro>(t1 - t0).count();
}

void bench_mpmc_throughput() {
    std::cout << "\n=== MPMC Coroutine Throughput (4 producers, 4 consumers) ===\n";
    std::cout << std::setw(35) << std::left << "Scenario"
              << std::setw(12) << std::right << "Total (us)"
              << std::setw(10) << "Avg/op"
              << std::setw(10) << "Min (us)"
              << std::setw(10) << "Max (us)"
              << std::setw(10) << "StdDev"
              << " Unit\n";
    std::cout << std::string(97, '-') << "\n";

    constexpr size_t ITERATIONS = 5;
    constexpr size_t TOTAL_OPS = 4 * 12'500;  // 4 producers * 12,500 ops each

    auto run_bench = [&](const char* name, size_t capacity) {
        std::vector<double> times;
        for (size_t iter = 0; iter < ITERATIONS; ++iter) {
            times.push_back(bench_mpmc_coroutine(capacity));
        }
        print_stats(name, bench_stats::compute(times), TOTAL_OPS, "us");
    };

    run_bench("rendezvous (cap=0)", 0);
    run_bench("bounded(1)", 1);
    run_bench("bounded(64)", 64);
    run_bench("unbounded", std::numeric_limits<size_t>::max());
}

// ---------------------------------------------------------------------------
// 4. Contention scalability
// ---------------------------------------------------------------------------

double bench_scalability(size_t num_pairs) {
    constexpr size_t TOTAL_OPS = 100'000;
    constexpr size_t CAPACITY = 64;
    channel<int> ch(CAPACITY);
    channel<int>* ch_ptr = &ch;

    const size_t ops_per_producer = TOTAL_OPS / num_pairs;

    std::atomic<size_t> consumed{0};
    std::atomic<size_t>* consumed_ptr = &consumed;

    auto producer = [ch_ptr](size_t n) -> task<void> {
        for (size_t i = 0; i < n; ++i) {
            co_await ch_ptr->send(static_cast<int>(i));
        }
        co_return;
    };

    auto consumer_fn = [ch_ptr, consumed_ptr](size_t /*n*/) -> task<void> {
        while (true) {
            auto val = co_await ch_ptr->recv();
            if (!val.has_value()) break;
            consumed_ptr->fetch_add(1, std::memory_order_relaxed);
        }
        co_return;
    };

    const size_t num_threads = std::max(num_pairs * 2, size_t{2});
    scheduler sched(num_threads);
    sched.start();

    std::vector<join_handle<void>> joins;
    joins.reserve(num_pairs * 2);

    auto t0 = steady_clock::now();

    for (size_t i = 0; i < num_pairs; ++i) {
        joins.push_back(sched.go_joinable(producer, ops_per_producer));
    }
    for (size_t i = 0; i < num_pairs; ++i) {
        joins.push_back(sched.go_joinable(consumer_fn, size_t{0}));
    }

    // Wait for producers
    for (size_t i = 0; i < num_pairs; ++i) {
        joins[i].wait_destroyed();
    }
    ch.close();

    // Wait for consumers
    for (size_t i = num_pairs; i < joins.size(); ++i) {
        joins[i].wait_destroyed();
    }

    auto t1 = steady_clock::now();

    sched.shutdown();

    return duration<double, std::micro>(t1 - t0).count();
}

void bench_contention_scalability() {
    std::cout << "\n=== Contention Scalability (bounded(64) channel) ===\n";
    std::cout << std::setw(35) << std::left << "Scenario"
              << std::setw(12) << std::right << "Total (us)"
              << std::setw(10) << "Avg/op"
              << std::setw(10) << "Min (us)"
              << std::setw(10) << "Max (us)"
              << std::setw(10) << "StdDev"
              << " Unit\n";
    std::cout << std::string(97, '-') << "\n";

    constexpr size_t ITERATIONS = 5;
    constexpr size_t TOTAL_OPS = 100'000;

    auto run_bench = [&](size_t num_pairs) {
        std::vector<double> times;
        for (size_t iter = 0; iter < ITERATIONS; ++iter) {
            times.push_back(bench_scalability(num_pairs));
        }
        char name[32];
        snprintf(name, sizeof(name), "%zu pairs", num_pairs);
        print_stats(name, bench_stats::compute(times), TOTAL_OPS, "us");
    };

    run_bench(1);
    run_bench(2);
    run_bench(4);
    run_bench(8);
}

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------

int main() {
    // Suppress verbose logs during benchmark
    elio::log::logger::instance().set_level(elio::log::level::error);

    std::cout << "=== Elio Channel Benchmark Suite ===\n";
    std::cout << "Measuring channel performance across different modes and contention levels.\n";

    bench_sync_throughput();
    bench_spsc_throughput();
    bench_mpmc_throughput();
    bench_contention_scalability();

    std::cout << "\n=== Benchmark Complete ===\n";
    return 0;
}
