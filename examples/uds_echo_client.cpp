/// @file uds_echo_client.cpp
/// @brief Unix Domain Socket Echo Client Example
/// 
/// This example demonstrates how to create a Unix Domain Socket client
/// using Elio's async I/O. The client connects to an echo server, sends
/// messages, and receives the echoed responses.
///
/// Usage: ./uds_echo_client [socket_path]
/// Default: /tmp/elio_echo.sock
/// Use "@name" for abstract sockets (Linux-specific)

#include <elio/elio.hpp>
#include <iostream>
#include <string>
#include <vector>

using namespace elio;
using namespace elio::coro;
using namespace elio::runtime;
using namespace elio::net;

/// Client coroutine - connects, sends messages, receives responses
task<int> client_main(const unix_address& addr) {
    // Use the default io_context which is polled by scheduler workers
    auto& ctx = io::default_io_context();
    
    ELIO_LOG_INFO("Connecting to {}...", addr.to_string());
    
    // Connect to server
    auto stream_result = co_await uds_connect(ctx, addr);
    
    if (!stream_result) {
        ELIO_LOG_ERROR("Connection failed: {}", strerror(errno));
        co_return 1;
    }
    
    auto& stream = *stream_result;
    ELIO_LOG_INFO("Connected!");
    std::cout << "Type messages to send (empty line to quit):" << std::endl;
    
    // Interactive message loop
    std::string line;
    char recv_buffer[1024];
    
    while (std::getline(std::cin, line)) {
        if (line.empty()) {
            break;
        }
        
        // Add newline for readability
        line += '\n';
        
        // Send message
        auto sent = co_await stream.write(line);
        if (sent.result <= 0) {
            ELIO_LOG_ERROR("Send error: {} ({})", strerror(-sent.result), sent.result);
            break;
        }
        
        // Receive echo
        auto received = co_await stream.read(recv_buffer, sizeof(recv_buffer) - 1);
        if (received.result <= 0) {
            ELIO_LOG_ERROR("Receive error: {} ({})", strerror(-received.result), received.result);
            break;
        }
        
        recv_buffer[received.result] = '\0';
        std::cout << "Echo: " << recv_buffer;
    }
    
    ELIO_LOG_INFO("Disconnecting...");
    co_return 0;
}

/// Non-interactive benchmark mode
task<int> benchmark_main(const unix_address& addr, int iterations) {
    // Use the default io_context which is polled by scheduler workers
    auto& ctx = io::default_io_context();
    
    ELIO_LOG_INFO("Connecting to {} for benchmark...", addr.to_string());
    
    auto stream_result = co_await uds_connect(ctx, addr);
    if (!stream_result) {
        ELIO_LOG_ERROR("Connection failed: {}", strerror(errno));
        co_return 1;
    }
    
    auto& stream = *stream_result;
    ELIO_LOG_INFO("Connected! Running {} round-trips...", iterations);
    
    const char* test_message = "Hello, Elio!\n";
    size_t msg_len = strlen(test_message);
    char recv_buffer[64];
    
    auto start = std::chrono::steady_clock::now();
    
    for (int i = 0; i < iterations; ++i) {
        auto sent = co_await stream.write(test_message, msg_len);
        if (sent.result <= 0) {
            ELIO_LOG_ERROR("Send error at iteration {}: {} ({})", i,
                          strerror(-sent.result), sent.result);
            co_return 1;
        }
        
        size_t total_received = 0;
        while (total_received < msg_len) {
            auto received = co_await stream.read(recv_buffer, sizeof(recv_buffer));
            if (received.result <= 0) {
                ELIO_LOG_ERROR("Receive error at iteration {}: {} ({})", i,
                              strerror(-received.result), received.result);
                co_return 1;
            }
            total_received += received.result;
        }
    }
    
    auto end = std::chrono::steady_clock::now();
    auto duration_ms = std::chrono::duration_cast<std::chrono::milliseconds>(end - start).count();
    
    double rps = (iterations * 1000.0) / duration_ms;
    double avg_latency = duration_ms / static_cast<double>(iterations);
    
    ELIO_LOG_INFO("Benchmark results:");
    ELIO_LOG_INFO("  Total time: {} ms", duration_ms);
    ELIO_LOG_INFO("  Round-trips: {}", iterations);
    ELIO_LOG_INFO("  Avg latency: {:.4f} ms", avg_latency);
    ELIO_LOG_INFO("  Throughput: {} req/s", static_cast<int>(rps));
    
    co_return 0;
}

int main(int argc, char* argv[]) {
    std::string socket_path = "/tmp/elio_echo.sock";
    bool benchmark = false;
    int iterations = 10000;
    
    // Collect positional arguments
    std::vector<std::string> positional;
    
    // Parse command line
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "-b" || arg == "--benchmark") {
            benchmark = true;
        } else if (arg == "-n" && i + 1 < argc) {
            iterations = std::stoi(argv[++i]);
        } else if (arg == "-h" || arg == "--help") {
            std::cout << "Usage: " << argv[0] << " [options] [socket_path]" << std::endl;
            std::cout << "Options:" << std::endl;
            std::cout << "  -b, --benchmark    Run benchmark mode" << std::endl;
            std::cout << "  -n <count>         Number of iterations (default: 10000)" << std::endl;
            std::cout << "  -h, --help         Show this help" << std::endl;
            std::cout << "Default socket: /tmp/elio_echo.sock" << std::endl;
            std::cout << "Use @name for abstract sockets (e.g., @my_socket)" << std::endl;
            return 0;
        } else if (arg[0] != '-') {
            positional.push_back(arg);
        }
    }
    
    // Parse positional argument: [socket_path]
    if (!positional.empty()) {
        socket_path = positional[0];
    }
    
    // Create address - check for abstract socket syntax
    unix_address addr;
    if (!socket_path.empty() && socket_path[0] == '@') {
        // Abstract socket (Linux-specific)
        addr = unix_address::abstract(socket_path.substr(1));
        ELIO_LOG_INFO("Using abstract socket: {}", addr.to_string());
    } else {
        addr = unix_address(socket_path);
        ELIO_LOG_INFO("Using filesystem socket: {}", addr.to_string());
    }
    
    ELIO_LOG_INFO("Target: {}", addr.to_string());
    
    // Create scheduler
    scheduler sched(2);
    
    // Set the I/O context so workers can poll for I/O completions
    sched.set_io_context(&io::default_io_context());
    
    sched.start();
    
    int result = 0;
    std::atomic<bool> done{false};
    
    // Run client
    auto run_client = [&]() -> task<void> {
        if (benchmark) {
            result = co_await benchmark_main(addr, iterations);
        } else {
            result = co_await client_main(addr);
        }
        done = true;
    };
    
    auto client = run_client();
    sched.spawn(client.release());
    
    // Wait for completion
    while (!done) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    
    sched.shutdown();
    
    return result;
}
