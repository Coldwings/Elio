#include <catch2/catch_test_macros.hpp>
#include <elio/coro/cancel_token.hpp>
#include <elio/coro/task.hpp>
#include <elio/runtime/scheduler.hpp>
#include <elio/time/timer.hpp>

#include <thread>
#include <vector>
#include <atomic>
#include <latch>

using namespace elio::coro;
using namespace elio::runtime;
using namespace elio::time;

// Helper to spawn a joinable task
template<typename F>
auto spawn_joinable(scheduler& sched, F&& f) {
    return sched.go_joinable(std::forward<F>(f));
}



// ==================== Basic Operations ====================

TEST_CASE("cancel_token default constructed is not cancelled", "[cancel_token][basic]") {
    cancel_token token;
    REQUIRE_FALSE(token.is_cancelled());
    REQUIRE(token);  // operator bool returns true when not cancelled
}

TEST_CASE("cancel_token is_cancelled returns correct state", "[cancel_token][basic]") {
    cancel_source source;
    cancel_token token = source.get_token();
    
    REQUIRE_FALSE(token.is_cancelled());
    REQUIRE(token);
    
    source.cancel();
    
    REQUIRE(token.is_cancelled());
    REQUIRE_FALSE(token);
}

TEST_CASE("cancel_token operator bool returns true when not cancelled", "[cancel_token][basic]") {
    cancel_source source;
    cancel_token token = source.get_token();
    
    // operator bool should return true when NOT cancelled
    REQUIRE(static_cast<bool>(token));
    
    source.cancel();
    
    // operator bool should return false when cancelled
    REQUIRE_FALSE(static_cast<bool>(token));
}

// ==================== Cancel Source ====================

TEST_CASE("cancel_source creates tokens that share state", "[cancel_token][source]") {
    cancel_source source;
    cancel_token token1 = source.get_token();
    cancel_token token2 = source.get_token();
    cancel_token token3 = source.get_token();
    
    // All tokens start not cancelled
    REQUIRE_FALSE(token1.is_cancelled());
    REQUIRE_FALSE(token2.is_cancelled());
    REQUIRE_FALSE(token3.is_cancelled());
    
    // Cancel the source
    source.cancel();
    
    // All tokens should be cancelled
    REQUIRE(token1.is_cancelled());
    REQUIRE(token2.is_cancelled());
    REQUIRE(token3.is_cancelled());
}

TEST_CASE("cancel_source cancel triggers all tokens", "[cancel_token][source]") {
    cancel_source source;
    std::vector<cancel_token> tokens;
    
    // Create multiple tokens
    for (int i = 0; i < 10; ++i) {
        tokens.push_back(source.get_token());
    }
    
    // Cancel once
    source.cancel();
    
    // All tokens should be cancelled
    for (const auto& token : tokens) {
        REQUIRE(token.is_cancelled());
    }
}

TEST_CASE("cancel_source is_cancelled reflects state", "[cancel_token][source]") {
    cancel_source source;
    
    REQUIRE_FALSE(source.is_cancelled());
    
    source.cancel();
    
    REQUIRE(source.is_cancelled());
}

TEST_CASE("cancel_source multiple cancel calls are safe", "[cancel_token][source]") {
    cancel_source source;
    cancel_token token = source.get_token();
    
    source.cancel();
    REQUIRE(token.is_cancelled());
    
    // Second cancel should be safe (no-op)
    source.cancel();
    REQUIRE(token.is_cancelled());
    
    // Third cancel should also be safe
    source.cancel();
    REQUIRE(token.is_cancelled());
}

// ==================== Callbacks ====================

TEST_CASE("cancel_token on_cancel registers callback", "[cancel_token][callback]") {
    cancel_source source;
    cancel_token token = source.get_token();
    std::atomic<bool> callback_invoked{false};
    
    {
        auto reg = token.on_cancel([&callback_invoked]() {
            callback_invoked.store(true, std::memory_order_release);
        });
        
        REQUIRE_FALSE(callback_invoked.load(std::memory_order_acquire));
        
        source.cancel();
        
        REQUIRE(callback_invoked.load(std::memory_order_acquire));
    }
}

TEST_CASE("cancel_token on_cancel invoked immediately if already cancelled", "[cancel_token][callback]") {
    cancel_source source;
    cancel_token token = source.get_token();
    
    // Cancel first
    source.cancel();
    REQUIRE(token.is_cancelled());
    
    // Register callback after cancellation
    std::atomic<bool> callback_invoked{false};
    auto reg = token.on_cancel([&callback_invoked]() {
        callback_invoked.store(true, std::memory_order_release);
    });
    
    // Callback should be invoked immediately
    REQUIRE(callback_invoked.load(std::memory_order_acquire));
}

TEST_CASE("cancel_token multiple callbacks all invoked", "[cancel_token][callback]") {
    cancel_source source;
    cancel_token token = source.get_token();
    std::atomic<int> callback_count{0};
    
    {
        auto reg1 = token.on_cancel([&callback_count]() {
            callback_count.fetch_add(1, std::memory_order_relaxed);
        });
        auto reg2 = token.on_cancel([&callback_count]() {
            callback_count.fetch_add(1, std::memory_order_relaxed);
        });
        auto reg3 = token.on_cancel([&callback_count]() {
            callback_count.fetch_add(1, std::memory_order_relaxed);
        });
        
        source.cancel();
        
        REQUIRE(callback_count.load(std::memory_order_relaxed) == 3);
    }
}

TEST_CASE("cancel_registration destroyed unregisters callback", "[cancel_token][callback]") {
    cancel_source source;
    cancel_token token = source.get_token();
    std::atomic<bool> callback_invoked{false};
    
    {
        auto reg = token.on_cancel([&callback_invoked]() {
            callback_invoked.store(true, std::memory_order_release);
        });
        
        // Registration goes out of scope
    }
    
    // Cancel after registration is destroyed
    source.cancel();
    
    // Callback should NOT be invoked
    REQUIRE_FALSE(callback_invoked.load(std::memory_order_acquire));
}

TEST_CASE("cancel_registration manual unregister prevents invocation", "[cancel_token][callback]") {
    cancel_source source;
    cancel_token token = source.get_token();
    std::atomic<bool> callback_invoked{false};
    
    auto reg = token.on_cancel([&callback_invoked]() {
        callback_invoked.store(true, std::memory_order_release);
    });
    
    // Manually unregister
    reg.unregister();
    
    source.cancel();
    
    // Callback should NOT be invoked
    REQUIRE_FALSE(callback_invoked.load(std::memory_order_acquire));
}

TEST_CASE("cancel_registration move constructor", "[cancel_token][callback]") {
    cancel_source source;
    cancel_token token = source.get_token();
    std::atomic<bool> callback_invoked{false};
    
    {
        auto reg1 = token.on_cancel([&callback_invoked]() {
            callback_invoked.store(true, std::memory_order_release);
        });
        
        // Move to new registration
        auto reg2 = std::move(reg1);
        
        source.cancel();
        
        REQUIRE(callback_invoked.load(std::memory_order_acquire));
    }
}

TEST_CASE("cancel_registration move assignment", "[cancel_token][callback]") {
    cancel_source source;
    cancel_token token = source.get_token();
    std::atomic<bool> callback1_invoked{false};
    std::atomic<bool> callback2_invoked{false};
    
    {
        auto reg1 = token.on_cancel([&callback1_invoked]() {
            callback1_invoked.store(true, std::memory_order_release);
        });
        
        auto reg2 = token.on_cancel([&callback2_invoked]() {
            callback2_invoked.store(true, std::memory_order_release);
        });
        
        // Move reg2 to reg1 (reg1's callback should be unregistered)
        reg1 = std::move(reg2);
        
        source.cancel();
        
        // Only the second callback should be invoked
        REQUIRE_FALSE(callback1_invoked.load(std::memory_order_acquire));
        REQUIRE(callback2_invoked.load(std::memory_order_acquire));
    }
}

TEST_CASE("cancel_registration move assignment transfers callback ownership", "[cancel_token][callback]") {
    cancel_source source;
    cancel_token token = source.get_token();
    std::atomic<bool> callback_invoked{false};
    
    auto reg = token.on_cancel([&callback_invoked]() {
        callback_invoked.store(true, std::memory_order_release);
    });
    
    // Move assignment: transfer callback ownership from reg to reg2.
    // reg2 now holds the registration; reg is in a moved-from state.
    auto reg2 = std::move(reg);
    
    source.cancel();
    
    // Callback should still be invoked via reg2
    REQUIRE(callback_invoked.load(std::memory_order_acquire));
}

// ==================== Coroutine Integration ====================

TEST_CASE("cancel_token on_cancel callback invoked on cancellation thread", "[cancel_token][coro]") {
    // NOTE: on_cancel_resume() calls handle.resume() directly from the cancel
    // callback, which runs on the cancelling thread (main), not necessarily
    // the scheduler thread where the coroutine frame lives.  That causes TSAN
    // data races on the coroutine's shared state (e.g. cancel_registration
    // shared_ptr ref-counts).
    //
    // The correct pattern is: use on_cancel() to set an atomic flag and have
    // the coroutine poll it on the scheduler thread.
    cancel_source source;
    cancel_token token = source.get_token();
    std::atomic<bool> callback_called{false};
    std::atomic<bool> completed{false};
    
    scheduler sched(1);
    sched.start();
    
    auto make_task = [&]() -> task<void> {
        auto reg = token.on_cancel([&callback_called]() {
            callback_called.store(true, std::memory_order_release);
        });
        
        // Poll until cancelled - the on_cancel callback runs on the cancel
        // thread (main), which safely sets the atomic flag consumed here.
        while (!callback_called.load(std::memory_order_acquire) && !token.is_cancelled()) {
            co_await yield();
        }
        
        completed.store(true, std::memory_order_release);
        co_return;
    };
    
    auto join_handle = spawn_joinable(sched, make_task);
    
    // Allow coroutine to reach suspend point
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    
    source.cancel();
    
    join_handle.wait_destroyed();
    sched.shutdown();
    
    REQUIRE(callback_called.load(std::memory_order_acquire));
    REQUIRE(completed.load(std::memory_order_acquire));
}

TEST_CASE("cancel_token cancelled coroutine can check and exit", "[cancel_token][coro]") {
    cancel_source source;
    cancel_token token = source.get_token();
    std::atomic<int> iterations{0};
    std::atomic<bool> completed{false};
    
    scheduler sched(1);
    sched.start();
    
    auto make_task = [&]() -> task<void> {
        while (token) {  // operator bool - continue while not cancelled
            iterations.fetch_add(1, std::memory_order_relaxed);
            co_await yield();
            
            // Prevent infinite loop in case of test failure
            if (iterations.load(std::memory_order_relaxed) > 1000) {
                break;
            }
        }
        completed.store(true, std::memory_order_release);
        co_return;
    };
    
    auto join_handle = spawn_joinable(sched, make_task);
    
    // Let it run a bit
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    
    REQUIRE(iterations.load(std::memory_order_relaxed) > 0);
    
    // Cancel the task
    source.cancel();
    
    join_handle.wait_destroyed();
    sched.shutdown();
    
    REQUIRE(completed.load(std::memory_order_acquire));
    REQUIRE(token.is_cancelled());
}

TEST_CASE("cancel_token co_await with cancellation check", "[cancel_token][coro]") {
    cancel_source source;
    cancel_token token = source.get_token();
    std::atomic<bool> checked_cancelled{false};
    std::atomic<bool> completed{false};
    
    scheduler sched(1);
    sched.start();
    
    auto make_task = [&]() -> task<void> {
        // Simulate work with periodic cancellation checks
        // Use sleep_for to allow the main thread time to cancel
        for (int i = 0; i < 100; ++i) {
            if (token.is_cancelled()) {
                checked_cancelled.store(true, std::memory_order_release);
                break;
            }
            co_await sleep_for(std::chrono::milliseconds(5));
        }
        completed.store(true, std::memory_order_release);
        co_return;
    };
    
    auto join_handle = spawn_joinable(sched, make_task);
    
    // Small delay then cancel
    std::this_thread::sleep_for(std::chrono::milliseconds(30));
    
    // Cancel
    source.cancel();
    
    join_handle.wait_destroyed();
    sched.shutdown();
    
    REQUIRE(checked_cancelled.load(std::memory_order_acquire));
    REQUIRE(completed.load(std::memory_order_acquire));
}

// ==================== Edge Cases ====================

TEST_CASE("cancel_token default constructed empty token behavior", "[cancel_token][edge]") {
    // Default constructed token has no state - should never be cancelled
    cancel_token token;
    
    REQUIRE_FALSE(token.is_cancelled());
    REQUIRE(token);
    
    // Callback registration on empty token should work but never be called
    std::atomic<bool> callback_invoked{false};
    auto reg = token.on_cancel([&callback_invoked]() {
        callback_invoked.store(true, std::memory_order_release);
    });
    
    // Since there's no source, callback won't be invoked
    REQUIRE_FALSE(callback_invoked.load(std::memory_order_acquire));
    
    // Token remains not cancelled
    REQUIRE_FALSE(token.is_cancelled());
}

TEST_CASE("cancel_token copied from cancelled source", "[cancel_token][edge]") {
    cancel_source source;
    source.cancel();
    
    // Get token after cancellation
    cancel_token token = source.get_token();
    
    // Should immediately be cancelled
    REQUIRE(token.is_cancelled());
    REQUIRE_FALSE(token);
    
    // Callback should be invoked immediately
    std::atomic<bool> callback_invoked{false};
    auto reg = token.on_cancel([&callback_invoked]() {
        callback_invoked.store(true, std::memory_order_release);
    });
    
    REQUIRE(callback_invoked.load(std::memory_order_acquire));
}

TEST_CASE("cancel_token token copy shares state", "[cancel_token][edge]") {
    cancel_source source;
    cancel_token token1 = source.get_token();
    cancel_token token2 = token1;  // Copy
    
    REQUIRE_FALSE(token1.is_cancelled());
    REQUIRE_FALSE(token2.is_cancelled());
    
    source.cancel();
    
    // Both copies should see cancellation
    REQUIRE(token1.is_cancelled());
    REQUIRE(token2.is_cancelled());
}

TEST_CASE("cancel_token multiple sources independent cancellation", "[cancel_token][edge]") {
    cancel_source source1;
    cancel_source source2;
    
    cancel_token token1 = source1.get_token();
    cancel_token token2 = source2.get_token();
    
    // Cancel only source1
    source1.cancel();
    
    REQUIRE(token1.is_cancelled());
    REQUIRE_FALSE(token2.is_cancelled());
    
    // Cancel source2
    source2.cancel();
    
    REQUIRE(token1.is_cancelled());
    REQUIRE(token2.is_cancelled());
}

TEST_CASE("cancel_source empty source cancel is safe", "[cancel_token][edge]") {
    // Default constructed source with no state
    // This tests internal safety - cancel_source always has state though
    cancel_source source;
    
    // Should be safe to cancel
    source.cancel();
    REQUIRE(source.is_cancelled());
}

// ==================== Thread Safety ====================

TEST_CASE("cancel_token concurrent cancellation and callback registration", "[cancel_token][thread]") {
    cancel_source source;
    std::atomic<int> callbacks_invoked{0};
    std::atomic<int> registrations_completed{0};
    std::atomic<bool> cancel_done{false};
    
    constexpr int NUM_THREADS = 4;
    constexpr int CALLBACKS_PER_THREAD = 100;
    
    std::vector<std::thread> threads;
    
    // Threads registering callbacks
    for (int t = 0; t < NUM_THREADS; ++t) {
        threads.emplace_back([&]() {
            for (int i = 0; i < CALLBACKS_PER_THREAD; ++i) {
                auto token = source.get_token();
                auto reg = token.on_cancel([&callbacks_invoked]() {
                    callbacks_invoked.fetch_add(1, std::memory_order_relaxed);
                });
                registrations_completed.fetch_add(1, std::memory_order_relaxed);
                
                // Hold registration briefly
                std::this_thread::yield();
            }
        });
    }
    
    // Thread that cancels
    std::thread cancel_thread([&]() {
        // Let some registrations happen first
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        source.cancel();
        cancel_done.store(true, std::memory_order_release);
    });
    
    for (auto& t : threads) {
        t.join();
    }
    cancel_thread.join();
    
    REQUIRE(cancel_done.load(std::memory_order_acquire));
    REQUIRE(source.is_cancelled());
    
    // Callbacks may or may not have been invoked depending on timing
    // But the system should remain stable
    REQUIRE(registrations_completed.load() == NUM_THREADS * CALLBACKS_PER_THREAD);
}

TEST_CASE("cancel_token concurrent cancellation from multiple threads", "[cancel_token][thread]") {
    cancel_source source;
    std::atomic<int> cancel_count{0};
    
    constexpr int NUM_CANCEL_THREADS = 4;
    
    std::vector<std::thread> threads;
    
    for (int t = 0; t < NUM_CANCEL_THREADS; ++t) {
        threads.emplace_back([&]() {
            source.cancel();
            cancel_count.fetch_add(1, std::memory_order_relaxed);
        });
    }
    
    for (auto& t : threads) {
        t.join();
    }
    
    REQUIRE(cancel_count.load() == NUM_CANCEL_THREADS);
    REQUIRE(source.is_cancelled());
}

TEST_CASE("cancel_token concurrent token access from multiple threads", "[cancel_token][thread]") {
    cancel_source source;
    cancel_token token = source.get_token();
    std::atomic<int> cancelled_count{0};
    std::atomic<int> not_cancelled_count{0};
    
    constexpr int NUM_THREADS = 4;
    constexpr int ITERATIONS = 1000;
    
    std::latch start_latch(NUM_THREADS + 1);
    std::vector<std::thread> threads;
    
    for (int t = 0; t < NUM_THREADS; ++t) {
        threads.emplace_back([&]() {
            start_latch.arrive_and_wait();
            for (int i = 0; i < ITERATIONS; ++i) {
                if (token.is_cancelled()) {
                    cancelled_count.fetch_add(1, std::memory_order_relaxed);
                } else {
                    not_cancelled_count.fetch_add(1, std::memory_order_relaxed);
                }
            }
        });
    }
    
    // Start all threads simultaneously
    start_latch.arrive_and_wait();
    
    // Cancel midway
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    source.cancel();
    
    for (auto& t : threads) {
        t.join();
    }
    
    REQUIRE(cancelled_count.load() + not_cancelled_count.load() == NUM_THREADS * ITERATIONS);
    REQUIRE(source.is_cancelled());
}

TEST_CASE("cancel_token callback invoked exactly once", "[cancel_token][thread]") {
    cancel_source source;
    cancel_token token = source.get_token();
    std::atomic<int> callback_count{0};
    
    {
        auto reg = token.on_cancel([&callback_count]() {
            callback_count.fetch_add(1, std::memory_order_relaxed);
        });
        
        // Cancel multiple times
        source.cancel();
        source.cancel();
        source.cancel();
        
        // Callback should be invoked exactly once
        REQUIRE(callback_count.load(std::memory_order_relaxed) == 1);
    }
}

TEST_CASE("cancel_token stress test with many callbacks", "[cancel_token][thread]") {
    cancel_source source;
    cancel_token token = source.get_token();
    std::atomic<int> callbacks_invoked{0};
    
    constexpr int NUM_CALLBACKS = 1000;
    
    {
        std::vector<cancel_registration> regs;
        regs.reserve(NUM_CALLBACKS);
        
        for (int i = 0; i < NUM_CALLBACKS; ++i) {
            regs.push_back(token.on_cancel([&callbacks_invoked]() {
                callbacks_invoked.fetch_add(1, std::memory_order_relaxed);
            }));
        }
        
        source.cancel();
        
        REQUIRE(callbacks_invoked.load(std::memory_order_relaxed) == NUM_CALLBACKS);
    }
}
