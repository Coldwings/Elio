#include <catch2/catch_test_macros.hpp>
#include <elio/elio.hpp>
#include <elio/runtime/spawn.hpp>
#include <elio/coro/vthread_stack.hpp>
#include <atomic>
#include <thread>
#include <vector>
#include "../test_main.cpp"  // For scaled timeouts

using namespace elio::coro;
using namespace elio::runtime;
using namespace elio::test;

// ============================================================================
// vthread_stack basic allocation/deallocation (LIFO correctness)
// ============================================================================

TEST_CASE("vthread_stack basic allocation and deallocation", "[vthread_stack]") {
    vthread_stack stack;
    
    // Allocate several blocks
    void* p1 = stack.push(64);
    void* p2 = stack.push(128);
    void* p3 = stack.push(256);
    
    REQUIRE(p1 != nullptr);
    REQUIRE(p2 != nullptr);
    REQUIRE(p3 != nullptr);
    
    // All pointers should be different
    REQUIRE(p1 != p2);
    REQUIRE(p2 != p3);
    REQUIRE(p1 != p3);
    
    // Pop in reverse order (LIFO)
    stack.pop(p3, 256);
    stack.pop(p2, 128);
    stack.pop(p1, 64);
}

TEST_CASE("vthread_stack LIFO order verification", "[vthread_stack]") {
    vthread_stack stack;
    
    // Push multiple allocations and store their addresses
    std::vector<std::pair<void*, size_t>> allocs;
    for (size_t i = 1; i <= 10; ++i) {
        size_t size = i * 16;
        void* p = stack.push(size);
        allocs.push_back({p, size});
    }
    
    // Pop in reverse order - should not fail assertions
    for (auto it = allocs.rbegin(); it != allocs.rend(); ++it) {
        stack.pop(it->first, it->second);
    }
}

// ============================================================================
// Segment growth test
// ============================================================================

TEST_CASE("vthread_stack segment growth", "[vthread_stack]") {
    vthread_stack stack;
    
    // Default segment size is 16KB, allocate more to trigger new segment
    constexpr size_t large_size = 8192;  // 8KB each
    
    void* p1 = stack.push(large_size);
    void* p2 = stack.push(large_size);
    void* p3 = stack.push(large_size);  // This should trigger a new segment
    
    REQUIRE(p1 != nullptr);
    REQUIRE(p2 != nullptr);
    REQUIRE(p3 != nullptr);
    
    // All should be valid allocations
    // Pop in reverse order
    stack.pop(p3, large_size);
    stack.pop(p2, large_size);
    stack.pop(p1, large_size);
}

TEST_CASE("vthread_stack oversized allocation", "[vthread_stack]") {
    vthread_stack stack;
    
    // Allocate larger than default segment size
    constexpr size_t huge_size = 32768;  // 32KB > 16KB default
    
    void* p = stack.push(huge_size);
    REQUIRE(p != nullptr);
    
    stack.pop(p, huge_size);
}

// ============================================================================
// vthread_stack static API (thread-local current)
// ============================================================================

TEST_CASE("vthread_stack thread-local current", "[vthread_stack]") {
    // Initially no current stack
    REQUIRE(vthread_stack::current() == nullptr);
    
    vthread_stack stack;
    vthread_stack::set_current(&stack);
    REQUIRE(vthread_stack::current() == &stack);
    
    vthread_stack::set_current(nullptr);
    REQUIRE(vthread_stack::current() == nullptr);
}

TEST_CASE("vthread_stack thread-local isolation", "[vthread_stack]") {
    vthread_stack main_stack;
    vthread_stack::set_current(&main_stack);
    
    std::atomic<bool> worker_isolated{false};
    
    std::thread worker([&]() {
        // Worker thread should have no current stack
        worker_isolated = (vthread_stack::current() == nullptr);
        
        vthread_stack worker_stack;
        vthread_stack::set_current(&worker_stack);
        
        // Worker's current should be different from main's
        worker_isolated = worker_isolated && (vthread_stack::current() != &main_stack);
        
        vthread_stack::set_current(nullptr);
    });
    
    worker.join();
    
    REQUIRE(worker_isolated.load());
    REQUIRE(vthread_stack::current() == &main_stack);
    
    vthread_stack::set_current(nullptr);
}

// ============================================================================
// task<T> on-site co_await evaluation
// ============================================================================

namespace {
task<int> compute(int x) {
    co_return x * 2;
}

task<void> test_basic_await_impl() {
    int val = co_await compute(21);
    REQUIRE(val == 42);
}
}

TEST_CASE("task co_await basic", "[vthread_stack]") {
    elio::run(test_basic_await_impl);
}

// ============================================================================
// task<void> symmetric test
// ============================================================================

namespace {
task<void> void_work() {
    co_return;
}

task<void> test_void_await_impl() {
    co_await void_work();
    // If we reach here, void await worked
}
}

TEST_CASE("task<void> co_await", "[vthread_stack]") {
    elio::run(test_void_await_impl);
}

// ============================================================================
// Nested call chain: LIFO correctness for multi-layer co_await
// ============================================================================

namespace {
task<int> level3() { co_return 1; }
task<int> level2() { co_return co_await level3() + 1; }
task<int> level1() { co_return co_await level2() + 1; }

task<void> test_nested_impl() {
    int result = co_await level1();
    REQUIRE(result == 3);  // 1 + 1 + 1
}
}

TEST_CASE("nested co_await chain LIFO", "[vthread_stack]") {
    elio::run(test_nested_impl);
}

// ============================================================================
// elio::go(func) — no-argument function
// ============================================================================

TEST_CASE("elio::go() with no-arg function", "[vthread_stack][spawn]") {
    scheduler sched(2);
    sched.start();
    
    std::atomic<bool> done{false};
    
    auto work = [&done]() -> task<void> {
        done.store(true);
        co_return;
    };
    
    elio::go(work);
    
    // Wait for completion
    std::this_thread::sleep_for(scaled_ms(100));
    
    REQUIRE(done.load());
    
    sched.shutdown();
}

// ============================================================================
// elio::go(func, args...) — with arguments
// ============================================================================

namespace {
task<void> work_with_args(std::atomic<int>* counter, int increment) {
    counter->fetch_add(increment);
    co_return;
}
}

TEST_CASE("elio::go() with arguments", "[vthread_stack][spawn]") {
    scheduler sched(2);
    sched.start();
    
    std::atomic<int> counter{0};
    
    elio::go(work_with_args, &counter, 10);
    
    std::this_thread::sleep_for(scaled_ms(100));
    
    REQUIRE(counter.load() == 10);
    
    sched.shutdown();
}

// ============================================================================
// elio::spawn(func) — returns join_handle
// ============================================================================

TEST_CASE("elio::spawn() returns join_handle", "[vthread_stack][spawn]") {
    scheduler sched(2);
    sched.start();
    
    std::atomic<bool> completed{false};
    
    auto driver = [&]() -> task<void> {
        auto h = elio::spawn(compute, 21);
        int result = co_await h;
        REQUIRE(result == 42);
        completed.store(true);
    };
    
    elio::go(driver);
    
    std::this_thread::sleep_for(scaled_ms(200));
    
    REQUIRE(completed.load());
    
    sched.shutdown();
}

// ============================================================================
// elio::spawn(func, args...) — with arguments
// ============================================================================

namespace {
task<int> add_values(int a, int b) {
    co_return a + b;
}
}

TEST_CASE("elio::spawn() with arguments", "[vthread_stack][spawn]") {
    scheduler sched(2);
    sched.start();
    
    std::atomic<bool> completed{false};
    
    auto driver = [&]() -> task<void> {
        auto h = elio::spawn(add_values, 10, 20);
        int result = co_await h;
        REQUIRE(result == 30);
        completed.store(true);
    };
    
    elio::go(driver);
    
    std::this_thread::sleep_for(scaled_ms(200));
    
    REQUIRE(completed.load());
    
    sched.shutdown();
}

// ============================================================================
// ELIO_GO(expr) / ELIO_SPAWN(expr) macro forms
// ============================================================================

TEST_CASE("ELIO_GO macro", "[vthread_stack][spawn]") {
    scheduler sched(2);
    sched.start();
    
    std::atomic<bool> done{false};
    
    auto work = [&done]() -> task<void> {
        done.store(true);
        co_return;
    };
    
    ELIO_GO(work());
    
    std::this_thread::sleep_for(scaled_ms(100));
    
    REQUIRE(done.load());
    
    sched.shutdown();
}

TEST_CASE("ELIO_SPAWN macro", "[vthread_stack][spawn]") {
    scheduler sched(2);
    sched.start();
    
    std::atomic<bool> completed{false};
    
    auto driver = [&]() -> task<void> {
        auto h = ELIO_SPAWN(compute(21));
        int result = co_await h;
        REQUIRE(result == 42);
        completed.store(true);
    };
    
    elio::go(driver);
    
    std::this_thread::sleep_for(scaled_ms(200));
    
    REQUIRE(completed.load());
    
    sched.shutdown();
}

// ============================================================================
// elio::run(func) — entry execution
// ============================================================================

TEST_CASE("elio::run() executes task to completion", "[vthread_stack]") {
    auto main_task = []() -> task<int> {
        co_return 42;
    };
    
    int result = elio::run(main_task);
    REQUIRE(result == 42);
}

TEST_CASE("elio::run() with void task", "[vthread_stack]") {
    std::atomic<bool> executed{false};
    
    auto main_task = [&executed]() -> task<void> {
        executed.store(true);
        co_return;
    };
    
    elio::run(main_task);
    REQUIRE(executed.load());
}

// ============================================================================
// Mixed scenario: elio::go + internal co_await chain
// ============================================================================

TEST_CASE("elio::go with internal co_await chain", "[vthread_stack][spawn]") {
    scheduler sched(2);
    sched.start();
    
    std::atomic<int> final_result{0};
    
    auto complex_task = [&]() -> task<void> {
        // Multi-level co_await chain inside a go'd task
        int r1 = co_await level1();  // Returns 3
        int r2 = co_await compute(r1);  // 3 * 2 = 6
        int r3 = co_await add_values(r2, 4);  // 6 + 4 = 10
        final_result.store(r3);
    };
    
    elio::go(complex_task);
    
    std::this_thread::sleep_for(scaled_ms(200));
    
    REQUIRE(final_result.load() == 10);
    
    sched.shutdown();
}

// ============================================================================
// Independent vstack isolation verification
// ============================================================================

TEST_CASE("independent vstack isolation between spawned coroutines", "[vthread_stack][spawn]") {
    scheduler sched(4);
    sched.start();
    
    // Each spawned coroutine should have its own vstack
    std::atomic<int> unique_vstacks{0};
    std::atomic<int> completed{0};
    std::vector<vthread_stack*> observed_vstacks;
    std::mutex vstacks_mutex;
    
    auto worker = [&]([[maybe_unused]] int id) -> task<void> {
        auto* my_vstack = vthread_stack::current();
        
        {
            std::lock_guard<std::mutex> lock(vstacks_mutex);
            // Check if this vstack was seen before
            bool is_unique = true;
            for (auto* vs : observed_vstacks) {
                if (vs == my_vstack) {
                    is_unique = false;
                    break;
                }
            }
            if (is_unique && my_vstack != nullptr) {
                observed_vstacks.push_back(my_vstack);
                unique_vstacks.fetch_add(1);
            }
        }
        
        completed.fetch_add(1);
        co_return;
    };
    
    // Spawn multiple tasks
    constexpr int num_tasks = 10;
    for (int i = 0; i < num_tasks; ++i) {
        elio::go([&worker, i]() { return worker(i); });
    }
    
    // Wait for all to complete
    while (completed.load() < num_tasks) {
        std::this_thread::sleep_for(scaled_ms(10));
    }
    
    // Each task should have had a unique vstack
    REQUIRE(unique_vstacks.load() == num_tasks);
    
    sched.shutdown();
}

// ============================================================================
// Exception propagation
// ============================================================================

namespace {
task<int> throwing_task() {
    throw std::runtime_error("test exception");
    co_return 0;
}
}

TEST_CASE("exception propagation through co_await", "[vthread_stack]") {
    auto test_task = []() -> task<void> {
        bool caught = false;
        try {
            co_await throwing_task();
        } catch (const std::runtime_error& e) {
            REQUIRE(std::string(e.what()) == "test exception");
            caught = true;
        }
        REQUIRE(caught);
    };
    
    elio::run(test_task);
}

TEST_CASE("exception propagation through spawn", "[vthread_stack][spawn]") {
    scheduler sched(2);
    sched.start();
    
    std::atomic<bool> caught_exception{false};
    
    auto catcher = [&]() -> task<void> {
        try {
            auto h = elio::spawn(throwing_task);
            co_await h;
        } catch (const std::runtime_error& e) {
            REQUIRE(std::string(e.what()) == "test exception");
            caught_exception.store(true);
        }
    };
    
    elio::go(catcher);
    
    std::this_thread::sleep_for(scaled_ms(200));
    
    REQUIRE(caught_exception.load());
    
    sched.shutdown();
}

// ============================================================================
// task immovability compile-time verification
// ============================================================================

TEST_CASE("task is non-movable and non-copyable", "[vthread_stack]") {
    STATIC_REQUIRE_FALSE(std::is_move_constructible_v<task<int>>);
    STATIC_REQUIRE_FALSE(std::is_move_assignable_v<task<int>>);
    STATIC_REQUIRE_FALSE(std::is_copy_constructible_v<task<int>>);
    STATIC_REQUIRE_FALSE(std::is_copy_assignable_v<task<int>>);
    
    STATIC_REQUIRE_FALSE(std::is_move_constructible_v<task<void>>);
    STATIC_REQUIRE_FALSE(std::is_move_assignable_v<task<void>>);
    STATIC_REQUIRE_FALSE(std::is_copy_constructible_v<task<void>>);
    STATIC_REQUIRE_FALSE(std::is_copy_assignable_v<task<void>>);
}

// ============================================================================
// Deep nested co_await test
// ============================================================================

namespace {
task<int> deep_recursion(int depth) {
    if (depth <= 0) {
        co_return 1;
    }
    co_return co_await deep_recursion(depth - 1) + 1;
}
}

TEST_CASE("deep nested co_await chain", "[vthread_stack]") {
    auto test = []() -> task<void> {
        int result = co_await deep_recursion(10);
        REQUIRE(result == 11);  // 10 levels + 1 base
    };
    
    elio::run(test);
}
