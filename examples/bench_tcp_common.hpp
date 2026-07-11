/// @file bench_tcp_common.hpp
/// @brief Shared utilities for TCP loopback benchmarks (Elio vs libuv vs Asio)
///
/// Provides configuration parsing, statistics collection, and consistent
/// output formatting across all three benchmark binaries.

#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <numeric>
#include <string>
#include <vector>

namespace bench {

// ---------------------------------------------------------------------------
// Configuration
// ---------------------------------------------------------------------------

struct config {
    enum class mode { server, client };
    enum class test_type { pingpong, streaming, both };

    mode        run_mode       = mode::client;
    test_type   type           = test_type::both;
    std::string host           = "127.0.0.1";
    uint16_t    port           = 9876;
    int         threads        = 1;
    int         duration_s     = 5;
    int         warmup_s       = 2;
    int         pipeline_depth = 16;
};

inline void print_usage(const char* prog, const char* library_name) {
    std::printf(
        "TCP Loopback Benchmark: %s\n"
        "Usage: %s [options]\n"
        "\n"
        "  -s              Server mode (default: client)\n"
        "  -c              Client mode (explicit)\n"
        "  -H HOST         Server host (default: 127.0.0.1)\n"
        "  -p PORT         Port (default: 9876)\n"
        "  -t THREADS      Thread count (default: 1)\n"
        "  -d SECONDS      Measurement duration per size (default: 5)\n"
        "  -w SECONDS      Warmup per size (default: 2)\n"
        "  -q DEPTH        Pipeline depth for streaming (default: 16)\n"
        "  -m MODE         pingpong | streaming | both (default: both)\n"
        "  -h              Show help\n",
        library_name, prog);
}

inline config parse_args(int argc, char* argv[], const char* library_name) {
    config cfg;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "-s") {
            cfg.run_mode = config::mode::server;
        } else if (arg == "-c") {
            cfg.run_mode = config::mode::client;
        } else if (arg == "-H" && i + 1 < argc) {
            cfg.host = argv[++i];
        } else if (arg == "-p" && i + 1 < argc) {
            cfg.port = static_cast<uint16_t>(std::stoi(argv[++i]));
        } else if (arg == "-t" && i + 1 < argc) {
            cfg.threads = std::stoi(argv[++i]);
        } else if (arg == "-d" && i + 1 < argc) {
            cfg.duration_s = std::stoi(argv[++i]);
        } else if (arg == "-w" && i + 1 < argc) {
            cfg.warmup_s = std::stoi(argv[++i]);
        } else if (arg == "-q" && i + 1 < argc) {
            cfg.pipeline_depth = std::stoi(argv[++i]);
        } else if (arg == "-m" && i + 1 < argc) {
            std::string m = argv[++i];
            if (m == "pingpong") cfg.type = config::test_type::pingpong;
            else if (m == "streaming") cfg.type = config::test_type::streaming;
            else cfg.type = config::test_type::both;
        } else if (arg == "-h" || arg == "--help") {
            print_usage(argv[0], library_name);
            std::exit(0);
        }
    }
    return cfg;
}

// ---------------------------------------------------------------------------
// Message size matrix
// ---------------------------------------------------------------------------

constexpr size_t kMessageSizes[] = {64, 1024, 4096, 65536};
constexpr int kNumMessageSizes = 4;
constexpr size_t kStreamingRecvBufferSize = kMessageSizes[kNumMessageSizes - 1] * 2;

inline std::string format_size(size_t bytes) {
    if (bytes >= 1024) {
        return std::to_string(bytes / 1024) + "KB";
    }
    return std::to_string(bytes) + "B";
}

inline std::chrono::seconds pingpong_watchdog_budget(const config& cfg) {
    const auto phase_budget =
        std::chrono::seconds(cfg.warmup_s + cfg.duration_s);
    const auto watchdog_grace =
        std::chrono::seconds(std::max(5, cfg.duration_s));
    return phase_budget + watchdog_grace;
}

// ---------------------------------------------------------------------------
// Output formatting
// ---------------------------------------------------------------------------

inline void print_header(const char* library_name, const config& cfg) {
    std::printf(
        "\n=== TCP Loopback Benchmark: %s ===\n"
        "Threads: %d | Duration: %ds | Warmup: %ds | Pipeline: %d\n\n",
        library_name, cfg.threads, cfg.duration_s, cfg.warmup_s,
        cfg.pipeline_depth);
}

inline void print_pingpong_table_header() {
    std::printf(
        "[Ping-Pong]\n"
        "%-8s %-10s %-10s %-10s %-10s %-10s\n",
        "Size", "Avg(us)", "Med(us)", "P99(us)", "Max(us)", "Req/s");
}

inline void print_streaming_table_header() {
    std::printf(
        "[Streaming]\n"
        "%-8s %-12s %-12s\n",
        "Size", "MB/s", "IOPS");
}

// ---------------------------------------------------------------------------
// Ping-pong statistics
// ---------------------------------------------------------------------------

struct pingpong_stats {
    double avg_us    = 0.0;
    double median_us = 0.0;
    double p99_us    = 0.0;
    double max_us    = 0.0;
    size_t count     = 0;
    double req_s     = 0.0;
    bool timed_out   = false;

    static pingpong_stats compute(std::vector<uint64_t>& latencies_ns,
                                  double elapsed_s) {
        pingpong_stats s;
        if (latencies_ns.empty()) return s;

        s.count = latencies_ns.size();
        s.req_s = s.count / elapsed_s;

        // Convert to microseconds
        std::vector<double> us(latencies_ns.size());
        for (size_t i = 0; i < latencies_ns.size(); ++i) {
            us[i] = latencies_ns[i] / 1000.0;
        }

        s.avg_us = std::accumulate(us.begin(), us.end(), 0.0) / us.size();
        s.max_us = *std::max_element(us.begin(), us.end());

        // Median via nth_element
        size_t mid = us.size() / 2;
        std::nth_element(us.begin(), us.begin() + mid, us.end());
        s.median_us = us[mid];

        // P99 via nth_element
        size_t p99_idx = static_cast<size_t>(us.size() * 0.99);
        if (p99_idx >= us.size()) p99_idx = us.size() - 1;
        std::nth_element(us.begin(), us.begin() + p99_idx, us.end());
        s.p99_us = us[p99_idx];

        return s;
    }

    void print_row(size_t message_size) const {
        if (timed_out) {
            std::printf("%-8s %-10s %-10s %-10s %-10s %-10s\n",
                        format_size(message_size).c_str(),
                        "TIMEOUT", "TIMEOUT", "TIMEOUT", "TIMEOUT",
                        "TIMEOUT");
            return;
        }
        std::printf("%-8s %-10.2f %-10.2f %-10.2f %-10.2f %-10.0f\n",
                    format_size(message_size).c_str(),
                    avg_us, median_us, p99_us, max_us, req_s);
    }
};

// ---------------------------------------------------------------------------
// Streaming statistics
// ---------------------------------------------------------------------------

struct streaming_stats {
    double mb_s   = 0.0;
    double iops   = 0.0;
    uint64_t total_bytes = 0;
    uint64_t total_msgs  = 0;

    static streaming_stats compute(uint64_t bytes, uint64_t msgs,
                                   double elapsed_s) {
        streaming_stats s;
        s.total_bytes = bytes;
        s.total_msgs  = msgs;
        if (elapsed_s > 0) {
            s.mb_s = (bytes / (1024.0 * 1024.0)) / elapsed_s;
            s.iops = msgs / elapsed_s;
        }
        return s;
    }

    void print_row(size_t message_size) const {
        std::printf("%-8s %-12.2f %-12.0f\n",
                    format_size(message_size).c_str(), mb_s, iops);
    }
};

inline const char* pingpong_failure_reason(const pingpong_stats& stats) noexcept {
    if (stats.timed_out) {
        return "timed out";
    }
    if (stats.count == 0) {
        return "produced no samples";
    }
    return nullptr;
}

inline const char* streaming_failure_reason(const streaming_stats& stats) noexcept {
    if (stats.total_bytes == 0 || stats.total_msgs == 0) {
        return "produced no samples";
    }
    return nullptr;
}

inline void print_failure(const char* library_name,
                          const char* test_name,
                          size_t message_size,
                          const char* reason) {
    std::fflush(stdout);
    std::fprintf(stderr, "%s %s %s failed: %s\n",
                 library_name, test_name, format_size(message_size).c_str(),
                 reason);
    std::fflush(stderr);
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

inline void fill_buffer(char* buf, size_t size) {
    // Fill with a recognizable pattern for debugging
    for (size_t i = 0; i < size; ++i) {
        buf[i] = static_cast<char>('A' + (i % 26));
    }
}

}  // namespace bench
