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

#if !ELIO_HAS_RDMA || !ELIO_HAS_RDMA_IBVERBS || !ELIO_HAS_RDMA_CM || !ELIO_HAS_RDMA_CUDA
#include <cstdio>
int main() {
    std::puts("rdma_gpu_bw requires RDMA + ibverbs + CM + CUDA support. "
              "Build with -DELIO_ENABLE_RDMA=ON -DELIO_ENABLE_RDMA_CM=ON "
              "-DELIO_ENABLE_RDMA_IBVERBS=ON -DELIO_ENABLE_RDMA_CUDA=ON");
    return 0;
}
#else

#include <elio/elio.hpp>
#include <elio/rdma_cuda/rdma_cuda.hpp>
#include <elio/rdma_cm/rdma_cm.hpp>
#include <elio/rdma_ibverbs/rdma_ibverbs.hpp>

#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>

#include <chrono>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

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
    if (cfg.buf_mode != "gpu" && cfg.buf_mode != "cpu"
        && cfg.buf_mode != "both") {
        usage(argv[0]);
    }
    if (cfg.size == 0 || cfg.iters <= 0) usage(argv[0]);
    return cfg;
}

struct bench_result {
    double bw_gbps;
    double avg_lat_us;
};

struct xchg_msg {
    std::uint64_t addr;
    std::uint32_t length;
    std::uint32_t rkey;
};

static sockaddr_in make_addr(const char* host, const std::string& port) {
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    errno = 0;
    char* port_end = nullptr;
    long port_num = std::strtol(port.c_str(), &port_end, 10);
    if (errno != 0 || port_end == port.c_str() || *port_end != '\0'
        || port_num <= 0 || port_num > 65535) {
        std::fprintf(stderr, "Invalid port: %s\n", port.c_str());
        std::exit(1);
    }
    addr.sin_port = htons(static_cast<std::uint16_t>(port_num));
    if (host) {
        if (::inet_pton(AF_INET, host, &addr.sin_addr) != 1) {
            struct addrinfo hints{}, *res = nullptr;
            hints.ai_family = AF_INET;
            hints.ai_socktype = SOCK_STREAM;
            int rc = ::getaddrinfo(host, nullptr, &hints, &res);
            if (rc != 0 || !res) {
                std::fprintf(stderr, "Failed to resolve %s: %s\n",
                             host, ::gai_strerror(rc));
                std::exit(1);
            }
            addr.sin_addr = reinterpret_cast<sockaddr_in*>(
                res->ai_addr)->sin_addr;
            ::freeaddrinfo(res);
        }
    } else {
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
    }
    return addr;
}

static void print_result(const char* label, const bench_result& r) {
    std::printf("  %-8s  %8.2f GB/s   %8.1f us/iter\n",
                label, r.bw_gbps, r.avg_lat_us);
}

static coro::task<bench_result> run_write_bw(rdma_ibverbs::endpoint& ep,
                                             rdma::buffer_view local,
                                             rdma::remote_buffer remote,
                                             int warmup, int iters) {
    for (int i = 0; i < warmup; ++i) {
        auto wc = co_await ep.conn().rdma_write(local, remote);
        if (!wc.ok()) {
            co_return bench_result{0.0, 0.0};
        }
    }

    auto t0 = steady_clock::now();
    for (int i = 0; i < iters; ++i) {
        auto wc = co_await ep.conn().rdma_write(local, remote);
        if (!wc.ok()) {
            co_return bench_result{0.0, 0.0};
        }
    }
    auto t1 = steady_clock::now();

    double elapsed_us = duration_cast<microseconds>(t1 - t0).count();
    double total_bytes = static_cast<double>(local.length) * iters;
    double bw = total_bytes / (elapsed_us * 1e-6) / 1e9;
    co_return bench_result{bw, elapsed_us / iters};
}

coro::task<int> async_main(int argc, char* argv[]) {
    auto cfg = parse_args(argc, argv);

    rdma_ibverbs::endpoint_config ep_cfg;
    ep_cfg.max_send_wr = 128;
    ep_cfg.max_recv_wr = 16;

    auto* sched = runtime::scheduler::current();
    if (!sched) {
        std::fprintf(stderr, "rdma_gpu_bw must run inside an Elio scheduler\n");
        co_return 1;
    }

    if (cfg.is_server) {
        std::printf("Server listening on port %s ...\n", cfg.port.c_str());
        auto addr = make_addr(nullptr, cfg.port);
        rdma_cm::event_channel cm_ch;
        rdma_ibverbs::acceptor ac{
            cm_ch, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)};
        auto ep = co_await ac.accept(ep_cfg);
        ep.start_cq_pump(*sched);
        std::printf("Client connected.\n");

        int result = 0;
        {
            // This scope owns every MR; it ends before endpoint shutdown.
            elio::rdma_cuda::gpu_memory_region gpu_mr{
                ep.pd(), cfg.size,
                IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE |
                IBV_ACCESS_REMOTE_READ};

            auto remote = gpu_mr.remote();
            xchg_msg msg_out{remote.addr, remote.length, remote.rkey};

            auto send_mr = ep.register_buffer(&msg_out, sizeof(msg_out),
                IBV_ACCESS_LOCAL_WRITE);
            auto send_wc = co_await ep.conn().send(send_mr.view());
            if (!send_wc.ok()) {
                std::fprintf(stderr, "server send failed status=%d\n",
                             static_cast<int>(send_wc.status));
                result = 1;
            } else {
                xchg_msg msg_in{};
                auto recv_mr = ep.register_buffer(&msg_in, sizeof(msg_in),
                    IBV_ACCESS_LOCAL_WRITE);
                auto recv_wc = co_await ep.conn().recv(recv_mr.view());
                if (!recv_wc.ok()) {
                    std::fprintf(stderr, "server recv failed status=%d\n",
                                 static_cast<int>(recv_wc.status));
                    result = 1;
                }
            }
        }

        co_await ep.shutdown();
        if (result == 0) {
            std::printf("Benchmark complete.\n");
        }
        co_return result;
    }

    // Client
    std::printf("Connecting to %s:%s ...\n",
                cfg.server_addr.c_str(), cfg.port.c_str());
    auto addr = make_addr(cfg.server_addr.c_str(), cfg.port);
    rdma_cm::event_channel cm_ch;
    auto ep = co_await rdma_ibverbs::connect(
        cm_ch, reinterpret_cast<sockaddr*>(&addr), sizeof(addr), ep_cfg);
    ep.start_cq_pump(*sched);

    int result = 0;
    {
        // Receive server's remote buffer info. All MRs in this scope are
        // released before endpoint shutdown below.
        xchg_msg srv_info{};
        auto recv_mr = ep.register_buffer(&srv_info, sizeof(srv_info),
            IBV_ACCESS_LOCAL_WRITE);
        auto recv_wc = co_await ep.conn().recv(recv_mr.view());
        if (!recv_wc.ok()) {
            std::fprintf(stderr, "client recv failed status=%d\n",
                         static_cast<int>(recv_wc.status));
            result = 1;
        } else {
            rdma::remote_buffer server_buf{
                srv_info.addr, srv_info.length, srv_info.rkey};
            std::printf("Connected. Server buffer: %zu bytes\n", cfg.size);
            std::printf(
                "Running RDMA WRITE bandwidth test (%d iters, %zu B):\n\n",
                cfg.iters, cfg.size);

            if (cfg.buf_mode == "gpu" || cfg.buf_mode == "both") {
                elio::rdma_cuda::gpu_memory_region gpu_mr{
                    ep.pd(), cfg.size, IBV_ACCESS_LOCAL_WRITE};
                auto r = co_await run_write_bw(
                    ep, gpu_mr.view(), server_buf, DEFAULT_WARMUP, cfg.iters);
                print_result("GPU", r);
            }

            if (cfg.buf_mode == "cpu" || cfg.buf_mode == "both") {
                std::vector<char> cpu_buf(cfg.size, 'A');
                auto cpu_mr = ep.register_buffer(
                    cpu_buf.data(), cfg.size, IBV_ACCESS_LOCAL_WRITE);
                auto r = co_await run_write_bw(
                    ep, cpu_mr.view(), server_buf, DEFAULT_WARMUP, cfg.iters);
                print_result("CPU", r);
            }

            xchg_msg done_msg{};
            auto done_mr = ep.register_buffer(
                &done_msg, sizeof(done_msg), IBV_ACCESS_LOCAL_WRITE);
            auto done_wc = co_await ep.conn().send(done_mr.view());
            if (!done_wc.ok()) {
                std::fprintf(stderr, "client done send failed status=%d\n",
                             static_cast<int>(done_wc.status));
                result = 1;
            }
        }
    }

    co_await ep.shutdown();
    if (result == 0) {
        std::printf("\nDone.\n");
    }
    co_return result;
}

ELIO_ASYNC_MAIN(async_main)

#endif
