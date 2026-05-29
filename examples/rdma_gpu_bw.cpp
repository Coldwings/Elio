/// @file rdma_gpu_bw.cpp
/// @brief GPU memory RDMA bandwidth benchmark.
///
/// Demonstrates GPUDirect RDMA: the NIC reads/writes GPU device memory
/// directly without staging through host RAM.
///
/// Usage:
///   rdma_gpu_bw -s               # server
///   rdma_gpu_bw -c <server_ip>   # client
///   rdma_gpu_bw -c <server_ip> -b gpu   # GPU buffer only
///   rdma_gpu_bw -c <server_ip> -b cpu   # CPU buffer only (baseline)
///   rdma_gpu_bw -c <server_ip> -b both  # compare side-by-side
///
/// Prerequisites:
///   - NVIDIA GPU with GPUDirect RDMA support
///   - nvidia-peermem kernel module loaded
///   - RDMA-capable NIC (ConnectX-5+)

#include <elio/elio.hpp>
#include <elio/rdma_cuda/rdma_cuda.hpp>
#include <elio/rdma_ibverbs/endpoint.hpp>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include <cuda_runtime.h>
#include <infiniband/verbs.h>

using namespace elio;
using namespace std::chrono;

static constexpr std::size_t DEFAULT_SIZE   = 1 << 20; // 1 MB
static constexpr int         DEFAULT_ITERS  = 1000;
static constexpr int         DEFAULT_WARMUP = 50;

struct config {
    std::string server_addr;
    std::string port = "18515";
    bool        is_server = false;
    std::string buf_mode  = "both"; // gpu, cpu, both
    std::size_t size      = DEFAULT_SIZE;
    int         iters     = DEFAULT_ITERS;
};

static void usage(const char* prog) {
    std::fprintf(stderr,
        "Usage:\n"
        "  %s -s [-p port]                       (server)\n"
        "  %s -c <addr> [-p port] [-b gpu|cpu|both] [-n size] [-i iters]\n",
        prog, prog);
    std::exit(1);
}

static config parse_args(int argc, char* argv[]) {
    config cfg;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "-s") { cfg.is_server = true; }
        else if (arg == "-c" && i + 1 < argc) { cfg.server_addr = argv[++i]; }
        else if (arg == "-p" && i + 1 < argc) { cfg.port = argv[++i]; }
        else if (arg == "-b" && i + 1 < argc) { cfg.buf_mode = argv[++i]; }
        else if (arg == "-n" && i + 1 < argc) { cfg.size = std::stoull(argv[++i]); }
        else if (arg == "-i" && i + 1 < argc) { cfg.iters = std::stoi(argv[++i]); }
        else { usage(argv[0]); }
    }
    if (!cfg.is_server && cfg.server_addr.empty()) usage(argv[0]);
    return cfg;
}

struct bench_result {
    double bw_gbps;
    double avg_lat_us;
};

static void print_result(const char* label, const bench_result& r) {
    std::printf("  %-8s  %8.2f GB/s   %8.1f us/iter\n",
                label, r.bw_gbps, r.avg_lat_us);
}

static bench_result run_write_bw(rdma_ibverbs::endpoint& ep,
                                  rdma::buffer_view local,
                                  rdma::remote_buffer remote,
                                  int warmup, int iters) {
    for (int i = 0; i < warmup; ++i)
        sync_wait(ep.conn().rdma_write(local, remote));

    auto t0 = steady_clock::now();
    for (int i = 0; i < iters; ++i)
        sync_wait(ep.conn().rdma_write(local, remote));
    auto t1 = steady_clock::now();

    double elapsed_us = duration_cast<microseconds>(t1 - t0).count();
    double total_bytes = static_cast<double>(local.length) * iters;
    double bw = total_bytes / (elapsed_us * 1e-6) / 1e9;
    return {bw, elapsed_us / iters};
}

coro::task<int> async_main(int argc, char* argv[]) {
    auto cfg = parse_args(argc, argv);

    rdma_ibverbs::endpoint_config ep_cfg;
    ep_cfg.max_send_wr = 128;
    ep_cfg.max_recv_wr = 16;

    if (cfg.is_server) {
        std::printf("Server listening on port %s ...\n", cfg.port.c_str());
        auto ep = co_await rdma_ibverbs::endpoint::accept(
            cfg.port.c_str(), ep_cfg);
        std::printf("Client connected.\n");

        // Allocate GPU buffer and register
        elio::rdma_cuda::gpu_memory_region gpu_mr{
            ep.pd(), cfg.size,
            IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE |
            IBV_ACCESS_REMOTE_READ};

        // Exchange remote info via SEND/RECV
        auto remote = gpu_mr.remote();
        struct xchg_msg { uint64_t addr; uint32_t rkey; uint64_t size; };
        xchg_msg msg_out{remote.addr, remote.rkey, cfg.size};

        auto send_mr = ep.register_buffer(&msg_out, sizeof(msg_out),
            IBV_ACCESS_LOCAL_WRITE);
        rdma::buffer_view send_view{&msg_out, sizeof(msg_out), send_mr.lkey()};
        co_await ep.conn().send(send_view);

        // Wait for client "done" signal
        xchg_msg msg_in{};
        auto recv_mr = ep.register_buffer(&msg_in, sizeof(msg_in),
            IBV_ACCESS_LOCAL_WRITE);
        rdma::buffer_view recv_view{&msg_in, sizeof(msg_in), recv_mr.lkey()};
        co_await ep.conn().post_recv(recv_view);
        co_await ep.conn().recv();

        std::printf("Benchmark complete.\n");
        co_return 0;
    }

    // Client
    std::printf("Connecting to %s:%s ...\n",
                cfg.server_addr.c_str(), cfg.port.c_str());
    auto ep = co_await rdma_ibverbs::endpoint::connect(
        cfg.server_addr.c_str(), cfg.port.c_str(), ep_cfg);

    // Receive server's remote buffer info
    struct xchg_msg { uint64_t addr; uint32_t rkey; uint64_t size; };
    xchg_msg srv_info{};
    auto recv_mr = ep.register_buffer(&srv_info, sizeof(srv_info),
        IBV_ACCESS_LOCAL_WRITE);
    rdma::buffer_view recv_view{&srv_info, sizeof(srv_info), recv_mr.lkey()};
    co_await ep.conn().post_recv(recv_view);
    co_await ep.conn().recv();

    rdma::remote_buffer server_buf{srv_info.addr, srv_info.rkey, srv_info.size};
    std::printf("Connected. Server buffer: %zu bytes\n", cfg.size);
    std::printf("Running RDMA WRITE bandwidth test (%d iters, %zu B):\n\n",
                cfg.iters, cfg.size);

    // GPU path
    if (cfg.buf_mode == "gpu" || cfg.buf_mode == "both") {
        elio::rdma_cuda::gpu_memory_region gpu_mr{
            ep.pd(), cfg.size, IBV_ACCESS_LOCAL_WRITE};
        auto r = run_write_bw(ep, gpu_mr.view(), server_buf,
                              DEFAULT_WARMUP, cfg.iters);
        print_result("GPU", r);
    }

    // CPU path
    if (cfg.buf_mode == "cpu" || cfg.buf_mode == "both") {
        std::vector<char> cpu_buf(cfg.size, 'A');
        auto cpu_mr = ep.register_buffer(cpu_buf.data(), cfg.size,
            IBV_ACCESS_LOCAL_WRITE);
        rdma::buffer_view cpu_view{cpu_buf.data(), cfg.size, cpu_mr.lkey()};
        auto r = run_write_bw(ep, cpu_view, server_buf,
                              DEFAULT_WARMUP, cfg.iters);
        print_result("CPU", r);
    }

    // Signal server we're done
    xchg_msg done_msg{};
    auto done_mr = ep.register_buffer(&done_msg, sizeof(done_msg),
        IBV_ACCESS_LOCAL_WRITE);
    rdma::buffer_view done_view{&done_msg, sizeof(done_msg), done_mr.lkey()};
    co_await ep.conn().send(done_view);

    std::printf("\nDone.\n");
    co_return 0;
}

ELIO_ASYNC_MAIN(async_main)
