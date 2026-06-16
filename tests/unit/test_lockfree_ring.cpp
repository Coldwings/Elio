#include <catch2/catch_test_macros.hpp>
#include <elio/sync/lockfree_ring.hpp>
#include <thread>
#include <vector>
#include <atomic>
#include <chrono>

using namespace elio;

TEST_CASE("LockfreeMPMCRing basic operations", "[common][lockfree_ring]") {
    SECTION("empty ring") {
        sync::LockfreeMPMCRing<int> ring(4);
        REQUIRE(ring.empty());
        REQUIRE(ring.size() == 0);
        auto val = ring.try_pop();
        REQUIRE_FALSE(val.has_value());
    }

    SECTION("single push and pop") {
        sync::LockfreeMPMCRing<int> ring(4);
        REQUIRE(ring.try_push(42));
        REQUIRE_FALSE(ring.empty());
        REQUIRE(ring.size() == 1);

        auto val = ring.try_pop();
        REQUIRE(val.has_value());
        REQUIRE(*val == 42);
        REQUIRE(ring.empty());
    }

    SECTION("fill to capacity") {
        sync::LockfreeMPMCRing<int> ring(4);
        REQUIRE(ring.try_push(1));
        REQUIRE(ring.try_push(2));
        REQUIRE(ring.try_push(3));
        REQUIRE(ring.try_push(4));
        REQUIRE(ring.size() == 4);

        // Ring should be full
        REQUIRE_FALSE(ring.try_push(5));
        REQUIRE(ring.size() == 4);
    }

    SECTION("FIFO order") {
        sync::LockfreeMPMCRing<int> ring(8);
        for (int i = 0; i < 5; ++i) {
            REQUIRE(ring.try_push(i));
        }

        for (int i = 0; i < 5; ++i) {
            auto val = ring.try_pop();
            REQUIRE(val.has_value());
            REQUIRE(*val == i);
        }
    }

    SECTION("wraparound") {
        sync::LockfreeMPMCRing<int> ring(4);

        // Fill and drain multiple times to test wraparound
        for (int round = 0; round < 3; ++round) {
            for (int i = 0; i < 4; ++i) {
                REQUIRE(ring.try_push(round * 10 + i));
            }
            for (int i = 0; i < 4; ++i) {
                auto val = ring.try_pop();
                REQUIRE(val.has_value());
                REQUIRE(*val == round * 10 + i);
            }
            REQUIRE(ring.empty());
        }
    }
}

TEST_CASE("LockfreeMPMCRing capacity handling", "[common][lockfree_ring]") {
    SECTION("capacity rounds up to power of 2") {
        sync::LockfreeMPMCRing<int> ring3(3);
        sync::LockfreeMPMCRing<int> ring4(4);
        sync::LockfreeMPMCRing<int> ring5(5);
        sync::LockfreeMPMCRing<int> ring7(7);
        sync::LockfreeMPMCRing<int> ring8(8);

        // All should have actual capacity >= requested
        REQUIRE(ring3.capacity() >= 3);
        REQUIRE(ring4.capacity() >= 4);
        REQUIRE(ring5.capacity() >= 5);
        REQUIRE(ring7.capacity() >= 7);
        REQUIRE(ring8.capacity() >= 8);

        // Verify they are powers of 2
        REQUIRE((ring3.capacity() & (ring3.capacity() - 1)) == 0);
        REQUIRE((ring4.capacity() & (ring4.capacity() - 1)) == 0);
        REQUIRE((ring5.capacity() & (ring5.capacity() - 1)) == 0);
        REQUIRE((ring7.capacity() & (ring7.capacity() - 1)) == 0);
        REQUIRE((ring8.capacity() & (ring8.capacity() - 1)) == 0);
    }
}

TEST_CASE("LockfreeMPMCRing concurrent SPSC", "[common][lockfree_ring][concurrent]") {
    sync::LockfreeMPMCRing<int> ring(1024);
    constexpr int NUM_ITEMS = 10000;
    std::atomic<bool> producer_done{false};
    std::atomic<int> items_received{0};

    std::thread producer([&]() {
        for (int i = 0; i < NUM_ITEMS; ++i) {
            while (!ring.try_push(i)) {
                std::this_thread::yield();
            }
        }
        producer_done.store(true, std::memory_order_release);
    });

    std::thread consumer([&]() {
        int received = 0;
        while (received < NUM_ITEMS) {
            auto val = ring.try_pop();
            if (val.has_value()) {
                REQUIRE(*val == received);
                received++;
            } else {
                std::this_thread::yield();
            }
        }
        items_received.store(received, std::memory_order_release);
    });

    producer.join();
    consumer.join();

    REQUIRE(producer_done.load());
    REQUIRE(items_received.load() == NUM_ITEMS);
}

TEST_CASE("LockfreeMPMCRing concurrent MPMC", "[common][lockfree_ring][concurrent]") {
    sync::LockfreeMPMCRing<int> ring(256);
    constexpr int NUM_PRODUCERS = 4;
    constexpr int NUM_CONSUMERS = 4;
    constexpr int ITEMS_PER_PRODUCER = 1000;
    constexpr int TOTAL_ITEMS = NUM_PRODUCERS * ITEMS_PER_PRODUCER;

    std::atomic<int> total_produced{0};
    std::atomic<int> total_consumed{0};

    std::vector<std::thread> threads;

    // Spawn producers
    for (int p = 0; p < NUM_PRODUCERS; ++p) {
        threads.emplace_back([&, p]() {
            for (int i = 0; i < ITEMS_PER_PRODUCER; ++i) {
                int value = p * ITEMS_PER_PRODUCER + i;
                while (!ring.try_push(value)) {
                    std::this_thread::yield();
                }
                total_produced.fetch_add(1, std::memory_order_relaxed);
            }
        });
    }

    // Spawn consumers
    for (int c = 0; c < NUM_CONSUMERS; ++c) {
        threads.emplace_back([&]() {
            int consumed = 0;
            while (total_consumed.load(std::memory_order_acquire) < TOTAL_ITEMS || !ring.empty()) {
                auto val = ring.try_pop();
                if (val.has_value()) {
                    consumed++;
                    total_consumed.fetch_add(1, std::memory_order_release);
                } else {
                    if (total_produced.load(std::memory_order_acquire) == TOTAL_ITEMS && ring.empty()) {
                        break;
                    }
                    std::this_thread::yield();
                }
            }
        });
    }

    for (auto& t : threads) {
        t.join();
    }

    REQUIRE(total_produced.load() == TOTAL_ITEMS);
    REQUIRE(total_consumed.load() == TOTAL_ITEMS);
}

TEST_CASE("LockfreeMPMCRing stress test", "[common][lockfree_ring][stress]") {
    sync::LockfreeMPMCRing<int> ring(64);  // Small capacity to increase contention
    constexpr int NUM_ITERATIONS = 100000;
    std::atomic<int> sum_produced{0};
    std::atomic<int> sum_consumed{0};
    std::atomic<bool> done{false};

    std::thread producer([&]() {
        for (int i = 1; i <= NUM_ITERATIONS; ++i) {
            while (!ring.try_push(i)) {
                std::this_thread::yield();
            }
            sum_produced.fetch_add(i, std::memory_order_relaxed);
        }
        done.store(true, std::memory_order_release);
    });

    std::thread consumer([&]() {
        while (!done.load(std::memory_order_acquire) || !ring.empty()) {
            auto val = ring.try_pop();
            if (val.has_value()) {
                sum_consumed.fetch_add(*val, std::memory_order_relaxed);
            } else {
                std::this_thread::yield();
            }
        }
    });

    producer.join();
    consumer.join();

    int expected_sum = (NUM_ITERATIONS * (NUM_ITERATIONS + 1)) / 2;
    REQUIRE(sum_produced.load() == expected_sum);
    REQUIRE(sum_consumed.load() == expected_sum);
}
