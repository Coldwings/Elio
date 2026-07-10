#include <catch2/catch_test_macros.hpp>
#include <elio/coro/task.hpp>
#include <elio/coro/task_handle.hpp>
#include <elio/coro/frame.hpp>
#include <elio/runtime/scheduler.hpp>
#include <elio/runtime/spawn.hpp>
#include <elio/runtime/affinity.hpp>
#include <string>
#include <atomic>
#include <chrono>
#include <latch>
#include <thread>
#include <vector>
#include "../test_main.cpp"  // For scaled timeouts

using namespace elio::coro;
using namespace elio::runtime;
using namespace elio::test;

// Helper: access handle from immovable task (for testing only)
template<typename T>
auto get_handle(task<T>& t) {
    return elio::coro::detail::task_access::handle(t);
}

// Helper: Simple coroutine that returns a value
task<int> simple_return_value() {
    co_return 42;
}

// Helper: Simple void coroutine
task<void> simple_void() {
    co_return;
}

// Helper: Coroutine that throws
task<int> throwing_coroutine() {
    throw std::runtime_error("test error");
    co_return 0;  // Unreachable
}

// Helper: Nested coroutines
task<int> nested_inner() {
    co_return 10;
}

task<int> nested_outer() {
    int value = co_await nested_inner();
    co_return value * 2;
}

TEST_CASE("task construction and destruction", "[task]") {
    {
        auto t = simple_return_value();
        REQUIRE(get_handle(t) != nullptr);
    }
    // Task should destroy handle in destructor
}

TEST_CASE("task is non-movable", "[task]") {
    // Verify task<T> is non-movable and non-copyable
    STATIC_REQUIRE_FALSE(std::is_move_constructible_v<task<int>>);
    STATIC_REQUIRE_FALSE(std::is_move_assignable_v<task<int>>);
    STATIC_REQUIRE_FALSE(std::is_copy_constructible_v<task<int>>);
    STATIC_REQUIRE_FALSE(std::is_copy_assignable_v<task<int>>);
    
    STATIC_REQUIRE_FALSE(std::is_move_constructible_v<task<void>>);
    STATIC_REQUIRE_FALSE(std::is_move_assignable_v<task<void>>);
    STATIC_REQUIRE_FALSE(std::is_copy_constructible_v<task<void>>);
    STATIC_REQUIRE_FALSE(std::is_copy_assignable_v<task<void>>);
}

TEST_CASE("destroyed task_handle waiter is unregistered",
          "[task][task_handle][cancellation]") {
    auto state = std::make_shared<elio::coro::detail::task_state<void>>();
    task_handle<void> handle(state);

    auto waiter_task = [&]() -> task<void> {
        auto result = co_await handle;
        (void)result;
    };

    auto waiter = waiter_task();
    auto h = elio::coro::detail::task_access::release(waiter);
    h.resume();
    REQUIRE_FALSE(h.done());

    h.destroy();
    state->set_value();
}

TEST_CASE("destroyed join_handle waiter is unregistered",
          "[task][join_handle][cancellation]") {
    auto state = std::make_shared<elio::coro::detail::join_state<void>>();

    auto waiter_task = [state]() -> task<void> {
        join_handle<void> handle(state);
        co_await handle;
    };

    auto waiter = waiter_task();
    auto h = elio::coro::detail::task_access::release(waiter);
    h.resume();
    REQUIRE_FALSE(h.done());

    h.destroy();
    state->set_value();
}

TEST_CASE("task<int> co_return value", "[task]") {
    auto t = simple_return_value();
    
    // Start the coroutine
    get_handle(t).resume();
    
    // The promise should have the value
    REQUIRE(get_handle(t).promise().value_.has_value());
    REQUIRE(get_handle(t).promise().value_.value() == 42);
}

TEST_CASE("task<void> co_return void", "[task]") {
    auto t = simple_void();
    
    // Start the coroutine
    get_handle(t).resume();
    
    // Should complete without error
    REQUIRE(get_handle(t).done());
}

TEST_CASE("task stores exception", "[task]") {
    auto t = throwing_coroutine();
    
    // Start the coroutine
    get_handle(t).resume();
    
    // The promise should have an exception
    REQUIRE(get_handle(t).promise().exception() != nullptr);
}

TEST_CASE("task co_await basic", "[task]") {
    auto outer = []() -> task<int> {
        auto t = simple_return_value();
        int result = co_await t;
        REQUIRE(result == 42);
        co_return result + 1;
    };
    
    auto t = outer();
    get_handle(t).resume();
    
    // The outer task should have result 43
    REQUIRE(get_handle(t).promise().value_.value() == 43);
}

TEST_CASE("task nested co_await", "[task]") {
    auto t = nested_outer();
    get_handle(t).resume();
    
    // The outer coroutine should return 20 (10 * 2)
    REQUIRE(get_handle(t).promise().value_.value() == 20);
}

TEST_CASE("task exception propagation via co_await", "[task]") {
    auto outer = []() -> task<void> {
        try {
            auto t = throwing_coroutine();
            co_await t;
            FAIL("Should have thrown");
        } catch (const std::runtime_error& e) {
            REQUIRE(std::string(e.what()) == "test error");
        }
    };
    
    auto t = outer();
    get_handle(t).resume();
    
    // Should complete without unhandled exception
    REQUIRE(get_handle(t).done());
}

TEST_CASE("task virtual stack integration", "[task][.integration]") {
    // This test requires scheduler context, run with elio::run()
    scheduler sched(2);
    sched.start();
    
    std::atomic<bool> passed{false};
    
    auto test_coro = []() -> task<void> {
        auto inner = []() -> task<int> {
            // Inside inner coroutine, virtual stack should be at least 1 deep
            size_t depth = get_stack_depth();
            REQUIRE(depth >= 1);
            co_return 100;
        };
        
        auto outer = [&]() -> task<int> {
            size_t outer_depth = get_stack_depth();
            int result = co_await inner();
            size_t after_depth = get_stack_depth();
            
            // After co_await, we should be back to outer depth
            REQUIRE(after_depth == outer_depth);
            co_return result;
        };
        
        int result = co_await outer();
        REQUIRE(result == 100);
        co_return;
    };
    
    auto driver = [&]() -> task<void> {
        co_await test_coro();
        passed.store(true);
    };
    
    elio::go(driver);
    
    std::this_thread::sleep_for(scaled_ms(200));
    
    sched.shutdown();
    
    REQUIRE(passed.load());
}

TEST_CASE("task multiple levels", "[task]") {
    auto level3 = []() -> task<int> { co_return 1; };
    auto level2 = [&]() -> task<int> {
        int v = co_await level3();
        co_return v + 1;
    };
    auto level1 = [&]() -> task<int> {
        int v = co_await level2();
        co_return v + 1;
    };
    
    auto t = level1();
    get_handle(t).resume();
    
    REQUIRE(get_handle(t).promise().value_.value() == 3);
}

TEST_CASE("task<void> exception propagation", "[task]") {
    auto thrower = []() -> task<void> {
        throw std::runtime_error("void error");
        co_return;
    };
    
    auto catcher = [&]() -> task<void> {
        try {
            co_await thrower();
            FAIL("Should have thrown");
        } catch (const std::runtime_error& e) {
            REQUIRE(std::string(e.what()) == "void error");
        }
    };
    
    auto t = catcher();
    get_handle(t).resume();
    REQUIRE(get_handle(t).done());
}

// ============================================================================
// Tests for new task spawning API: elio::go(), elio::spawn(), join_handle
// ============================================================================

TEST_CASE("elio::go() spawns fire-and-forget task", "[task][spawn]") {
    scheduler sched(2);
    sched.start();
    
    std::atomic<bool> executed{false};
    
    auto coro = [&]() -> task<void> {
        executed.store(true);
        co_return;
    };
    
    // Use elio::go() to spawn fire-and-forget
    elio::go(coro);
    
    // Wait for execution
    std::this_thread::sleep_for(scaled_ms(100));
    
    REQUIRE(executed.load());
    
    sched.shutdown();
}

TEST_CASE("elio::go() spawns fire-and-forget task with value", "[task][spawn]") {
    scheduler sched(2);
    sched.start();
    
    std::atomic<int> result{0};
    
    auto coro = [&]() -> task<int> {
        result.store(42);
        co_return 42;  // Value is discarded in fire-and-forget
    };
    
    elio::go(coro);
    
    std::this_thread::sleep_for(scaled_ms(100));
    
    REQUIRE(result.load() == 42);
    
    sched.shutdown();
}

TEST_CASE("elio::go_to() pins task to specific worker", "[task][spawn][affinity]") {
    scheduler sched(4);
    sched.start();

    const size_t target = 2;
    std::atomic<bool> executed{false};
    std::atomic<size_t> observed_worker{NO_AFFINITY};

    auto coro = [&]() -> task<void> {
        observed_worker.store(elio::current_worker_id());
        executed.store(true);
        co_return;
    };

    elio::go_to(target, coro);

    auto start = std::chrono::steady_clock::now();
    while (!executed.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        if (std::chrono::steady_clock::now() - start > scaled_sec(5)) break;
    }

    REQUIRE(executed.load());
    REQUIRE(observed_worker.load() == target);

    sched.shutdown();
}

TEST_CASE("elio::go_to() with arguments", "[task][spawn][affinity]") {
    scheduler sched(4);
    sched.start();

    const size_t target = 1;
    std::atomic<bool> executed{false};
    std::atomic<size_t> observed_worker{NO_AFFINITY};
    std::atomic<int> sum{0};

    auto coro = [](std::atomic<bool>* done, std::atomic<size_t>* worker,
                   std::atomic<int>* result, int a, int b) -> task<void> {
        worker->store(elio::current_worker_id());
        result->store(a + b);
        done->store(true);
        co_return;
    };

    elio::go_to(target, coro, &executed, &observed_worker, &sum, 10, 32);

    auto start = std::chrono::steady_clock::now();
    while (!executed.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        if (std::chrono::steady_clock::now() - start > scaled_sec(5)) break;
    }

    REQUIRE(executed.load());
    REQUIRE(observed_worker.load() == target);
    REQUIRE(sum.load() == 42);

    sched.shutdown();
}

TEST_CASE("ELIO_GO_TO macro pins task to worker", "[task][spawn][affinity]") {
    scheduler sched(4);
    sched.start();

    const size_t target = 3;
    std::atomic<bool> executed{false};
    std::atomic<size_t> observed_worker{NO_AFFINITY};

    auto work = [&]() -> task<void> {
        observed_worker.store(elio::current_worker_id());
        executed.store(true);
        co_return;
    };

    ELIO_GO_TO(target, work());

    auto start = std::chrono::steady_clock::now();
    while (!executed.load()) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        if (std::chrono::steady_clock::now() - start > scaled_sec(5)) break;
    }

    REQUIRE(executed.load());
    REQUIRE(observed_worker.load() == target);

    sched.shutdown();
}

TEST_CASE("elio::spawn() returns joinable handle", "[task][spawn][join_handle]") {
    scheduler sched(2);
    sched.start();
    
    std::atomic<bool> completed{false};
    
    auto compute = []() -> task<int> {
        co_return 100;
    };
    
    auto driver = [&]() -> task<void> {
        auto handle = elio::spawn(compute);
        int result = co_await handle;
        REQUIRE(result == 100);
        completed.store(true);
    };
    
    elio::go(driver);
    
    std::this_thread::sleep_for(scaled_ms(200));
    
    REQUIRE(completed.load());
    
    sched.shutdown();
}

TEST_CASE("elio::spawn() with void task returns joinable handle", "[task][spawn][join_handle]") {
    scheduler sched(2);
    sched.start();
    
    std::atomic<int> counter{0};
    std::atomic<bool> completed{false};
    
    auto work = [&]() -> task<void> {
        counter.fetch_add(1);
        co_return;
    };
    
    auto driver = [&]() -> task<void> {
        auto handle = elio::spawn(work);
        co_await handle;
        REQUIRE(counter.load() == 1);
        completed.store(true);
    };
    
    elio::go(driver);
    
    std::this_thread::sleep_for(scaled_ms(200));
    
    REQUIRE(completed.load());
    
    sched.shutdown();
}

TEST_CASE("join_handle propagates exceptions", "[task][spawn][join_handle]") {
    scheduler sched(2);
    sched.start();
    
    std::atomic<bool> caught_exception{false};
    
    auto thrower = []() -> task<int> {
        throw std::runtime_error("spawn error");
        co_return 0;
    };
    
    auto catcher = [&]() -> task<void> {
        try {
            auto handle = elio::spawn(thrower);
            co_await handle;
            FAIL("Should have thrown");
        } catch (const std::runtime_error& e) {
            REQUIRE(std::string(e.what()) == "spawn error");
            caught_exception.store(true);
        }
    };
    
    elio::go(catcher);
    
    std::this_thread::sleep_for(scaled_ms(200));
    
    REQUIRE(caught_exception.load());
    
    sched.shutdown();
}

TEST_CASE("multiple elio::spawn() tasks run concurrently", "[task][spawn][join_handle]") {
    scheduler sched(4);
    sched.start();
    
    std::atomic<int> running{0};
    std::atomic<int> max_concurrent{0};
    std::atomic<bool> completed{false};
    
    auto work = [&]() -> task<int> {
        int current = running.fetch_add(1) + 1;
        // Update max_concurrent
        int expected = max_concurrent.load();
        while (current > expected && !max_concurrent.compare_exchange_weak(expected, current)) {}
        
        std::this_thread::sleep_for(scaled_ms(50));
        running.fetch_sub(1);
        co_return current;
    };
    
    auto driver = [&]() -> task<void> {
        auto h1 = elio::spawn(work);
        auto h2 = elio::spawn(work);
        auto h3 = elio::spawn(work);
        
        co_await h1;
        co_await h2;
        co_await h3;
        
        completed.store(true);
    };
    
    elio::go(driver);
    
    std::this_thread::sleep_for(scaled_ms(500));
    
    REQUIRE(completed.load());
    // With 4 threads and 3 tasks, at least 2 should run concurrently
    REQUIRE(max_concurrent.load() >= 2);
    
    sched.shutdown();
}

TEST_CASE("join_handle::is_ready() reflects completion state", "[task][spawn][join_handle]") {
    scheduler sched(2);
    sched.start();
    
    std::atomic<bool> test_passed{false};
    
    auto slow_task = []() -> task<int> {
        std::this_thread::sleep_for(scaled_ms(100));
        co_return 42;
    };
    
    auto driver = [&]() -> task<void> {
        auto handle = elio::spawn(slow_task);
        
        // Initially not ready
        bool was_not_ready = !handle.is_ready();
        
        // Wait for completion
        int result = co_await handle;
        
        // After await, should be ready
        bool is_now_ready = handle.is_ready();
        
        test_passed.store(was_not_ready && is_now_ready && result == 42);
    };
    
    elio::go(driver);
    
    std::this_thread::sleep_for(scaled_ms(300));
    
    REQUIRE(test_passed.load());
    
    sched.shutdown();
}

TEST_CASE("elio::go() works with scheduler context", "[scheduler][spawn]") {
    scheduler sched(2);
    sched.start();
    
    std::atomic<bool> executed{false};
    
    auto coro = [&]() -> task<void> {
        executed.store(true);
        co_return;
    };
    
    // Use elio::go() to spawn fire-and-forget task
    elio::go(coro);
    
    std::this_thread::sleep_for(scaled_ms(100));
    
    REQUIRE(executed.load());
    
    sched.shutdown();
}

TEST_CASE("elio::go() works with task<int>", "[scheduler][spawn]") {
    scheduler sched(2);
    sched.start();

    std::atomic<int> result{0};

    auto coro = [&]() -> task<int> {
        result.store(99);
        co_return 99;
    };

    elio::go(coro);

    std::this_thread::sleep_for(scaled_ms(100));

    REQUIRE(result.load() == 99);

    sched.shutdown();
}

// --- go() exception handler tests ---

TEST_CASE("go() task throws — handler is called", "[task][spawn][exception]") {
    std::atomic<bool> handler_called{false};
    std::string handler_message;

    scheduler sched(2);
    sched.start();
    sched.set_unhandled_exception_handler([&](std::exception_ptr ex) {
        handler_called.store(true, std::memory_order_release);
        try {
            std::rethrow_exception(ex);
        } catch (const std::exception& e) {
            handler_message = e.what();
        }
    });

    elio::go([]() -> task<void> {
        throw std::runtime_error("go task exception");
        co_return;
    });

    // Wait for the detached task to complete and handler to fire
    std::this_thread::sleep_for(scaled_ms(100));
    sched.shutdown();

    REQUIRE(handler_called.load(std::memory_order_acquire));
    REQUIRE(handler_message == "go task exception");
}

TEST_CASE("scheduler exception handler can be replaced during reports", "[task][spawn][exception]") {
    scheduler sched(1);

    constexpr int reporter_count = 4;
    std::latch reporters_ready(reporter_count);
    std::latch start_reporting(1);
    std::atomic<bool> done{false};
    std::atomic<int> handled{0};

    sched.set_unhandled_exception_handler([&handled](std::exception_ptr) {
        handled.fetch_add(1, std::memory_order_relaxed);
    });

    auto reporter = [&] {
        reporters_ready.count_down();
        start_reporting.wait();
        do {
            sched.report_unhandled_exception(
                std::make_exception_ptr(std::runtime_error("reported")));
        } while (!done.load(std::memory_order_acquire));
    };

    std::vector<std::thread> reporters;
    for (int i = 0; i < reporter_count; ++i) {
        reporters.emplace_back(reporter);
    }

    std::thread updater([&] {
        reporters_ready.wait();
        start_reporting.count_down();
        for (int i = 0; i < 20000; ++i) {
            sched.set_unhandled_exception_handler(
                [&handled](std::exception_ptr) {
                    handled.fetch_add(1, std::memory_order_relaxed);
                });
        }
        done.store(true, std::memory_order_release);
    });

    updater.join();
    for (auto& t : reporters) {
        t.join();
    }

    REQUIRE(handled.load(std::memory_order_relaxed) > 0);
}

TEST_CASE("go() task throws — default log when no handler set", "[task][spawn][exception]") {
    scheduler sched(2);
    sched.start();
    // No handler set — should log ERROR, not crash

    elio::go([]() -> task<void> {
        throw std::runtime_error("go unhandled exception");
        co_return;
    });

    std::this_thread::sleep_for(scaled_ms(100));
    sched.shutdown();
    // Test passes if no crash — default behavior is log ERROR
}

TEST_CASE("go() task completes normally — no handler called", "[task][spawn][exception]") {
    std::atomic<bool> handler_called{false};

    scheduler sched(2);
    sched.start();
    sched.set_unhandled_exception_handler([&](std::exception_ptr) {
        handler_called.store(true, std::memory_order_release);
    });

    std::atomic<bool> task_ran{false};
    elio::go([&]() -> task<void> {
        task_ran.store(true, std::memory_order_release);
        co_return;
    });

    std::this_thread::sleep_for(scaled_ms(100));
    sched.shutdown();

    REQUIRE(task_ran.load(std::memory_order_acquire));
    REQUIRE_FALSE(handler_called.load(std::memory_order_acquire));
}
