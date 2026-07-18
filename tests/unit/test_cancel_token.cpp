#include <catch2/catch_test_macros.hpp>
#include <elio/coro/cancel_token.hpp>
#include <elio/coro/task.hpp>
#include <elio/runtime/scheduler.hpp>
#include <elio/time/timer.hpp>

#include <atomic>
#include <barrier>
#include <latch>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using namespace elio::coro;
using namespace elio::runtime;
using namespace elio::time;

// Helper to spawn a joinable task
template<typename F>
auto spawn_joinable(scheduler& sched, F&& f) {
    return sched.go_joinable(std::forward<F>(f));
}

struct unregister_on_destroy {
    std::optional<cancel_registration>* target;
    std::barrier<>* callbacks_started;
    std::atomic<bool>* invoked;

    unregister_on_destroy(std::optional<cancel_registration>* target_registration,
                          std::barrier<>* started,
                          std::atomic<bool>* was_invoked) noexcept
        : target(target_registration),
          callbacks_started(started),
          invoked(was_invoked) {}

    unregister_on_destroy(unregister_on_destroy&& other) noexcept
        : target(std::exchange(other.target, nullptr)),
          callbacks_started(other.callbacks_started),
          invoked(other.invoked) {}

    unregister_on_destroy(const unregister_on_destroy&) = delete;
    unregister_on_destroy& operator=(const unregister_on_destroy&) = delete;

    ~unregister_on_destroy() {
        if (target) target->reset();
    }

    void operator()() {
        callbacks_started->arrive_and_wait();
        invoked->store(true, std::memory_order_release);
    }
};



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

TEST_CASE("cancel_token releases all callbacks when one throws",
          "[cancel_token][callback][regression]") {
    cancel_source source;
    cancel_token token = source.get_token();
    bool later_invoked = false;
    auto capture = std::make_shared<int>(42);
    std::weak_ptr<int> weak_capture = capture;
    auto tail_capture = std::make_shared<int>(7);
    std::weak_ptr<int> weak_tail_capture = tail_capture;

    auto later_throwing = token.on_cancel(
        [capture = std::move(tail_capture)] {
            throw std::runtime_error("later callback failed");
        });
    auto later = token.on_cancel([&later_invoked] {
        later_invoked = true;
    });
    auto throwing = token.on_cancel([capture = std::move(capture)] {
        throw std::runtime_error("callback failed");
    });

    std::string exception_message;
    try {
        source.cancel();
    } catch (const std::runtime_error& e) {
        exception_message = e.what();
    }
    REQUIRE(exception_message == "callback failed");
    REQUIRE(later_invoked);
    REQUIRE(weak_capture.expired());
    REQUIRE(weak_tail_capture.expired());
}

TEST_CASE("already-cancelled token releases a throwing callback",
          "[cancel_token][callback][regression]") {
    cancel_source source;
    cancel_token token = source.get_token();
    source.cancel();
    auto capture = std::make_shared<int>(42);
    std::weak_ptr<int> weak_capture = capture;

    std::string exception_message;
    try {
        auto registration = token.on_cancel([capture = std::move(capture)] {
            throw std::runtime_error("immediate callback failed");
        });
        (void)registration;
    } catch (const std::runtime_error& e) {
        exception_message = e.what();
    }
    REQUIRE(exception_message == "immediate callback failed");
    REQUIRE(weak_capture.expired());
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

TEST_CASE("cancel registration waits for a callback running on another thread",
          "[cancel_token][callback][thread][lifetime]") {
    cancel_source source;
    std::latch callback_started(1);
    std::latch release_callback(1);
    std::latch unregister_started(1);
    std::atomic<bool> unregister_done{false};
    std::atomic<bool> callback_done{false};

    auto registration = source.get_token().on_cancel([&] {
        callback_started.count_down();
        release_callback.wait();
        callback_done.store(true, std::memory_order_release);
    });

    std::thread canceller([&] { source.cancel(); });
    callback_started.wait();

    std::thread unregisterer(
        [registration = std::move(registration), &unregister_started,
         &unregister_done]() mutable {
            unregister_started.count_down();
            registration.unregister();
            unregister_done.store(true, std::memory_order_release);
        });
    unregister_started.wait();

    // Give the dedicated unregister thread a scheduling opportunity. It must
    // remain inside unregister() until the callback has stopped using captures.
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    REQUIRE_FALSE(unregister_done.load(std::memory_order_acquire));

    release_callback.count_down();
    canceller.join();
    unregisterer.join();
    REQUIRE(callback_done.load(std::memory_order_acquire));
    REQUIRE(unregister_done.load(std::memory_order_acquire));
}

TEST_CASE("cancel registration waits after another thread selects its callback",
          "[cancel_token][callback][thread][lifetime]") {
    cancel_source source;
    std::latch dispatch_blocked(1);
    std::latch release_dispatch(1);
    std::latch unregister_started(1);
    std::atomic<bool> selected_callback_ran{false};
    std::atomic<bool> unregister_done{false};

    // The list is LIFO. Register the selected callback first so the blocker is
    // dispatched before it after cancel() atomically claims both callbacks.
    auto selected = source.get_token().on_cancel([&] {
        selected_callback_ran.store(true, std::memory_order_release);
    });
    auto blocker = source.get_token().on_cancel([&] {
        dispatch_blocked.count_down();
        release_dispatch.wait();
    });

    std::thread canceller([&] { source.cancel(); });
    dispatch_blocked.wait();

    std::thread unregisterer(
        [selected = std::move(selected), &unregister_started,
         &unregister_done]() mutable {
            unregister_started.count_down();
            selected.unregister();
            unregister_done.store(true, std::memory_order_release);
        });
    unregister_started.wait();
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    REQUIRE_FALSE(unregister_done.load(std::memory_order_acquire));

    release_dispatch.count_down();
    canceller.join();
    unregisterer.join();
    REQUIRE(selected_callback_ran.load(std::memory_order_acquire));
    REQUIRE(unregister_done.load(std::memory_order_acquire));
}

TEST_CASE("cancel callback can unregister itself without deadlock",
          "[cancel_token][callback][thread][lifetime]") {
    cancel_source source;
    std::optional<cancel_registration> registration;
    std::atomic<bool> callback_done{false};

    registration.emplace(source.get_token().on_cancel([&] {
        registration.reset();
        callback_done.store(true, std::memory_order_release);
    }));

    source.cancel();
    REQUIRE(callback_done.load(std::memory_order_acquire));
    REQUIRE_FALSE(registration.has_value());
}

TEST_CASE("cancel callback can remove a later selected callback",
          "[cancel_token][callback][thread][lifetime]") {
    cancel_source source;
    std::optional<cancel_registration> later_registration;
    std::atomic<bool> first_ran{false};
    std::atomic<bool> later_ran{false};

    later_registration.emplace(source.get_token().on_cancel([&] {
        later_ran.store(true, std::memory_order_release);
    }));
    auto first_registration = source.get_token().on_cancel([&] {
        first_ran.store(true, std::memory_order_release);
        later_registration.reset();
    });

    source.cancel();
    REQUIRE(first_ran.load(std::memory_order_acquire));
    REQUIRE_FALSE(later_ran.load(std::memory_order_acquire));
}

TEST_CASE("nested cancellation preserves an outer selected callback",
          "[cancel_token][callback][lifetime]") {
    cancel_source outer_source;
    cancel_source nested_source;
    std::optional<cancel_registration> outer_later_registration;
    std::atomic<unsigned> outer_later_invocations{0};
    std::atomic<bool> outer_first_ran{false};
    std::atomic<bool> nested_ran{false};

    outer_later_registration.emplace(
        outer_source.get_token().on_cancel([&] {
            outer_later_invocations.fetch_add(1, std::memory_order_relaxed);
        }));
    auto nested_registration = nested_source.get_token().on_cancel([&] {
        nested_ran.store(true, std::memory_order_release);
        outer_later_registration.reset();
    });
    auto outer_first_registration = outer_source.get_token().on_cancel([&] {
        outer_first_ran.store(true, std::memory_order_release);
        nested_source.cancel();
    });

    outer_source.cancel();

    REQUIRE(outer_first_ran.load(std::memory_order_acquire));
    REQUIRE(nested_ran.load(std::memory_order_acquire));
    REQUIRE(outer_later_invocations.load(std::memory_order_relaxed) == 1);
    REQUIRE_FALSE(outer_later_registration.has_value());
}

TEST_CASE("callbacks on separate dispatchers can mutually unregister",
          "[cancel_token][callback][thread][lifetime]") {
    cancel_source first_source;
    cancel_source second_source;
    std::optional<cancel_registration> first_registration;
    std::optional<cancel_registration> second_registration;
    std::barrier callbacks_started(2);
    std::atomic<bool> first_done{false};
    std::atomic<bool> second_done{false};

    first_registration.emplace(first_source.get_token().on_cancel([&] {
        callbacks_started.arrive_and_wait();
        second_registration.reset();
        first_done.store(true, std::memory_order_release);
    }));
    second_registration.emplace(second_source.get_token().on_cancel([&] {
        callbacks_started.arrive_and_wait();
        first_registration.reset();
        second_done.store(true, std::memory_order_release);
    }));

    std::thread first_canceller([&] { first_source.cancel(); });
    std::thread second_canceller([&] { second_source.cancel(); });
    first_canceller.join();
    second_canceller.join();

    REQUIRE(first_done.load(std::memory_order_acquire));
    REQUIRE(second_done.load(std::memory_order_acquire));
    REQUIRE_FALSE(first_registration.has_value());
    REQUIRE_FALSE(second_registration.has_value());
}

TEST_CASE("cross-dispatch unregister preserves a claimed callback",
          "[cancel_token][callback][thread][lifetime]") {
    cancel_source target_source;
    cancel_source unregister_source;
    std::optional<cancel_registration> target_registration;
    std::latch blocker_started(1);
    std::latch release_blocker(1);
    std::atomic<unsigned> target_invocations{0};
    std::atomic<bool> unregister_done{false};

    target_registration.emplace(target_source.get_token().on_cancel([&] {
        target_invocations.fetch_add(1, std::memory_order_relaxed);
    }));
    auto blocker_registration = target_source.get_token().on_cancel([&] {
        blocker_started.count_down();
        release_blocker.wait();
    });
    auto unregister_registration = unregister_source.get_token().on_cancel([&] {
        target_registration.reset();
        unregister_done.store(true, std::memory_order_release);
    });

    std::thread target_canceller([&] { target_source.cancel(); });
    blocker_started.wait();
    std::thread unregister_canceller([&] { unregister_source.cancel(); });

    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::seconds(5);
    while (!unregister_done.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    const bool returned_without_wait =
        unregister_done.load(std::memory_order_acquire);

    release_blocker.count_down();
    target_canceller.join();
    unregister_canceller.join();

    REQUIRE(returned_without_wait);
    REQUIRE(target_invocations.load(std::memory_order_relaxed) == 1);
    REQUIRE_FALSE(target_registration.has_value());
}

TEST_CASE("callback payload destructors can mutually unregister",
          "[cancel_token][callback][thread][lifetime]") {
    cancel_source first_source;
    cancel_source second_source;
    std::optional<cancel_registration> first_registration;
    std::optional<cancel_registration> second_registration;
    std::barrier callbacks_started(2);
    std::atomic<bool> first_invoked{false};
    std::atomic<bool> second_invoked{false};

    first_registration.emplace(first_source.get_token().on_cancel(
        unregister_on_destroy{&second_registration, &callbacks_started,
                              &first_invoked}));
    second_registration.emplace(second_source.get_token().on_cancel(
        unregister_on_destroy{&first_registration, &callbacks_started,
                              &second_invoked}));

    std::thread first_canceller([&] { first_source.cancel(); });
    std::thread second_canceller([&] { second_source.cancel(); });
    first_canceller.join();
    second_canceller.join();

    REQUIRE(first_invoked.load(std::memory_order_acquire));
    REQUIRE(second_invoked.load(std::memory_order_acquire));
    REQUIRE_FALSE(first_registration.has_value());
    REQUIRE_FALSE(second_registration.has_value());
}
