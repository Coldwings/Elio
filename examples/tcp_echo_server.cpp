/// @file tcp_echo_server.cpp
/// @brief TCP Echo Server Example
/// 
/// This example demonstrates how to build a concurrent TCP echo server
/// using Elio's async I/O and coroutine support. The server handles
/// multiple client connections concurrently.
///
/// Usage: ./tcp_echo_server [port]
/// Default port: 8080

#include <elio/elio.hpp>
#include <atomic>

using namespace elio;
using namespace elio::coro;
using namespace elio::runtime;
using namespace elio::net;
using namespace elio::signal;

// Global flag for graceful shutdown
std::atomic<bool> g_running{true};

// Global listener fd for cancellation on shutdown
std::atomic<int> g_listener_fd{-1};

/// Signal handler coroutine - waits for SIGINT/SIGTERM
task<void> signal_handler_task() {
    signal_set sigs{SIGINT, SIGTERM};
    signal_fd sigfd(sigs);
    
    ELIO_LOG_DEBUG("Signal handler started, waiting for SIGINT/SIGTERM...");
    
    auto info = co_await sigfd.wait();
    if (info) {
        ELIO_LOG_INFO("Received signal: {} - initiating shutdown", info->full_name());
    }
    
    g_running = false;
    
    // Close the listener to interrupt the pending accept
    int fd = g_listener_fd.exchange(-1);
    if (fd >= 0) {
        ::close(fd);
    }
    
    co_return;
}

/// Handle a single client connection
/// Reads data and echoes it back until the client disconnects
task<void> handle_client(tcp_stream stream, int client_id) {
    auto peer = stream.peer_address();
    ELIO_LOG_INFO("[Client {}] Connected from {}", client_id,
                  peer ? peer->to_string() : "unknown");
    
    char buffer[1024];
    size_t total_bytes = 0;
    
    while (g_running) {
        // Read from client
        auto result = co_await stream.read(buffer, sizeof(buffer));
        
        if (result.result <= 0) {
            if (result.result == 0) {
                ELIO_LOG_INFO("[Client {}] Disconnected (EOF)", client_id);
            } else {
                ELIO_LOG_ERROR("[Client {}] Read error: {} ({})", client_id,
                              strerror(-result.result), result.result);
            }
            break;
        }
        
        total_bytes += result.result;
        
        // Echo back to client
        auto written = co_await stream.write(buffer, result.result);
        if (written.result <= 0) {
            ELIO_LOG_ERROR("[Client {}] Write error: {} ({})", client_id,
                          strerror(-written.result), written.result);
            break;
        }
    }
    
    ELIO_LOG_INFO("[Client {}] Total bytes echoed: {}", client_id, total_bytes);
    co_return;
}

/// Main server loop - accepts connections and spawns handlers
task<void> server_main(uint16_t port, scheduler& sched) {
    // Use the default io_context which is polled by scheduler workers
    auto& ctx = io::default_io_context();
    
    // Bind TCP listener
    auto listener_result = tcp_listener::bind(ipv4_address(port), ctx);
    if (!listener_result) {
        ELIO_LOG_ERROR("Failed to bind to port {}: {}", port,
                      strerror(errno));
        co_return;
    }
    
    auto& listener = *listener_result;
    
    // Store listener fd for shutdown cancellation
    g_listener_fd.store(listener.fd(), std::memory_order_release);
    
    ELIO_LOG_INFO("Echo server listening on port {}", port);
    ELIO_LOG_INFO("Press Ctrl+C to stop");
    
    int client_counter = 0;
    
    while (g_running) {
        // Accept new connection
        auto stream_result = co_await listener.accept();
        
        if (!stream_result) {
            if (g_running) {
                ELIO_LOG_ERROR("Accept error: {}", strerror(errno));
            }
            // Exit loop if listener was closed for shutdown
            if (!g_running || errno == EBADF) {
                break;
            }
            continue;
        }
        
        // Spawn handler coroutine for this client
        int client_id = ++client_counter;
        auto handler = handle_client(std::move(*stream_result), client_id);
        sched.spawn(handler.release());
    }
    
    ELIO_LOG_INFO("Server shutting down...");
    co_return;
}

int main(int argc, char* argv[]) {
    // Parse port from command line
    uint16_t port = 8080;
    if (argc > 1) {
        port = static_cast<uint16_t>(std::stoi(argv[1]));
    }
    
    // Block signals BEFORE creating scheduler threads
    signal_set sigs{SIGINT, SIGTERM};
    sigs.block_all_threads();
    
    // Create scheduler with worker threads
    scheduler sched(4);
    
    // Set the I/O context so workers can poll for I/O completions
    sched.set_io_context(&io::default_io_context());
    
    sched.start();
    
    // Spawn signal handler coroutine
    auto sig_handler = signal_handler_task();
    sched.spawn(sig_handler.release());
    
    // Run server
    auto server = server_main(port, sched);
    sched.spawn(server.release());
    
    // Wait until interrupted
    while (g_running) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    
    // Give the server coroutine time to process the cancelled accept
    // and exit gracefully before shutting down the scheduler
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    
    // Brief drain of any remaining I/O completions (with timeout)
    auto& ctx = io::default_io_context();
    for (int i = 0; i < 10 && ctx.has_pending(); ++i) {
        ctx.poll(std::chrono::milliseconds(10));
    }
    
    sched.shutdown();
    
    ELIO_LOG_INFO("Server stopped");
    return 0;
}
