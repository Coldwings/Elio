/// @file tcp_echo_client.cpp
/// @brief TCP Echo Client Example
/// 
/// This example demonstrates how to create a TCP client using Elio's
/// async I/O. The client connects to an echo server, sends messages,
/// and receives the echoed responses.
///
/// Supports both IPv4 and IPv6:
///   - Hostname resolution auto-detects address family
///   - Use IPv4 address (e.g., 127.0.0.1) for explicit IPv4
///   - Use IPv6 address (e.g., ::1) for explicit IPv6
///
/// Usage: ./tcp_echo_client [options] [host] [port]
///   -b, --benchmark    Run benchmark mode
///   -n <count>         Number of iterations (default: 10000)
///   -h, --help         Show help
/// Default: localhost:8080

#include <elio/elio.hpp>
#include <iostream>
#include <string>
#include <vector>

using namespace elio;
using namespace elio::coro;
using namespace elio::runtime;
using namespace elio::net;

/// Client coroutine - connects, sends messages, receives responses
task<int> client_main(std::string_view host, uint16_t port) {
    ELIO_LOG_INFO("Connecting to {}:{}...", host, port);
    
    // Connect to server
    auto stream_result = co_await tcp_connect(host, port);
    
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
task<int> benchmark_main(std::string_view host, uint16_t port, int iterations) {
    ELIO_LOG_INFO("Connecting to {}:{} for benchmark...", host, port);
    
    auto stream_result = co_await tcp_connect(host, port);
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
    std::string host = "localhost";  // Will resolve via DNS (IPv4 or IPv6)
    uint16_t port = 8080;
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
            std::cout << "Usage: " << argv[0] << " [options] [host] [port]" << std::endl;
            std::cout << "Options:" << std::endl;
            std::cout << "  -b, --benchmark    Run benchmark mode" << std::endl;
            std::cout << "  -n <count>         Number of iterations (default: 10000)" << std::endl;
            std::cout << "  -h, --help         Show this help" << std::endl;
            std::cout << std::endl;
            std::cout << "IPv4/IPv6 examples:" << std::endl;
            std::cout << "  " << argv[0] << " localhost 8080     # Auto-detect" << std::endl;
            std::cout << "  " << argv[0] << " 127.0.0.1 8080     # IPv4" << std::endl;
            std::cout << "  " << argv[0] << " ::1 8080           # IPv6" << std::endl;
            std::cout << std::endl;
            std::cout << "Default: localhost:8080" << std::endl;
            return 0;
        } else if (arg[0] != '-') {
            positional.push_back(arg);
        }
    }
    
    // Parse positional arguments: [host] [port] or just [port]
    if (positional.size() == 1) {
        // Could be just port or just host
        try {
            port = static_cast<uint16_t>(std::stoi(positional[0]));
        } catch (...) {
            host = positional[0];
        }
    } else if (positional.size() >= 2) {
        host = positional[0];
        port = static_cast<uint16_t>(std::stoi(positional[1]));
    }
    
    ELIO_LOG_INFO("Target: {}:{}", host, port);
    
    // Create scheduler
    scheduler sched(2);
    
    sched.start();
    
    int result = 0;
    std::atomic<bool> done{false};
    
    // Run client
    auto run_client = [&]() -> task<void> {
        if (benchmark) {
            result = co_await benchmark_main(host, port, iterations);
        } else {
            result = co_await client_main(host, port);
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
