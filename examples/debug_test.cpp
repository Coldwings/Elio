/// Debug Tools Test Example
/// 
/// This example demonstrates the Elio debug tools by creating multiple
/// coroutines that run concurrently and can be inspected using:
/// - elio-pstack (command line)
/// - GDB with elio-gdb.py
/// - LLDB with elio-lldb.py
/// 
/// Usage:
///   ./debug_test                    # Run normally
///   ./debug_test --pause            # Pause for debugger attachment
///   elio-pstack $(pidof debug_test) # Inspect from another terminal

#include <elio/elio.hpp>
#include <iostream>
#include <chrono>
#include <thread>
#include <csignal>
#include <atomic>

using namespace elio;

// Global flag for signal handling
std::atomic<bool> g_running{true};

void signal_handler(int) {
    g_running = false;
}

// Helper macro to set debug location in coroutine
#define ELIO_SET_DEBUG_LOCATION() \
    do { \
        auto& p = co_await detail::get_promise{}; \
        p.set_location(__FILE__, __FUNCTION__, __LINE__); \
    } while(0)

// Helper awaitable to get promise reference
namespace detail {
struct get_promise {
    bool await_ready() const noexcept { return false; }
    
    template<typename Promise>
    bool await_suspend(std::coroutine_handle<Promise> h) noexcept {
        promise_ = &h.promise();
        return false; // Don't actually suspend
    }
    
    coro::promise_base& await_resume() noexcept {
        return *promise_;
    }
    
    coro::promise_base* promise_ = nullptr;
};
}

// Level 3: Leaf coroutine that does some work
coro::task<int> compute_value(int x) {
    // Set debug location
    auto& p = co_await detail::get_promise{};
    p.set_location(__FILE__, "compute_value", __LINE__);
    p.set_state(coro::coroutine_state::running);
    
    // Simulate work
    co_await time::sleep_for(std::chrono::milliseconds(100));
    
    p.set_state(coro::coroutine_state::suspended);
    co_return x * 2;
}

// Level 2: Middle coroutine
coro::task<int> process_data(int id) {
    auto& p = co_await detail::get_promise{};
    p.set_location(__FILE__, "process_data", __LINE__);
    p.set_state(coro::coroutine_state::running);
    
    int result = co_await compute_value(id * 10);
    
    co_return result + id;
}

// Level 1: Outer coroutine (worker)
coro::task<void> worker_task(int worker_id) {
    auto& p = co_await detail::get_promise{};
    p.set_location(__FILE__, "worker_task", __LINE__);
    p.set_state(coro::coroutine_state::running);
    
    for (int i = 0; i < 10 && g_running; ++i) {
        int result = co_await process_data(worker_id * 100 + i);
        (void)result;
        
        // Yield to let other tasks run
        co_await time::yield();
    }
    
    p.set_state(coro::coroutine_state::completed);
}

// Long-running task for debugging
coro::task<void> long_running_task([[maybe_unused]] int id) {
    auto& p = co_await detail::get_promise{};
    p.set_location(__FILE__, "long_running_task", __LINE__);
    p.set_state(coro::coroutine_state::running);
    
    while (g_running) {
        co_await time::sleep_for(std::chrono::milliseconds(500));
        p.set_state(coro::coroutine_state::suspended);
    }
    
    p.set_state(coro::coroutine_state::completed);
}

coro::task<int> async_main(int argc, char* argv[]) {
    // Set up signal handler
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);
    
    bool pause_mode = false;
    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == "--pause") {
            pause_mode = true;
        }
    }
    
    std::cout << "=== Elio Debug Tools Test ===" << std::endl;
    std::cout << "PID: " << getpid() << std::endl;
    std::cout << std::endl;
    
    if (pause_mode) {
        std::cout << "Paused for debugger. Use one of:" << std::endl;
        std::cout << "  elio-pstack " << getpid() << std::endl;
        std::cout << "  gdb -p " << getpid() << " -ex 'source tools/elio-gdb.py' -ex 'elio bt'" << std::endl;
        std::cout << std::endl;
        std::cout << "Press Ctrl+C to exit." << std::endl;
        std::cout << std::endl;
    }
    
    // Spawn some worker tasks
    std::vector<coro::task<void>> workers;
    for (int i = 0; i < 4; ++i) {
        workers.push_back(worker_task(i));
    }
    
    // Spawn long-running tasks for debugging
    std::vector<coro::task<void>> long_tasks;
    for (int i = 0; i < 2; ++i) {
        long_tasks.push_back(long_running_task(i));
    }
    
    // Get scheduler and spawn tasks
    auto* sched = runtime::scheduler::current();
    if (!sched) {
        std::cerr << "Error: No scheduler" << std::endl;
        co_return 1;
    }
    
    for (auto& w : workers) {
        sched->spawn(w.release());
    }
    for (auto& t : long_tasks) {
        sched->spawn(t.release());
    }
    
    std::cout << "Spawned " << workers.size() + long_tasks.size() << " tasks" << std::endl;
    std::cout << std::endl;
    
    if (pause_mode) {
        // Keep running until signal
        while (g_running) {
            co_await time::sleep_for(std::chrono::milliseconds(100));
        }
        std::cout << std::endl << "Shutting down..." << std::endl;
    } else {
        // Wait a bit for tasks to run
        co_await time::sleep_for(std::chrono::seconds(2));
        g_running = false;
        co_await time::sleep_for(std::chrono::milliseconds(200));
    }
    
    std::cout << "=== Test Complete ===" << std::endl;
    
    co_return 0;
}

ELIO_ASYNC_MAIN(async_main)
