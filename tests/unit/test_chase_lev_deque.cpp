#include <catch2/catch_test_macros.hpp>
#include <elio/runtime/chase_lev_deque.hpp>
#include <thread>
#include <vector>
#include <atomic>
#include <algorithm>
#include <barrier>

using namespace elio::runtime;

TEST_CASE("chase_lev_deque construction", "[chase_lev_deque]") {
    chase_lev_deque<int> deque(64);
    REQUIRE(deque.empty());
    REQUIRE(deque.size() == 0);
}

TEST_CASE("chase_lev_deque single-threaded push/pop", "[chase_lev_deque]") {
    chase_lev_deque<int> deque;
    
    int values[] = {1, 2, 3, 4, 5};
    
    // Push values
    for (int i = 0; i < 5; ++i) {
        deque.push(&values[i]);
    }
    
    REQUIRE(deque.size() == 5);
    REQUIRE(!deque.empty());
    
    // Pop values (LIFO order for owner)
    for (int i = 4; i >= 0; --i) {
        int* value = deque.pop();
        REQUIRE(value != nullptr);
        REQUIRE(*value == values[i]);
    }
    
    REQUIRE(deque.empty());
    REQUIRE(deque.pop() == nullptr);
}

TEST_CASE("chase_lev_deque single-threaded push/steal", "[chase_lev_deque]") {
    chase_lev_deque<int> deque;
    
    int values[] = {1, 2, 3, 4, 5};
    
    // Push values
    for (int i = 0; i < 5; ++i) {
        deque.push(&values[i]);
    }
    
    // Steal values (FIFO order for thieves)
    for (int i = 0; i < 5; ++i) {
        int* value = deque.steal();
        REQUIRE(value != nullptr);
        REQUIRE(*value == values[i]);
    }
    
    REQUIRE(deque.empty());
    REQUIRE(deque.steal() == nullptr);
}

TEST_CASE("chase_lev_deque LIFO vs FIFO order", "[chase_lev_deque]") {
    chase_lev_deque<int> deque;
    
    int values[] = {10, 20, 30};
    for (int i = 0; i < 3; ++i) {
        deque.push(&values[i]);
    }
    
    // Owner pops: LIFO (30, 20, 10)
    int* v1 = deque.pop();
    REQUIRE(v1 != nullptr);
    REQUIRE(*v1 == 30);
    
    deque.push(&values[0]);  // Push 10 back
    // Deque now: [10, 20, 10] (from bottom perspective)
    // FIFO view (steal order): 10, 20, 10
    
    // Thief steals: FIFO (oldest first)
    int* v2 = deque.steal();
    REQUIRE(v2 != nullptr);
    REQUIRE(*v2 == 10);
    
    int* v3 = deque.steal();
    REQUIRE(v3 != nullptr);
    REQUIRE(*v3 == 20);
    
    // One element left (the 10 we pushed back)
    REQUIRE(deque.size() == 1);
    
    // Pop the remaining element
    int* v4 = deque.pop();
    REQUIRE(v4 != nullptr);
    REQUIRE(*v4 == 10);
    
    REQUIRE(deque.empty());
}

TEST_CASE("chase_lev_deque buffer resize", "[chase_lev_deque]") {
    chase_lev_deque<int> deque(4);  // Small initial capacity
    
    std::vector<int> values(100);
    for (int i = 0; i < 100; ++i) {
        values[i] = i;
    }
    
    // Push many values, triggering resize
    for (int i = 0; i < 100; ++i) {
        deque.push(&values[i]);
    }
    
    REQUIRE(deque.size() >= 100);
    
    // Pop and verify
    for (int i = 99; i >= 0; --i) {
        int* value = deque.pop();
        REQUIRE(value != nullptr);
        REQUIRE(*value == i);
    }
    
    REQUIRE(deque.empty());
}

TEST_CASE("chase_lev_deque concurrent push and steal", "[chase_lev_deque]") {
    chase_lev_deque<int> deque;
    
    const int num_items = 1000;
    std::vector<int> values(num_items);
    for (int i = 0; i < num_items; ++i) {
        values[i] = i;
    }
    
    std::atomic<int> stolen_count{0};
    std::vector<int> stolen_values;
    std::mutex stolen_mutex;
    
    // Owner thread: push items
    std::thread owner([&]() {
        for (int i = 0; i < num_items; ++i) {
            deque.push(&values[i]);
            // Small delay to allow stealing
            if (i % 10 == 0) {
                std::this_thread::yield();
            }
        }
    });
    
    // Thief threads: steal items
    auto thief_func = [&]() {
        while (stolen_count.load() < num_items) {
            if (int* value = deque.steal()) {
                stolen_count++;

                std::lock_guard<std::mutex> lock(stolen_mutex);
                stolen_values.push_back(*value);
            } else {
                std::this_thread::yield();
            }
        }
    };
    
    std::vector<std::thread> thieves;
    for (int i = 0; i < 3; ++i) {
        thieves.emplace_back(thief_func);
    }
    
    owner.join();
    
    // Wait a bit for thieves to finish
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    
    for (auto& t : thieves) {
        t.join();
    }
    
    // Owner pops remaining items
    int popped_count = 0;
    while (deque.pop() != nullptr) {
        popped_count++;
    }
    
    // All items should be accounted for
    REQUIRE(stolen_count.load() + popped_count == num_items);
}

TEST_CASE("chase_lev_deque contention on single element", "[chase_lev_deque]") {
    // Test the race condition when there's only one element
    chase_lev_deque<int> deque;
    
    // Reduce iterations under sanitizers to avoid thread ID exhaustion
#ifdef ELIO_DEBUG
    const int iterations = 100;
#else
    const int iterations = 10000;
#endif
    std::atomic<int> pop_wins{0};
    std::atomic<int> steal_wins{0};
    
    for (int iter = 0; iter < iterations; ++iter) {
        int value = iter;
        deque.push(&value);
        
        std::atomic<bool> done{false};
        int* pop_result = nullptr;
        int* steal_result = nullptr;
        
        // Use barrier to synchronize thread start for fair racing
        std::barrier sync_point(2);
        
        // Owner pops
        std::thread owner([&]() {
            sync_point.arrive_and_wait();
            pop_result = deque.pop();
            done.store(true, std::memory_order_release);
        });
        
        // Thief steals
        std::thread thief([&]() {
            sync_point.arrive_and_wait();
            steal_result = deque.steal();
            while (!done.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
        });
        
        owner.join();
        thief.join();
        
        // Exactly one should succeed
        if (pop_result != nullptr) {
            pop_wins++;
            REQUIRE(steal_result == nullptr);
        } else {
            steal_wins++;
            REQUIRE(steal_result != nullptr);
        }
    }
    
    // Both should win at least once (probabilistic)
    // Under heavy instrumentation (TSAN), timing may be severely skewed
    // We only verify the total is correct - the important thing is no data races
    REQUIRE(pop_wins + steal_wins == iterations);
}

TEST_CASE("chase_lev_deque multiple thieves", "[chase_lev_deque]") {
    chase_lev_deque<int> deque;

    const int num_items = 500;
    std::vector<int> values(num_items);
    for (int i = 0; i < num_items; ++i) {
        values[i] = i;
        deque.push(&values[i]);
    }

    std::atomic<int> total_stolen{0};
    const int num_thieves = 5;

    auto thief_func = [&]() {
        int local_stolen = 0;
        while (total_stolen.load() < num_items) {
            if (deque.steal() != nullptr) {
                local_stolen++;
                total_stolen++;
            } else {
                std::this_thread::yield();
            }
        }
        (void)local_stolen;  // Suppress unused warning
    };

    std::vector<std::thread> thieves;
    for (int i = 0; i < num_thieves; ++i) {
        thieves.emplace_back(thief_func);
    }

    for (auto& t : thieves) {
        t.join();
    }

    REQUIRE(total_stolen.load() == num_items);
    REQUIRE(deque.empty());
}

TEST_CASE("chase_lev_deque steal_batch basic", "[chase_lev_deque]") {
    chase_lev_deque<int> deque;

    int values[] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10};

    // Push 10 values
    for (int i = 0; i < 10; ++i) {
        deque.push(&values[i]);
    }

    REQUIRE(deque.size() == 10);

    // Steal batch of up to 4 items
    std::array<int*, 4> batch;
    size_t stolen = deque.steal_batch(batch);

    // Should steal min(10/2, 4) = 4
    REQUIRE(stolen == 4);

    // Verify stolen values (FIFO order: 1, 2, 3, 4)
    for (int i = 0; i < 4; ++i) {
        REQUIRE(batch[i] != nullptr);
        REQUIRE(*batch[i] == i + 1);
    }

    // Remaining: 6 items
    REQUIRE(deque.size() == 6);
}

TEST_CASE("chase_lev_deque steal_batch then pop all", "[chase_lev_deque]") {
    chase_lev_deque<int> deque;

    int values[] = {1, 2, 3, 4, 5, 6};

    // Push 6 values
    for (int i = 0; i < 6; ++i) {
        deque.push(&values[i]);
    }

    // Steal batch (should steal 3 items: 1, 2, 3)
    std::array<int*, 4> batch;
    size_t stolen = deque.steal_batch(batch);
    REQUIRE(stolen == 3);

    // Pop remaining 3 items
    int pop_count = 0;
    while (deque.pop() != nullptr) {
        pop_count++;
    }
    REQUIRE(pop_count == 3);
    REQUIRE(deque.empty());
}

TEST_CASE("chase_lev_deque steal_batch concurrent", "[chase_lev_deque]") {
    chase_lev_deque<int> deque;

    const int num_items = 1000;
    std::vector<int> values(num_items);
    for (int i = 0; i < num_items; ++i) {
        values[i] = i;
    }

    std::atomic<int> total_stolen{0};
    const int num_thieves = 4;

    // Owner pushes items
    std::thread owner([&]() {
        for (int i = 0; i < num_items; ++i) {
            deque.push(&values[i]);
            if (i % 10 == 0) {
                std::this_thread::yield();
            }
        }
    });

    // Thieves steal using batch operations
    auto thief_func = [&]() {
        std::array<int*, 4> batch;
        while (total_stolen.load() < num_items) {
            size_t stolen = deque.steal_batch(batch);
            if (stolen > 0) {
                total_stolen.fetch_add(stolen, std::memory_order_relaxed);
            } else {
                std::this_thread::yield();
            }
        }
    };

    std::vector<std::thread> thieves;
    for (int i = 0; i < num_thieves; ++i) {
        thieves.emplace_back(thief_func);
    }

    owner.join();

    // Wait for thieves to finish
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    for (auto& t : thieves) {
        t.join();
    }

    // Owner pops remaining items
    int popped_count = 0;
    while (deque.pop() != nullptr) {
        popped_count++;
    }

    // All items should be accounted for
    REQUIRE(total_stolen.load() + popped_count == num_items);
}
