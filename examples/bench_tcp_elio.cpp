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

#include <atomic>
#include <chrono>
#include <cerrno>
#include <csignal>
#include <cstring>
#include <string>
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

// ---------------------------------------------------------------------------
// Client: helper — full read (handle partial reads)
// ---------------------------------------------------------------------------

static task<bool> read_exact(tcp_stream& stream, char* buf, size_t n) {
    while (n > 0) {
        auto r = co_await stream.read(buf, n);
        if (r.result <= 0) co_return false;
        n -= r.result;
        buf += r.result;
    }
    co_return true;
}

// ---------------------------------------------------------------------------
// Client: helper — full write (handle partial writes)
// ---------------------------------------------------------------------------

static task<bool> write_exact(tcp_stream& stream, const char* buf, size_t n) {
    while (n > 0) {
        auto w = co_await stream.write(buf, n);
        if (w.result > 0) {
            n -= w.result;
            buf += w.result;
        } else if (w.result == -EAGAIN || w.result == -EWOULDBLOCK) {
            continue;
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

    // Warmup phase
    auto warmup_end = std::chrono::steady_clock::now() +
                      std::chrono::seconds(cfg.warmup_s);
    while (std::chrono::steady_clock::now() < warmup_end) {
        if (!co_await write_exact(*stream, send_buf.data(), msg_size)) co_return;
        if (!co_await read_exact(*stream, recv_buf.data(), msg_size)) co_return;
    }

    latencies.clear();

    // Measurement phase
    auto measure_start = std::chrono::steady_clock::now();
    auto measure_end   = measure_start + std::chrono::seconds(cfg.duration_s);

    while (std::chrono::steady_clock::now() < measure_end) {
        auto t0 = std::chrono::steady_clock::now();

        if (!co_await write_exact(*stream, send_buf.data(), msg_size)) co_return;
        if (!co_await read_exact(*stream, recv_buf.data(), msg_size)) co_return;

        auto t1 = std::chrono::steady_clock::now();
        latencies.push_back(
            std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count());
    }

    double elapsed = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - measure_start).count();
    out = bench::pingpong_stats::compute(latencies, elapsed);
    co_return;
}

// ---------------------------------------------------------------------------
// Client: streaming writer/reader tasks (standalone for go_joinable)
// ---------------------------------------------------------------------------

struct streaming_ctx {
    tcp_stream* stream;
    const char* send_buf;
    size_t msg_size;
    int pipeline_depth;
    std::atomic<bool>* stop;
};

struct streaming_counters {
    std::atomic<uint64_t> total_bytes{0};
    std::atomic<uint64_t> total_msgs{0};
};

static task<void> streaming_writer(streaming_ctx* ctx) {
    while (!ctx->stop->load(std::memory_order_relaxed)) {
        if (!co_await write_exact(*ctx->stream, ctx->send_buf, ctx->msg_size)) {
            co_return;
        }
    }
    co_return;
}

static task<void> streaming_reader(streaming_ctx* ctx, streaming_counters* ctr) {
    constexpr size_t kReadBufSize = 65536;
    std::vector<char> recv_buf(kReadBufSize);
    size_t recv_offset = 0;

    while (!ctx->stop->load(std::memory_order_relaxed)) {
        size_t avail = kReadBufSize - recv_offset;
        auto r = co_await ctx->stream->read(recv_buf.data() + recv_offset, avail);
        if (r.result <= 0) {
            co_return;
        }
        recv_offset += static_cast<size_t>(r.result);

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

    streaming_ctx ctx{&*stream, send_buf.data(), msg_size,
                      cfg.pipeline_depth, &stop};

    auto* sched = get_current_scheduler();

    int depth = std::max(1, cfg.pipeline_depth);
    std::vector<join_handle<void>> w_handles;
    w_handles.reserve(depth);
    for (int i = 0; i < depth; ++i) {
        w_handles.push_back(sched->go_joinable(streaming_writer, &ctx));
    }

    auto r_handle = sched->go_joinable(streaming_reader, &ctx, &counters);

    // Warmup
    co_await elio::time::sleep_for(std::chrono::seconds(cfg.warmup_s));
    counters.total_bytes.store(0, std::memory_order_release);
    counters.total_msgs.store(0, std::memory_order_release);

    // Measure
    auto measure_start = std::chrono::steady_clock::now();
    co_await elio::time::sleep_for(std::chrono::seconds(cfg.duration_s));
    auto measure_end = std::chrono::steady_clock::now();
    stop.store(true, std::memory_order_release);

    for (auto& h : w_handles) {
        co_await h;
    }
    co_await r_handle;

    double elapsed = std::chrono::duration<double>(
        measure_end - measure_start).count();
    out = bench::streaming_stats::compute(
        counters.total_bytes.load(std::memory_order_acquire),
        counters.total_msgs.load(std::memory_order_acquire),
        elapsed);
    co_return;
}

// ---------------------------------------------------------------------------
// Client: drive all message sizes
// ---------------------------------------------------------------------------

static task<void> client_main(const bench::config& cfg) {
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
    auto cfg = bench::parse_args(argc, argv, "Elio");

    scheduler sched(std::max(1, cfg.threads));
    sched.start();

    std::atomic<bool> done{false};

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
            co_await client_main(cfg);
            done.store(true, std::memory_order_release);
        });
        while (!done.load(std::memory_order_acquire)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }

    g_running.store(false, std::memory_order_release);
    sched.shutdown();
    return 0;
}
