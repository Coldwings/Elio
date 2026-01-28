/// @file tcp_echo_server.cpp
/// @brief TCP Echo Server Example
/// 
/// This example demonstrates how to build a concurrent TCP echo server
/// using Elio's async I/O and coroutine support. The server handles
/// multiple client connections concurrently.
///
/// Supports both IPv4 and IPv6:
///   - Default: Dual-stack (IPv6 socket accepting both IPv4 and IPv6)
///   - Use -4 for IPv4-only mode
///   - Use -6 for IPv6-only mode
///   - Use -b <addr> to bind to a specific address
///
/// Usage: ./tcp_echo_server [options] [port]
///   -4           IPv4 only (bind to 0.0.0.0)
///   -6           IPv6 only (bind to :: with IPV6_V6ONLY)
///   -b <addr>    Bind to specific address (e.g., 127.0.0.1, ::1, 192.168.1.1)
///   -h, --help   Show help
/// Default port: 8080

#include <elio/elio.hpp>
#include <atomic>
#include <iostream>

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
task<void> server_main(const socket_address& bind_addr, const tcp_options& opts, scheduler& sched) {
    // Bind TCP listener
    auto listener_result = tcp_listener::bind(bind_addr, opts);
    if (!listener_result) {
        ELIO_LOG_ERROR("Failed to bind to {}: {}", bind_addr.to_string(),
                      strerror(errno));
        co_return;
    }
    
    auto& listener = *listener_result;
    
    // Store listener fd for shutdown cancellation
    g_listener_fd.store(listener.fd(), std::memory_order_release);
    
    ELIO_LOG_INFO("Echo server listening on {}", bind_addr.to_string());
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
    // Parse command line options
    uint16_t port = 8080;
    std::string bind_address;
    bool ipv4_only = false;
    bool ipv6_only = false;
    
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "-4") {
            ipv4_only = true;
        } else if (arg == "-6") {
            ipv6_only = true;
        } else if (arg == "-b" && i + 1 < argc) {
            bind_address = argv[++i];
        } else if (arg == "-h" || arg == "--help") {
            std::cout << "Usage: " << argv[0] << " [options] [port]\n"
                      << "Options:\n"
                      << "  -4           IPv4 only (bind to 0.0.0.0)\n"
                      << "  -6           IPv6 only (bind to :: with IPV6_V6ONLY)\n"
                      << "  -b <addr>    Bind to specific address\n"
                      << "  -h, --help   Show this help\n"
                      << "Default: Dual-stack on port 8080 (accepts IPv4 and IPv6)\n";
            return 0;
        } else if (arg[0] != '-') {
            port = static_cast<uint16_t>(std::stoi(arg));
        }
    }
    
    // Determine bind address
    socket_address bind_addr;
    tcp_options opts;
    
    if (!bind_address.empty()) {
        // User specified address
        bind_addr = socket_address(bind_address, port);
    } else if (ipv4_only) {
        // IPv4 only
        bind_addr = socket_address(ipv4_address(port));
    } else {
        // Default: IPv6 dual-stack (accepts both IPv4 and IPv6)
        bind_addr = socket_address(ipv6_address(port));
        opts.ipv6_only = ipv6_only;  // If -6 specified, disable dual-stack
    }
    
    ELIO_LOG_INFO("Binding to {} ({})", bind_addr.to_string(),
                  ipv4_only ? "IPv4 only" : (ipv6_only ? "IPv6 only" : "dual-stack"));
    
    // Block signals BEFORE creating scheduler threads
    signal_set sigs{SIGINT, SIGTERM};
    sigs.block_all_threads();
    
    // Create scheduler with worker threads
    scheduler sched(4);
    
    sched.start();
    
    // Spawn signal handler coroutine
    auto sig_handler = signal_handler_task();
    sched.spawn(sig_handler.release());
    
    // Run server
    auto server = server_main(bind_addr, opts, sched);
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
