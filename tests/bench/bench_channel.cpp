#include <catch2/catch_test_macros.hpp>
#include <catch2/benchmark/catch_benchmark.hpp>
#include <elio/sync/channel.hpp>
#include <elio/coro/task.hpp>
#include <elio/runtime/scheduler.hpp>

#include <atomic>
#include <chrono>
#include <vector>

using namespace elio::sync;
using namespace elio::coro;
using namespace elio::runtime;

// ---------------------------------------------------------------------------
// 1. Synchronous try_send / try_recv throughput (no coroutines, no scheduler)
//    Measures raw channel primitive overhead.
// ---------------------------------------------------------------------------

TEST_CASE("channel benchmark: try_send/try_recv throughput", "[benchmark][channel]") {
    constexpr size_t OPS = 100'000;

    SECTION("rendezvous channel (capacity=0)") {
        // Rendezvous try_send only succeeds when a receiver is waiting.
        // bounded(1) acts as a synchronous single-slot ping-pong proxy.
        BENCHMARK("bounded(1) try_send+try_recv ping-pong") {
            channel<int> ch(1);
            size_t received = 0;
            for (size_t i = 0; i < OPS; ++i) {
                ch.try_send(static_cast<int>(i));
                auto v = ch.try_recv();
                if (v.has_value()) ++received;
            }
            return received;
        };
    }

    SECTION("bounded channel (capacity=64)") {
        BENCHMARK("bounded(64) try_send+try_recv") {
            channel<int> ch(64);
            size_t received = 0;
            for (size_t i = 0; i < OPS; ++i) {
                ch.try_send(static_cast<int>(i));
                auto v = ch.try_recv();
                if (v.has_value()) ++received;
            }
            return received;
        };
    }

    SECTION("unbounded channel") {
        BENCHMARK("unbounded try_send+try_recv") {
            auto ch = channel<int>::unbounded();
            size_t received = 0;
            for (size_t i = 0; i < OPS; ++i) {
                ch.try_send(static_cast<int>(i));
                auto v = ch.try_recv();
                if (v.has_value()) ++received;
            }
            return received;
        };
    }
}

// ---------------------------------------------------------------------------
// 2. Coroutine SPSC throughput: 1 producer, 1 consumer
// ---------------------------------------------------------------------------

TEST_CASE("channel benchmark: SPSC coroutine throughput", "[benchmark][channel]") {
    constexpr size_t OPS = 50'000;

    auto bench_spsc = [](size_t capacity, size_t ops) -> double {
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

        auto p = sched.go_joinable(producer, ops);
        auto c = sched.go_joinable(consumer, ops);

        auto t0 = std::chrono::steady_clock::now();
        p.wait_destroyed();
        c.wait_destroyed();
        auto t1 = std::chrono::steady_clock::now();

        sched.shutdown();

        return std::chrono::duration<double, std::micro>(t1 - t0).count();
    };

    BENCHMARK("SPSC rendezvous (cap=0)") {
        return bench_spsc(0, OPS);
    };

    BENCHMARK("SPSC bounded(1)") {
        return bench_spsc(1, OPS);
    };

    BENCHMARK("SPSC bounded(64)") {
        return bench_spsc(64, OPS);
    };

    BENCHMARK("SPSC unbounded") {
        return bench_spsc(std::numeric_limits<size_t>::max(), OPS);
    };
}

// ---------------------------------------------------------------------------
// 3. Coroutine MPMC throughput: N producers, N consumers
// ---------------------------------------------------------------------------

TEST_CASE("channel benchmark: MPMC coroutine throughput", "[benchmark][channel]") {
    auto bench_mpmc = [](size_t capacity) -> double {
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

        auto t0 = std::chrono::steady_clock::now();

        // Wait for consumers to drain
        for (size_t i = NUM_P; i < joins.size(); ++i) {
            joins[i].wait_destroyed();
        }

        auto t1 = std::chrono::steady_clock::now();

        sched.shutdown();

        return std::chrono::duration<double, std::micro>(t1 - t0).count();
    };

    BENCHMARK("MPMC 4x4 rendezvous (cap=0)") {
        return bench_mpmc(0);
    };

    BENCHMARK("MPMC 4x4 bounded(1)") {
        return bench_mpmc(1);
    };

    BENCHMARK("MPMC 4x4 bounded(64)") {
        return bench_mpmc(64);
    };

    BENCHMARK("MPMC 4x4 unbounded") {
        return bench_mpmc(std::numeric_limits<size_t>::max());
    };
}

// ---------------------------------------------------------------------------
// 4. Contention scalability: vary thread count with fixed workload
// ---------------------------------------------------------------------------

TEST_CASE("channel benchmark: contention scalability", "[benchmark][channel]") {
    constexpr size_t TOTAL_OPS = 100'000;

    // Benchmark with a given number of producer/consumer pairs on a
    // bounded(64) channel.
    auto bench_scalability = [](size_t num_pairs) -> double {
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

        // Use enough threads to cover all coroutines
        const size_t num_threads = std::max(num_pairs * 2, size_t{2});
        scheduler sched(num_threads);
        sched.start();

        std::vector<join_handle<void>> joins;
        joins.reserve(num_pairs * 2);

        for (size_t i = 0; i < num_pairs; ++i) {
            joins.push_back(sched.go_joinable(producer, ops_per_producer));
        }
        for (size_t i = 0; i < num_pairs; ++i) {
            joins.push_back(sched.go_joinable(consumer_fn, size_t{0}));
        }

        auto t0 = std::chrono::steady_clock::now();

        // Wait for producers
        for (size_t i = 0; i < num_pairs; ++i) {
            joins[i].wait_destroyed();
        }
        ch.close();

        // Wait for consumers
        for (size_t i = num_pairs; i < joins.size(); ++i) {
            joins[i].wait_destroyed();
        }

        auto t1 = std::chrono::steady_clock::now();

        sched.shutdown();

        return std::chrono::duration<double, std::micro>(t1 - t0).count();
    };

    BENCHMARK("scalability 1 pair") {
        return bench_scalability(1);
    };

    BENCHMARK("scalability 2 pairs") {
        return bench_scalability(2);
    };

    BENCHMARK("scalability 4 pairs") {
        return bench_scalability(4);
    };

    BENCHMARK("scalability 8 pairs") {
        return bench_scalability(8);
    };
}

// ---------------------------------------------------------------------------
// 5. Channel type comparison: side-by-side summary
// ---------------------------------------------------------------------------

TEST_CASE("channel benchmark: type comparison summary", "[benchmark][channel]") {
    constexpr size_t OPS = 50'000;

    // Helper that runs a SPSC pipeline and returns elapsed us.
    auto spsc_time_us = [](size_t capacity, size_t ops) -> double {
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

        auto p = sched.go_joinable(producer, ops);
        auto c = sched.go_joinable(consumer, ops);

        auto t0 = std::chrono::steady_clock::now();
        p.wait_destroyed();
        c.wait_destroyed();
        auto t1 = std::chrono::steady_clock::now();

        sched.shutdown();

        return std::chrono::duration<double, std::micro>(t1 - t0).count();
    };

    BENCHMARK("comparison: rendezvous") {
        return spsc_time_us(0, OPS);
    };

    BENCHMARK("comparison: bounded(1)") {
        return spsc_time_us(1, OPS);
    };

    BENCHMARK("comparison: bounded(16)") {
        return spsc_time_us(16, OPS);
    };

    BENCHMARK("comparison: bounded(256)") {
        return spsc_time_us(256, OPS);
    };

    BENCHMARK("comparison: unbounded") {
        return spsc_time_us(std::numeric_limits<size_t>::max(), OPS);
    };
}
