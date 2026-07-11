/// @file bench_tcp_asio.cpp
/// @brief TCP Loopback Benchmark using standalone Asio (callback-based)
///
/// Server and client for measuring TCP ping-pong latency and streaming
/// throughput on loopback. Compare with bench_tcp_elio and bench_tcp_libuv.
///
/// Usage: bench_tcp_asio -s  (server)
///        bench_tcp_asio -c  (client, default)

#include <asio.hpp>
#include "bench_tcp_common.hpp"

#include <sys/socket.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <csignal>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using asio::ip::tcp;

// ---------------------------------------------------------------------------
// Server: echo via Asio callbacks
// ---------------------------------------------------------------------------

class echo_session : public std::enable_shared_from_this<echo_session> {
public:
    echo_session(tcp::socket socket) : socket_(std::move(socket)) {}

    void start() {
        asio::error_code ec;
        socket_.set_option(tcp::no_delay(true), ec);
        do_read();
    }

private:
    void do_read() {
        auto self = shared_from_this();
        socket_.async_read_some(
            asio::buffer(buf_, sizeof(buf_)),
            [this, self](std::error_code ec, size_t length) {
                if (!ec) {
                    do_write(length);
                }
            });
    }

    void do_write(size_t length) {
        auto self = shared_from_this();
        asio::async_write(
            socket_, asio::buffer(buf_, length),
            [this, self](std::error_code ec, size_t /*length*/) {
                if (!ec) {
                    do_read();
                }
            });
    }

    tcp::socket socket_;
    char buf_[65536];
};

static void run_server(const bench::config& cfg) {
    asio::io_context io(cfg.threads);

    tcp::acceptor acceptor(io,
        tcp::endpoint(asio::ip::make_address("0.0.0.0"), cfg.port));
    acceptor.set_option(tcp::no_delay(true));

    std::function<void()> do_accept;
    do_accept = [&]() {
        acceptor.async_accept(
            [&](std::error_code ec, tcp::socket socket) {
                if (!ec) {
                    std::make_shared<echo_session>(std::move(socket))->start();
                }
                do_accept();
            });
    };
    do_accept();

    std::printf("Asio server listening on port %d\n", cfg.port);

    std::vector<std::thread> threads;
    for (int i = 1; i < cfg.threads; ++i) {
        threads.emplace_back([&io] { io.run(); });
    }
    io.run();
    for (auto& t : threads) t.join();
}

// ---------------------------------------------------------------------------
// Client: ping-pong
// ---------------------------------------------------------------------------

static void run_client_pingpong(const bench::config& cfg, size_t msg_size,
                                bench::pingpong_stats& out) {
    asio::io_context io;

    tcp::socket socket(io);
    tcp::endpoint ep(asio::ip::make_address(cfg.host), cfg.port);

    std::error_code ec;
    socket.connect(ep, ec);
    if (ec) {
        std::fprintf(stderr, "Connect failed: %s\n", ec.message().c_str());
        return;
    }
    socket.set_option(tcp::no_delay(true));

    std::vector<char> send_buf(msg_size, 'X');
    std::vector<char> recv_buf(msg_size);
    std::atomic<bool> timed_out{false};
    std::mutex timer_mutex;
    std::condition_variable timer_cv;
    bool timer_cancelled = false;

    std::thread timer_thread([&]() {
        std::unique_lock<std::mutex> lock(timer_mutex);
        // This is a stall watchdog, not the normal phase boundary. Warmup and
        // measurement end through the loop deadlines below; allow one extra
        // measurement window (at least 5s) before declaring a real timeout.
        const auto budget = bench::pingpong_watchdog_budget(cfg);
        if (timer_cv.wait_for(lock, budget, [&]() {
                return timer_cancelled;
            })) {
            return;
        }
        timed_out.store(true, std::memory_order_release);
        ::shutdown(socket.native_handle(), SHUT_RDWR);
    });

    auto stop_timer = [&]() {
        {
            std::lock_guard<std::mutex> lock(timer_mutex);
            timer_cancelled = true;
        }
        timer_cv.notify_one();
        if (timer_thread.joinable()) {
            timer_thread.join();
        }
    };

    // Helper: full write
    auto write_all = [&](const char* data, size_t n) -> bool {
        while (n > 0) {
            if (timed_out.load(std::memory_order_acquire)) {
                return false;
            }
            std::error_code wec;
            size_t written = socket.write_some(asio::buffer(data, n), wec);
            if (wec) {
                return false;
            }
            n -= written;
            data += written;
        }
        return true;
    };

    // Helper: full read
    auto read_all = [&](char* data, size_t n) -> bool {
        while (n > 0) {
            if (timed_out.load(std::memory_order_acquire)) {
                return false;
            }
            std::error_code rec;
            size_t got = socket.read_some(asio::buffer(data, n), rec);
            if (rec) {
                return false;
            }
            n -= got;
            data += got;
        }
        return true;
    };

    std::vector<uint64_t> latencies;
    latencies.reserve(static_cast<size_t>(cfg.duration_s) * 200000);

    // Warmup
    auto warmup_end = std::chrono::steady_clock::now() +
                      std::chrono::seconds(cfg.warmup_s);
    while (!timed_out.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < warmup_end) {
        if (!write_all(send_buf.data(), msg_size)) {
            stop_timer();
            out.timed_out = timed_out.load(std::memory_order_acquire);
            return;
        }
        if (!read_all(recv_buf.data(), msg_size)) {
            stop_timer();
            out.timed_out = timed_out.load(std::memory_order_acquire);
            return;
        }
    }

    latencies.clear();

    // Measurement
    auto measure_start = std::chrono::steady_clock::now();
    auto measure_end   = measure_start + std::chrono::seconds(cfg.duration_s);
    auto finish = [&]() {
        out = bench::pingpong_stats::compute(latencies,
            std::chrono::duration<double>(
                std::chrono::steady_clock::now() - measure_start).count());
        out.timed_out = timed_out.load(std::memory_order_acquire);
    };

    while (!timed_out.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < measure_end) {
        auto t0 = std::chrono::steady_clock::now();
        if (!write_all(send_buf.data(), msg_size)) {
            finish();
            stop_timer();
            return;
        }
        if (!read_all(recv_buf.data(), msg_size)) {
            finish();
            stop_timer();
            return;
        }
        auto t1 = std::chrono::steady_clock::now();
        latencies.push_back(
            std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());
    }

    stop_timer();
    finish();
}

// ---------------------------------------------------------------------------
// Client: streaming
// ---------------------------------------------------------------------------

class streaming_session
    : public std::enable_shared_from_this<streaming_session> {
public:
    streaming_session(asio::io_context& io, const bench::config& cfg,
                      size_t msg_size)
        : io_(io)
        , socket_(io)
        , msg_size_(msg_size)
        , pipeline_depth_(std::min(cfg.pipeline_depth, 64))
        , send_buf_(msg_size, 'X')
        , recv_buf_(bench::kStreamingRecvBufferSize)
        , warmup_s_(cfg.warmup_s)
        , duration_s_(cfg.duration_s)
        , timer_(io)
    {
    }

    void run(bench::streaming_stats& out, const std::string& host, uint16_t port) {
        out_ref_ = &out;
        tcp::endpoint ep(asio::ip::make_address(host), port);

        auto self = shared_from_this();
        std::error_code ec;
        socket_.connect(ep, ec);
        if (ec) {
            std::fprintf(stderr, "Connect failed: %s\n", ec.message().c_str());
            return;
        }
        socket_.set_option(tcp::no_delay(true));

        warmup_end_ = std::chrono::steady_clock::now() +
                      std::chrono::seconds(warmup_s_);
        measuring_ = false;
        stopped_   = false;

        // Start reader
        do_read();

        // Start writer pipeline
        for (int i = 0; i < pipeline_depth_; ++i) {
            do_write();
        }

        // Timer for phase transitions
        schedule_phase_check();

        io_.run();

        // Compute results
        double elapsed = std::chrono::duration<double>(
            measure_end_ - measure_start_).count();
        out = bench::streaming_stats::compute(
            total_bytes_.load(), total_msgs_.load(), elapsed);
    }

private:
    void do_read() {
        if (stopped_) return;
        auto self = shared_from_this();
        socket_.async_read_some(
            asio::buffer(recv_buf_.data() + recv_offset_,
                         recv_buf_.size() - recv_offset_),
            [this, self](std::error_code ec, size_t length) {
                if (ec) {
                    stopped_ = true;
                    return;
                }
                size_t total = recv_offset_ + length;
                const char* ptr = recv_buf_.data();
                while (total >= msg_size_) {
                    if (measuring_) {
                        total_bytes_.fetch_add(msg_size_,
                                               std::memory_order_relaxed);
                        total_msgs_.fetch_add(1, std::memory_order_relaxed);
                    }
                    ptr += msg_size_;
                    total -= msg_size_;
                }
                recv_offset_ = total;
                if (total > 0) {
                    std::memmove(recv_buf_.data(), ptr, total);
                }
                do_read();
            });
    }

    void do_write() {
        if (stopped_) return;
        auto self = shared_from_this();
        asio::async_write(
            socket_, asio::buffer(send_buf_.data(), msg_size_),
            [this, self](std::error_code ec, size_t /*length*/) {
                if (ec) {
                    stopped_ = true;
                    return;
                }
                do_write();  // Send next
            });
    }

    void schedule_phase_check() {
        if (stopped_) return;
        auto self = shared_from_this();
        timer_.expires_after(std::chrono::milliseconds(100));
        timer_.async_wait([this, self](std::error_code ec) {
            if (ec || stopped_) return;
            auto now = std::chrono::steady_clock::now();
            if (!measuring_ && now >= warmup_end_) {
                measuring_    = true;
                measure_start_ = now;
                measure_end_  = now + std::chrono::seconds(duration_s_);
                total_bytes_.store(0, std::memory_order_release);
                total_msgs_.store(0, std::memory_order_release);
            }
            if (measuring_ && now >= measure_end_) {
                stopped_ = true;
                std::error_code close_ec;
                socket_.close(close_ec);
                return;
            }
            schedule_phase_check();
        });
    }

    asio::io_context& io_;
    tcp::socket    socket_;
    size_t         msg_size_;
    int            pipeline_depth_;
    std::vector<char> send_buf_;
    std::vector<char> recv_buf_;
    size_t         recv_offset_ = 0;
    std::atomic<uint64_t> total_bytes_{0};
    std::atomic<uint64_t> total_msgs_{0};
    bool           measuring_ = false;
    bool           stopped_ = false;
    int            warmup_s_;
    int            duration_s_;
    asio::steady_timer timer_;
    std::chrono::steady_clock::time_point warmup_end_;
    std::chrono::steady_clock::time_point measure_start_;
    std::chrono::steady_clock::time_point measure_end_;
    bench::streaming_stats* out_ref_ = nullptr;
};

static void run_client_streaming(const bench::config& cfg, size_t msg_size,
                                 bench::streaming_stats& out) {
    auto session = std::make_shared<streaming_session>(
        *new asio::io_context(), cfg, msg_size);
    // Note: io_context leaked intentionally for simplicity in benchmark
    session->run(out, cfg.host, cfg.port);
}

// ---------------------------------------------------------------------------
// Client: drive all message sizes
// ---------------------------------------------------------------------------

static bool run_client(const bench::config& cfg) {
    bench::print_header("Asio", cfg);

    bool do_pingpong  = (cfg.type == bench::config::test_type::pingpong ||
                         cfg.type == bench::config::test_type::both);
    bool do_streaming = (cfg.type == bench::config::test_type::streaming ||
                         cfg.type == bench::config::test_type::both);
    bool ok = true;

    if (do_pingpong) {
        bench::print_pingpong_table_header();
        for (int i = 0; i < bench::kNumMessageSizes; ++i) {
            size_t sz = bench::kMessageSizes[i];
            bench::pingpong_stats stats;
            run_client_pingpong(cfg, sz, stats);
            stats.print_row(sz);
            if (const char* reason = bench::pingpong_failure_reason(stats)) {
                bench::print_failure("Asio", "ping-pong", sz, reason);
                ok = false;
            }
            std::fflush(stdout);
        }
        std::printf("\n");
    }

    if (do_streaming) {
        bench::print_streaming_table_header();
        for (int i = 0; i < bench::kNumMessageSizes; ++i) {
            size_t sz = bench::kMessageSizes[i];
            bench::streaming_stats stats;
            run_client_streaming(cfg, sz, stats);
            stats.print_row(sz);
            if (const char* reason = bench::streaming_failure_reason(stats)) {
                bench::print_failure("Asio", "streaming", sz, reason);
                ok = false;
            }
            std::fflush(stdout);
        }
        std::printf("\n");
    }

    return ok;
}

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------

int main(int argc, char* argv[]) {
    std::signal(SIGPIPE, SIG_IGN);
    bench::config cfg;
    try {
        cfg = bench::parse_args(argc, argv, "Asio");
    } catch (const bench::argument_error&) {
        return 1;
    }

    if (cfg.run_mode == bench::config::mode::server) {
        run_server(cfg);
    } else {
        return run_client(cfg) ? 0 : 1;
    }
    return 0;
}
