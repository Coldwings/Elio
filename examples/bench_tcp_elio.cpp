/// @file bench_tcp_elio.cpp
/// @brief TCP Loopback Benchmark using Elio (coroutine-based)
///
/// Server and client for measuring TCP ping-pong latency and streaming
/// throughput on loopback. Compare with bench_tcp_libuv and bench_tcp_asio.
///
/// Usage: bench_tcp_elio -s  (server)
///        bench_tcp_elio -c  (client, default)

#include <elio/elio.hpp>
#include "bench_tcp_common.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cerrno>
#include <csignal>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

using namespace elio;
using namespace elio::coro;
using namespace elio::runtime;
using namespace elio::net;

static std::atomic<bool> g_running{true};

// ---------------------------------------------------------------------------
// Server: echo handler
// ---------------------------------------------------------------------------

static task<void> echo_handler(tcp_stream stream) {
    char buf[65536];
    while (g_running.load(std::memory_order_relaxed)) {
        auto r = co_await stream.read(buf, sizeof(buf));
        if (r.result <= 0) break;

        // Full write loop (handle partial writes and EAGAIN)
        size_t remaining = r.result;
        const char* ptr  = buf;
        while (remaining > 0) {
            auto w = co_await stream.write(ptr, remaining);
            if (w.result > 0) {
                remaining -= w.result;
                ptr += w.result;
            } else if (w.result == -EAGAIN || w.result == -EWOULDBLOCK) {
                continue;
            } else {
                co_return;
            }
        }
    }
    co_return;
}

// ---------------------------------------------------------------------------
// Server: accept loop
// ---------------------------------------------------------------------------

static task<void> server_main(const bench::config& cfg, scheduler& sched) {
    tcp_options opts;
    opts.no_delay = true;

    auto listener = tcp_listener::bind(ipv4_address(cfg.port), opts);
    if (!listener) {
        std::fprintf(stderr, "Failed to bind port %d\n", cfg.port);
        co_return;
    }

    std::printf("Elio server listening on port %d\n", cfg.port);

    while (g_running.load(std::memory_order_relaxed)) {
        auto stream = co_await listener->accept();
        if (!stream) {
            if (g_running.load(std::memory_order_relaxed)) {
                std::fprintf(stderr, "Accept failed\n");
            }
            break;
        }
        stream->set_no_delay(true);

        sched.go([s = std::move(*stream)]() mutable {
            return echo_handler(std::move(s));
        });
    }

    std::printf("Server shutting down\n");
    co_return;
}

static task<bool> read_exact_cancellable(tcp_stream& stream, char* buf,
                                         size_t n, cancel_token token) {
    while (n > 0) {
        if (token.is_cancelled()) {
            co_return false;
        }

        auto result = co_await io::async_recv(stream.fd(), buf, n, 0, token);
        if (result.was_cancelled()) {
            co_return false;
        }

        if (result.io.result > 0) {
            n -= result.io.result;
            buf += result.io.result;
        } else if (result.io.result == -EAGAIN ||
                   result.io.result == -EWOULDBLOCK ||
                   result.io.result == -EINTR) {
            co_await time::yield();
        } else {
            co_return false;
        }
    }
    co_return true;
}

static task<bool> write_exact_cancellable(tcp_stream& stream, const char* buf,
                                          size_t n, cancel_token token) {
    while (n > 0) {
        if (token.is_cancelled()) {
            co_return false;
        }

        auto result = co_await io::async_send(stream.fd(), buf, n, 0, token);
        if (result.was_cancelled()) {
            co_return false;
        }

        if (result.io.result > 0) {
            n -= result.io.result;
            buf += result.io.result;
        } else if (result.io.result == -EAGAIN ||
                   result.io.result == -EWOULDBLOCK ||
                   result.io.result == -EINTR) {
            co_await time::yield();
        } else {
            co_return false;
        }
    }
    co_return true;
}

// ---------------------------------------------------------------------------
// Client: ping-pong for one message size
// ---------------------------------------------------------------------------

static task<void> client_pingpong(const bench::config& cfg,
                                  size_t msg_size,
                                  bench::pingpong_stats& out) {
    auto resolved = co_await resolve_hostname(cfg.host, cfg.port);
    if (!resolved) {
        std::fprintf(stderr, "Resolve %s failed\n", cfg.host.c_str());
        co_return;
    }

    auto stream = co_await tcp_connect(*resolved);
    if (!stream) {
        std::fprintf(stderr, "Connect to %s:%d failed\n",
                     cfg.host.c_str(), cfg.port);
        co_return;
    }
    stream->set_no_delay(true);

    std::vector<char> send_buf(msg_size, 'X');
    std::vector<char> recv_buf(msg_size);

    std::vector<uint64_t> latencies;
    latencies.reserve(static_cast<size_t>(cfg.duration_s) * 200000);

    cancel_source cancel;
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
        cancel.cancel();
        stream->shutdown_socket();
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

    // Warmup phase
    auto warmup_end = std::chrono::steady_clock::now() +
                      std::chrono::seconds(cfg.warmup_s);
    while (!timed_out.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < warmup_end) {
        if (!co_await write_exact_cancellable(
                *stream, send_buf.data(), msg_size, cancel.get_token())) {
            stop_timer();
            out.timed_out = timed_out.load(std::memory_order_acquire);
            co_return;
        }
        if (!co_await read_exact_cancellable(
                *stream, recv_buf.data(), msg_size, cancel.get_token())) {
            stop_timer();
            out.timed_out = timed_out.load(std::memory_order_acquire);
            co_return;
        }
    }

    latencies.clear();

    // Measurement phase
    auto measure_start = std::chrono::steady_clock::now();
    auto measure_end   = measure_start + std::chrono::seconds(cfg.duration_s);
    auto finish = [&]() {
        double elapsed = std::chrono::duration<double>(
            std::chrono::steady_clock::now() - measure_start).count();
        out = bench::pingpong_stats::compute(latencies, elapsed);
        out.timed_out = timed_out.load(std::memory_order_acquire);
    };

    while (!timed_out.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < measure_end) {
        auto t0 = std::chrono::steady_clock::now();

        if (!co_await write_exact_cancellable(
                *stream, send_buf.data(), msg_size, cancel.get_token())) {
            finish();
            stop_timer();
            co_return;
        }
        if (!co_await read_exact_cancellable(
                *stream, recv_buf.data(), msg_size, cancel.get_token())) {
            finish();
            stop_timer();
            co_return;
        }

        auto t1 = std::chrono::steady_clock::now();
        latencies.push_back(
            std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());
    }

    stop_timer();
    finish();
    co_return;
}

// ---------------------------------------------------------------------------
// Client: streaming writer/reader tasks (standalone for go_joinable)
// ---------------------------------------------------------------------------

struct streaming_ctx {
    tcp_stream* stream;
    const char* send_buf;
    size_t msg_size;
    std::atomic<bool>* stop;
    cancel_token token;
};

struct streaming_counters {
    std::atomic<uint64_t> total_bytes{0};
    std::atomic<uint64_t> total_msgs{0};
};

static task<void> streaming_reader(streaming_ctx* ctx, streaming_counters* ctr) {
    constexpr size_t kReadBufSize = bench::kStreamingRecvBufferSize;
    std::vector<char> recv_buf(kReadBufSize);
    size_t recv_offset = 0;

    while (!ctx->stop->load(std::memory_order_relaxed)) {
        size_t avail = kReadBufSize - recv_offset;
        auto r = co_await io::async_recv(ctx->stream->fd(),
                                         recv_buf.data() + recv_offset,
                                         avail, 0, ctx->token);
        if (r.was_cancelled()) {
            co_return;
        }
        if (r.io.result == -EAGAIN || r.io.result == -EWOULDBLOCK ||
            r.io.result == -EINTR) {
            co_await time::yield();
            continue;
        }
        if (r.io.result <= 0) {
            co_return;
        }
        recv_offset += static_cast<size_t>(r.io.result);

        const char* ptr = recv_buf.data();
        while (recv_offset >= ctx->msg_size) {
            ctr->total_bytes.fetch_add(ctx->msg_size, std::memory_order_relaxed);
            ctr->total_msgs.fetch_add(1, std::memory_order_relaxed);
            ptr += ctx->msg_size;
            recv_offset -= ctx->msg_size;
        }

        if (recv_offset > 0) {
            std::memmove(recv_buf.data(), ptr, recv_offset);
        }
    }
    co_return;
}

// ---------------------------------------------------------------------------
// Client: streaming for one message size
// ---------------------------------------------------------------------------

static task<void> client_streaming(const bench::config& cfg,
                                   size_t msg_size,
                                   bench::streaming_stats& out) {
    auto resolved = co_await resolve_hostname(cfg.host, cfg.port);
    if (!resolved) {
        std::fprintf(stderr, "Resolve %s failed\n", cfg.host.c_str());
        co_return;
    }

    auto stream = co_await tcp_connect(*resolved);
    if (!stream) {
        std::fprintf(stderr, "Connect to %s:%d failed\n",
                     cfg.host.c_str(), cfg.port);
        co_return;
    }
    stream->set_no_delay(true);

    std::vector<char> send_buf(msg_size, 'X');

    std::atomic<bool> stop{false};
    streaming_counters counters;
    cancel_source cancel;

    streaming_ctx ctx{&*stream, send_buf.data(), msg_size, &stop,
                      cancel.get_token()};

    auto* sched = get_current_scheduler();
    auto r_handle = sched->go_joinable(streaming_reader, &ctx, &counters);

    std::atomic<uint64_t> measured_bytes{0};
    std::atomic<uint64_t> measured_msgs{0};
    std::mutex timer_mutex;
    std::condition_variable timer_cv;
    bool timer_cancelled = false;

    std::thread timer_thread([&]() {
        auto wait_or_cancel = [&](std::chrono::seconds duration) {
            std::unique_lock<std::mutex> lock(timer_mutex);
            return timer_cv.wait_for(lock, duration, [&]() {
                return timer_cancelled;
            });
        };

        if (wait_or_cancel(std::chrono::seconds(cfg.warmup_s))) {
            return;
        }

        counters.total_bytes.store(0, std::memory_order_release);
        counters.total_msgs.store(0, std::memory_order_release);

        if (wait_or_cancel(std::chrono::seconds(cfg.duration_s))) {
            return;
        }

        measured_bytes.store(
            counters.total_bytes.load(std::memory_order_acquire),
            std::memory_order_release);
        measured_msgs.store(
            counters.total_msgs.load(std::memory_order_acquire),
            std::memory_order_release);
        stop.store(true, std::memory_order_release);
        cancel.cancel();
        stream->shutdown_socket();
    });

    while (!stop.load(std::memory_order_acquire)) {
        if (!co_await write_exact_cancellable(
                *stream, send_buf.data(), msg_size, cancel.get_token())) {
            break;
        }
    }

    if (!stop.load(std::memory_order_acquire)) {
        measured_bytes.store(
            counters.total_bytes.load(std::memory_order_acquire),
            std::memory_order_release);
        measured_msgs.store(
            counters.total_msgs.load(std::memory_order_acquire),
            std::memory_order_release);
        stop.store(true, std::memory_order_release);
        cancel.cancel();
        stream->shutdown_socket();
    }

    co_await r_handle;

    {
        std::lock_guard<std::mutex> lock(timer_mutex);
        timer_cancelled = true;
    }
    timer_cv.notify_one();
    if (timer_thread.joinable()) {
        timer_thread.join();
    }

    out = bench::streaming_stats::compute(
        measured_bytes.load(std::memory_order_acquire),
        measured_msgs.load(std::memory_order_acquire),
        static_cast<double>(cfg.duration_s));
    co_return;
}

// ---------------------------------------------------------------------------
// Client: drive all message sizes
// ---------------------------------------------------------------------------

static task<void> client_main(const bench::config& cfg,
                              std::atomic<bool>& ok) {
    bench::print_header("Elio", cfg);

    bool do_pingpong  = (cfg.type == bench::config::test_type::pingpong ||
                         cfg.type == bench::config::test_type::both);
    bool do_streaming = (cfg.type == bench::config::test_type::streaming ||
                         cfg.type == bench::config::test_type::both);

    if (do_pingpong) {
        bench::print_pingpong_table_header();
        for (int i = 0; i < bench::kNumMessageSizes; ++i) {
            size_t sz = bench::kMessageSizes[i];
            bench::pingpong_stats stats;
            co_await client_pingpong(cfg, sz, stats);
            stats.print_row(sz);
            if (const char* reason = bench::pingpong_failure_reason(stats)) {
                bench::print_failure("Elio", "ping-pong", sz, reason);
                ok.store(false, std::memory_order_release);
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
            co_await client_streaming(cfg, sz, stats);
            stats.print_row(sz);
            if (const char* reason = bench::streaming_failure_reason(stats)) {
                bench::print_failure("Elio", "streaming", sz, reason);
                ok.store(false, std::memory_order_release);
            }
            std::fflush(stdout);
        }
        std::printf("\n");
    }

    co_return;
}

// ---------------------------------------------------------------------------
// Main
// ---------------------------------------------------------------------------

int main(int argc, char* argv[]) {
    std::signal(SIGPIPE, SIG_IGN);
    elio::log::logger::instance().set_level(elio::log::level::error);
    bench::config cfg;
    try {
        cfg = bench::parse_args(argc, argv, "Elio");
    } catch (const bench::argument_error&) {
        return 1;
    }

    scheduler sched(std::max(1, cfg.threads));
    sched.start();

    std::atomic<bool> done{false};
    std::atomic<bool> client_ok{true};

    if (cfg.run_mode == bench::config::mode::server) {
        sched.go([&]() -> task<void> {
            co_await server_main(cfg, sched);
            done.store(true, std::memory_order_release);
        });
        while (!done.load(std::memory_order_acquire)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    } else {
        sched.go([&]() -> task<void> {
            co_await client_main(cfg, client_ok);
            done.store(true, std::memory_order_release);
        });
        while (!done.load(std::memory_order_acquire)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }

    g_running.store(false, std::memory_order_release);
    sched.shutdown();
    return client_ok.load(std::memory_order_acquire) ? 0 : 1;
}
