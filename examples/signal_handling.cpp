/// @file signal_handling.cpp
/// @brief Signal Handling Example
///
/// This example demonstrates how to use Elio's signalfd-based signal handling
/// for graceful shutdown in coroutine-based services. Unlike traditional signal
/// handlers that can interrupt at any time (making it unsafe to modify coroutine
/// state), signalfd converts signals into readable events that can be handled
/// safely in normal coroutine context.
///
/// Usage: ./signal_handling
/// Press Ctrl+C (SIGINT) or send SIGTERM to trigger graceful shutdown.
/// Send SIGUSR1 to print status information.

#include <elio/elio.hpp>
#include <atomic>
#include <iostream>

using namespace elio;
using namespace elio::coro;
using namespace elio::runtime;
using namespace elio::signal;
using namespace std::chrono_literals;

// Global flag for shutdown coordination
std::atomic<bool> g_running{true};
std::atomic<size_t> g_request_count{0};

/// Signal handler coroutine
/// Waits for signals and handles them appropriately
task<void> signal_handler_task(scheduler& sched) {
    // Create signal set for signals we want to handle
    signal_set sigs;
    sigs.add(SIGINT)    // Ctrl+C
       .add(SIGTERM)   // kill command
       .add(SIGUSR1);  // Status request
    
    // Create signalfd - this blocks the signals and creates a file descriptor
    signal_fd sigfd(sigs);
    
    ELIO_LOG_INFO("Signal handler started, waiting for signals...");
    ELIO_LOG_INFO("  - Press Ctrl+C or send SIGTERM for graceful shutdown");
    ELIO_LOG_INFO("  - Send SIGUSR1 (kill -USR1 {}) for status", getpid());
    
    while (g_running) {
        // Wait for a signal asynchronously
        auto info = co_await sigfd.wait();
        
        if (!info) {
            ELIO_LOG_ERROR("Signal read failed");
            continue;
        }
        
        ELIO_LOG_INFO("Received signal: {} ({})", 
                     info->full_name(), info->signo);
        
        switch (info->signo) {
            case SIGINT:
            case SIGTERM:
                ELIO_LOG_INFO("Shutdown requested, stopping gracefully...");
                g_running = false;
                break;
                
            case SIGUSR1:
                ELIO_LOG_INFO("Status: {} requests processed, {} threads active",
                             g_request_count.load(),
                             sched.num_threads());
                break;
                
            default:
                ELIO_LOG_INFO("Unhandled signal: {}", info->signo);
                break;
        }
    }
    
    ELIO_LOG_INFO("Signal handler exiting");
    co_return;
}

/// Simulated worker coroutine
/// Processes "requests" until shutdown is signaled
task<void> worker_task(int worker_id) {
    ELIO_LOG_INFO("Worker {} started", worker_id);
    
    while (g_running) {
        // Simulate some work
        co_await time::sleep_for(100ms);
        
        if (g_running) {
            g_request_count++;
            
            // Log every 10 requests
            size_t count = g_request_count.load();
            if (count % 10 == 0) {
                ELIO_LOG_DEBUG("Worker {}: processed request #{}", worker_id, count);
            }
        }
    }
    
    ELIO_LOG_INFO("Worker {} stopped", worker_id);
    co_return;
}

/// Main coroutine that coordinates everything
task<void> main_task(scheduler& sched) {
    ELIO_LOG_INFO("Starting application with PID {}", getpid());
    
    // Spawn the signal handler
    auto sig_handler = signal_handler_task(sched);
    sched.spawn(sig_handler.release());
    
    // Spawn some worker coroutines
    constexpr int num_workers = 3;
    for (int i = 0; i < num_workers; ++i) {
        auto worker = worker_task(i);
        sched.spawn(worker.release());
    }
    
    ELIO_LOG_INFO("All workers started");
    
    // Main loop - just yield while running
    while (g_running) {
        co_await time::sleep_for(500ms);
    }
    
    ELIO_LOG_INFO("Main task completing, processed {} total requests",
                 g_request_count.load());
    co_return;
}

int main() {
    // IMPORTANT: Block signals BEFORE creating any threads
    // This ensures all threads inherit the blocked signal mask
    signal_set sigs{SIGINT, SIGTERM, SIGUSR1};
    sigs.block_all_threads();
    
    // Set up logging
    log::logger::instance().set_level(log::level::info);
    
    // Create scheduler with worker threads
    scheduler sched(4);
    
    // Set up I/O context for async operations
    io::io_context ctx;
    sched.set_io_context(&ctx);
    
    sched.start();
    
    // Spawn main task
    auto main = main_task(sched);
    sched.spawn(main.release());
    
    // Run until shutdown
    while (g_running) {
        ctx.poll(std::chrono::milliseconds(10));
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    
    // Give coroutines time to clean up
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    
    // Poll any remaining I/O
    for (int i = 0; i < 10 && ctx.has_pending(); ++i) {
        ctx.poll(std::chrono::milliseconds(10));
    }
    
    sched.shutdown();
    
    ELIO_LOG_INFO("Application shutdown complete");
    return 0;
}
