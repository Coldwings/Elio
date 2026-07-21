/// @file rdma_perf.cpp
/// @brief iperf-like throughput / latency benchmark for RDMA.
///
/// Usage:
///   rdma_perf -s [-p PORT]                          # server
///   rdma_perf -c HOST [-p PORT] [-m SIZE] [-n COUNT] [-q DEPTH] [-t MODE]
///
///   -s            server mode
///   -c HOST       client mode, connect to HOST
///   -p PORT       port (default 18515)
///   -m SIZE       message size in bytes (default 4096)
///   -n COUNT      number of messages (default 100000)
///   -q DEPTH      pipeline depth (default 32)
///   -t MODE       test mode: send | write | read (default send)
///
/// Requires a working uverbs ABI (Soft-RoCE / real HCA).

#if !ELIO_HAS_RDMA || !ELIO_HAS_RDMA_IBVERBS || !ELIO_HAS_RDMA_CM
#include <cstdio>
int main() {
    std::puts("rdma_perf requires RDMA + ibverbs + CM support. "
              "Build with -DELIO_ENABLE_RDMA=ON "
              "-DELIO_ENABLE_RDMA_CM=ON -DELIO_ENABLE_RDMA_IBVERBS=ON");
    return 0;
}
#else

#include <elio/elio.hpp>
#include <elio/rdma/rdma.hpp>
#include <elio/rdma_cm/rdma_cm.hpp>
#include <elio/rdma_ibverbs/rdma_ibverbs.hpp>

#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <exception>
#include <string>
#include <utility>
#include <vector>

using elio::coro::task;
using elio::rdma::buffer_view;
using elio::rdma::remote_buffer;
using elio::rdma::send_flags;
using elio::rdma::wc_status;

namespace {

// ------------------------------------------------------------------
// Config
// ------------------------------------------------------------------

enum class test_mode { send, write, read };
enum class session_result { success, failed_safe, failed_outstanding };

void abandon_outstanding_or_terminate(
    elio::rdma_ibverbs::endpoint& ep) noexcept {
    if (!ep.abandon_outstanding()) {
        std::terminate();
    }
}

struct config {
    bool        is_server = false;
    std::string host;
    std::uint16_t port    = 18515;
    std::size_t msg_size  = 4096;
    std::size_t count     = 100000;
    std::size_t depth     = 32;
    test_mode   mode      = test_mode::send;
};

// Exchange metadata for RDMA WRITE/READ (addr, rkey, length).
struct mr_info {
    std::uint64_t addr;
    std::uint32_t length;
    std::uint32_t rkey;
};

// ------------------------------------------------------------------
// Helpers
// ------------------------------------------------------------------

sockaddr_in make_addr(const char* host, std::uint16_t port) {
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port   = htons(port);
    if (host) {
        if (::inet_pton(AF_INET, host, &addr.sin_addr) != 1) {
            struct addrinfo hints{}, *res = nullptr;
            hints.ai_family = AF_INET;
            if (::getaddrinfo(host, nullptr, &hints, &res) == 0 && res) {
                addr.sin_addr = reinterpret_cast<sockaddr_in*>(
                    res->ai_addr)->sin_addr;
                ::freeaddrinfo(res);
            }
        }
    } else {
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
    }
    return addr;
}

void print_results(const config& cfg,
                   std::chrono::nanoseconds elapsed,
                   std::size_t total_bytes) {
    const double secs = elapsed.count() / 1e9;
    const double mbps = (total_bytes / 1e6) / secs;
    const double gbps = (total_bytes * 8.0 / 1e9) / secs;
    const double iops = cfg.count / secs;
    const double lat_us = (secs * 1e6) / cfg.count;

    const char* mode_str = cfg.mode == test_mode::send  ? "SEND/RECV"
                         : cfg.mode == test_mode::write ? "RDMA WRITE"
                         :                                "RDMA READ";

    std::printf("\n--- rdma_perf results ---\n");
    std::printf("Mode:          %s\n", mode_str);
    std::printf("Message size:  %zu bytes\n", cfg.msg_size);
    std::printf("Messages:      %zu\n", cfg.count);
    std::printf("Pipeline depth:%zu\n", cfg.depth);
    std::printf("Elapsed:       %.3f s\n", secs);
    std::printf("Throughput:    %.2f MB/s  (%.2f Gbps)\n", mbps, gbps);
    std::printf("IOPS:          %.0f\n", iops);
    std::printf("Avg latency:   %.2f us\n", lat_us);
}

elio::rdma_ibverbs::endpoint_config make_ep_cfg(const config& cfg) {
    const auto depth32 = static_cast<std::uint32_t>(cfg.depth + 4);
    return {
        .max_send_wr     = depth32,
        .max_recv_wr     = depth32,
        .max_send_sge    = 1,
        .max_recv_sge    = 1,
        .max_inline_data = cfg.msg_size <= 64 ? static_cast<std::uint32_t>(cfg.msg_size) : 0u,
    };
}

// ------------------------------------------------------------------
// SEND/RECV benchmark
// ------------------------------------------------------------------

task<session_result> send_server(elio::rdma_ibverbs::endpoint& ep,
                                 const config& cfg) {
    const auto depth = std::max<std::size_t>(cfg.depth, 1);
    std::vector<char> buf(cfg.msg_size * depth);
    auto mr = ep.register_buffer(buf.data(), buf.size(),
                                 IBV_ACCESS_LOCAL_WRITE);

    std::size_t posted = 0, completed = 0;
    std::deque<decltype(ep.conn().recv(mr.view(0, cfg.msg_size)).start())>
        inflight;

    while (completed < cfg.count) {
        while (posted < cfg.count && (posted - completed) < depth) {
            const auto offset = (posted % depth) * cfg.msg_size;
            inflight.push_back(
                ep.conn().recv(mr.view(offset, cfg.msg_size)).start());
            ++posted;
        }

        auto wc = co_await std::move(inflight.front());
        inflight.pop_front();
        if (!wc.ok()) {
            std::fprintf(stderr, "server recv error #%zu status=%d\n",
                         completed, static_cast<int>(wc.status));
            if (!inflight.empty()) {
                abandon_outstanding_or_terminate(ep);
                co_return session_result::failed_outstanding;
            }
            co_return session_result::failed_safe;
        }
        ++completed;
    }
    // Send a single ACK so client knows we're done.
    auto ack = co_await ep.conn().send(mr.view(0, 1));
    co_return ack.ok() ? session_result::success
                       : session_result::failed_safe;
}

task<session_result> send_client(elio::rdma_ibverbs::endpoint& ep,
                                 const config& cfg) {
    std::vector<char> tx_buf(cfg.msg_size, 'A');
    std::vector<char> ack_buf(cfg.msg_size);
    auto tx_mr = ep.register_buffer(tx_buf.data(), tx_buf.size(),
                                    IBV_ACCESS_LOCAL_WRITE);
    auto ack_mr = ep.register_buffer(ack_buf.data(), ack_buf.size(),
                                     IBV_ACCESS_LOCAL_WRITE);

    // Pre-post all recv WRs up front (just one for the final ACK).
    auto ack_aw = ep.conn().recv(ack_mr.view(0, 1)).start();

    const auto start = std::chrono::steady_clock::now();

    // Pipeline: keep `depth` sends in flight.
    const auto depth = std::max<std::size_t>(cfg.depth, 1);
    std::size_t posted = 0, completed = 0;
    std::deque<decltype(ep.conn().send(tx_mr.view()).start())> inflight;

    while (completed < cfg.count) {
        // Post up to depth.
        while (posted < cfg.count && (posted - completed) < depth) {
            inflight.push_back(ep.conn().send(tx_mr.view()).start());
            ++posted;
        }
        // Drain one.
        if (!inflight.empty()) {
            auto wc = co_await std::move(inflight.front());
            inflight.pop_front();
            if (!wc.ok()) {
                std::fprintf(stderr, "client send error status=%d\n",
                             static_cast<int>(wc.status));
                abandon_outstanding_or_terminate(ep);
                co_return session_result::failed_outstanding;
            }
            ++completed;
        }
    }

    // Wait for server ACK.
    auto ack = co_await std::move(ack_aw);
    if (!ack.ok()) {
        co_return session_result::failed_safe;
    }

    const auto end = std::chrono::steady_clock::now();
    print_results(cfg, end - start, cfg.count * cfg.msg_size);
    co_return session_result::success;
}

// ------------------------------------------------------------------
// RDMA WRITE benchmark
// ------------------------------------------------------------------

task<session_result> write_server(elio::rdma_ibverbs::endpoint& ep,
                                  const config& cfg) {
    // Expose a buffer for the client to RDMA WRITE into.
    std::vector<char> buf(cfg.msg_size);
    auto mr = ep.register_buffer(buf.data(), buf.size(),
                                 IBV_ACCESS_LOCAL_WRITE |
                                 IBV_ACCESS_REMOTE_WRITE);
    auto remote = mr.remote();

    // Wait until the client has started its metadata receive.
    std::vector<char> ready_buf(1);
    auto ready_mr = ep.register_buffer(ready_buf.data(), ready_buf.size(),
                                       IBV_ACCESS_LOCAL_WRITE);
    auto ready = co_await ep.conn().recv(ready_mr.view(0, 1));
    if (!ready.ok()) {
        std::fprintf(stderr, "server: client-ready recv failed status=%d\n",
                     static_cast<int>(ready.status));
        co_return session_result::failed_safe;
    }

    // Send our MR info to the client.
    mr_info info{remote.addr, remote.length, remote.rkey};
    std::vector<char> ctrl(sizeof(info));
    std::memcpy(ctrl.data(), &info, sizeof(info));
    auto ctrl_mr = ep.register_buffer(ctrl.data(), ctrl.size(),
                                      IBV_ACCESS_LOCAL_WRITE);
    auto ctrl_wc = co_await ep.conn().send(ctrl_mr.view(0, sizeof(info)));
    if (!ctrl_wc.ok()) {
        std::fprintf(stderr, "server: failed to send MR info status=%d\n",
                     static_cast<int>(ctrl_wc.status));
        co_return session_result::failed_safe;
    }

    // Wait for client "done" signal (a single SEND).
    std::vector<char> done_buf(4);
    auto done_mr = ep.register_buffer(done_buf.data(), done_buf.size(),
                                      IBV_ACCESS_LOCAL_WRITE);
    auto done = co_await ep.conn().recv(done_mr.view());
    co_return done.ok() ? session_result::success
                        : session_result::failed_safe;
}

task<session_result> write_client(elio::rdma_ibverbs::endpoint& ep,
                                  const config& cfg) {
    // Receive server's MR info.
    mr_info info{};
    std::vector<char> ctrl(sizeof(info));
    auto ctrl_mr = ep.register_buffer(ctrl.data(), ctrl.size(),
                                      IBV_ACCESS_LOCAL_WRITE);
    auto ctrl_recv = ep.conn().recv(ctrl_mr.view(0, sizeof(info))).start();

    std::vector<char> ready_buf(1, 'R');
    auto ready_mr = ep.register_buffer(ready_buf.data(), ready_buf.size(),
                                       IBV_ACCESS_LOCAL_WRITE);
    auto ready_wc = co_await ep.conn().send(ready_mr.view(0, 1));
    if (!ready_wc.ok()) {
        std::fprintf(stderr, "client: failed to send metadata-ready signal\n");
        abandon_outstanding_or_terminate(ep);
        co_return session_result::failed_outstanding;
    }

    auto wc = co_await std::move(ctrl_recv);
    if (!wc.ok()) {
        std::fprintf(stderr, "client: failed to recv MR info\n");
        co_return session_result::failed_safe;
    }
    std::memcpy(&info, ctrl.data(), sizeof(info));
    remote_buffer rb{info.addr, info.length, info.rkey};

    std::vector<char> tx_buf(cfg.msg_size, 'W');
    auto tx_mr = ep.register_buffer(tx_buf.data(), tx_buf.size(),
                                    IBV_ACCESS_LOCAL_WRITE);

    const auto start = std::chrono::steady_clock::now();

    const auto depth = std::max<std::size_t>(cfg.depth, 1);
    std::size_t posted = 0, completed = 0;
    std::deque<decltype(ep.conn().rdma_write(tx_mr.view(), rb).start())>
        inflight;

    while (completed < cfg.count) {
        while (posted < cfg.count && (posted - completed) < depth) {
            inflight.push_back(ep.conn().rdma_write(tx_mr.view(), rb).start());
            ++posted;
        }
        if (!inflight.empty()) {
            auto r = co_await std::move(inflight.front());
            inflight.pop_front();
            if (!r.ok()) {
                std::fprintf(stderr, "write error status=%d\n",
                             static_cast<int>(r.status));
                if (!inflight.empty()) {
                    abandon_outstanding_or_terminate(ep);
                    co_return session_result::failed_outstanding;
                }
                co_return session_result::failed_safe;
            }
            ++completed;
        }
    }

    const auto end = std::chrono::steady_clock::now();

    // Signal server that we're done.
    std::vector<char> done_buf(4, 'D');
    auto done_mr = ep.register_buffer(done_buf.data(), done_buf.size(),
                                      IBV_ACCESS_LOCAL_WRITE);
    auto done = co_await ep.conn().send(done_mr.view(0, 1));
    if (!done.ok()) {
        co_return session_result::failed_safe;
    }

    print_results(cfg, end - start, cfg.count * cfg.msg_size);
    co_return session_result::success;
}

// ------------------------------------------------------------------
// RDMA READ benchmark
// ------------------------------------------------------------------

task<session_result> read_server(elio::rdma_ibverbs::endpoint& ep,
                                 const config& cfg) {
    // Expose a source buffer for the client to RDMA READ from.
    std::vector<char> buf(cfg.msg_size, 'R');
    auto mr = ep.register_buffer(buf.data(), buf.size(),
                                 IBV_ACCESS_LOCAL_WRITE |
                                 IBV_ACCESS_REMOTE_READ);
    auto remote = mr.remote();

    // Wait until the client has started its metadata receive.
    std::vector<char> ready_buf(1);
    auto ready_mr = ep.register_buffer(ready_buf.data(), ready_buf.size(),
                                       IBV_ACCESS_LOCAL_WRITE);
    auto ready = co_await ep.conn().recv(ready_mr.view(0, 1));
    if (!ready.ok()) {
        std::fprintf(stderr, "server: client-ready recv failed status=%d\n",
                     static_cast<int>(ready.status));
        co_return session_result::failed_safe;
    }

    mr_info info{remote.addr, remote.length, remote.rkey};
    std::vector<char> ctrl(sizeof(info));
    std::memcpy(ctrl.data(), &info, sizeof(info));
    auto ctrl_mr = ep.register_buffer(ctrl.data(), ctrl.size(),
                                      IBV_ACCESS_LOCAL_WRITE);
    auto ctrl_wc = co_await ep.conn().send(ctrl_mr.view(0, sizeof(info)));
    if (!ctrl_wc.ok()) {
        std::fprintf(stderr, "server: failed to send MR info status=%d\n",
                     static_cast<int>(ctrl_wc.status));
        co_return session_result::failed_safe;
    }

    // Wait for client "done" signal.
    std::vector<char> done_buf(4);
    auto done_mr = ep.register_buffer(done_buf.data(), done_buf.size(),
                                      IBV_ACCESS_LOCAL_WRITE);
    auto done = co_await ep.conn().recv(done_mr.view());
    co_return done.ok() ? session_result::success
                        : session_result::failed_safe;
}

task<session_result> read_client(elio::rdma_ibverbs::endpoint& ep,
                                 const config& cfg) {
    // Receive server's MR info.
    mr_info info{};
    std::vector<char> ctrl(sizeof(info));
    auto ctrl_mr = ep.register_buffer(ctrl.data(), ctrl.size(),
                                      IBV_ACCESS_LOCAL_WRITE);
    auto ctrl_recv = ep.conn().recv(ctrl_mr.view(0, sizeof(info))).start();

    std::vector<char> ready_buf(1, 'R');
    auto ready_mr = ep.register_buffer(ready_buf.data(), ready_buf.size(),
                                       IBV_ACCESS_LOCAL_WRITE);
    auto ready_wc = co_await ep.conn().send(ready_mr.view(0, 1));
    if (!ready_wc.ok()) {
        std::fprintf(stderr, "client: failed to send metadata-ready signal\n");
        abandon_outstanding_or_terminate(ep);
        co_return session_result::failed_outstanding;
    }

    auto wc = co_await std::move(ctrl_recv);
    if (!wc.ok()) {
        std::fprintf(stderr, "client: failed to recv MR info\n");
        co_return session_result::failed_safe;
    }
    std::memcpy(&info, ctrl.data(), sizeof(info));
    remote_buffer rb{info.addr, info.length, info.rkey};

    const auto depth = std::max<std::size_t>(cfg.depth, 1);
    std::vector<char> rx_buf(cfg.msg_size * depth);
    auto rx_mr = ep.register_buffer(rx_buf.data(), rx_buf.size(),
                                    IBV_ACCESS_LOCAL_WRITE);

    const auto start = std::chrono::steady_clock::now();

    std::size_t posted = 0, completed = 0;
    std::deque<decltype(
        ep.conn().rdma_read(rx_mr.view(0, cfg.msg_size), rb).start())> inflight;

    while (completed < cfg.count) {
        while (posted < cfg.count && (posted - completed) < depth) {
            const auto offset = (posted % depth) * cfg.msg_size;
            inflight.push_back(
                ep.conn()
                    .rdma_read(rx_mr.view(offset, cfg.msg_size), rb)
                    .start());
            ++posted;
        }
        if (!inflight.empty()) {
            auto r = co_await std::move(inflight.front());
            inflight.pop_front();
            if (!r.ok()) {
                std::fprintf(stderr, "read error status=%d\n",
                             static_cast<int>(r.status));
                if (!inflight.empty()) {
                    abandon_outstanding_or_terminate(ep);
                    co_return session_result::failed_outstanding;
                }
                co_return session_result::failed_safe;
            }
            ++completed;
        }
    }

    const auto end = std::chrono::steady_clock::now();

    std::vector<char> done_buf(4, 'D');
    auto done_mr = ep.register_buffer(done_buf.data(), done_buf.size(),
                                      IBV_ACCESS_LOCAL_WRITE);
    auto done = co_await ep.conn().send(done_mr.view(0, 1));
    if (!done.ok()) {
        co_return session_result::failed_safe;
    }

    print_results(cfg, end - start, cfg.count * cfg.msg_size);
    co_return session_result::success;
}

// ------------------------------------------------------------------
// Main entry
// ------------------------------------------------------------------

task<void> run_server(elio::runtime::scheduler& sched,
                      const config& cfg) {
    auto addr = make_addr(nullptr, cfg.port);
    elio::rdma_cm::event_channel cm_ch;
    elio::rdma_ibverbs::acceptor ac{
        cm_ch, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)};

    std::printf("rdma_perf server listening on port %u  (Ctrl+C to stop)\n",
                cfg.port);

    while (true) {
        auto ep = co_await ac.accept(make_ep_cfg(cfg));
        ep.start_cq_pump(sched);
        std::printf("client connected\n");

        // Pre-post a recv for SEND-based modes (server needs recv WRs
        // ready before client sends).
        auto result = session_result::failed_safe;
        switch (cfg.mode) {
            case test_mode::send:
                result = co_await send_server(ep, cfg);
                break;
            case test_mode::write:
                result = co_await write_server(ep, cfg);
                break;
            case test_mode::read:
                result = co_await read_server(ep, cfg);
                break;
        }
        // Graceful paths have released every helper-owned MR/awaiter before
        // shutdown. An outstanding failure already called abandon_outstanding()
        // from inside that helper while those objects were still alive.
        if (result != session_result::failed_outstanding) {
            co_await ep.shutdown();
        }
        if (result != session_result::success) {
            std::fprintf(stderr, "session failed\n");
        }
        std::printf("session complete, waiting for next client...\n");
    }
}

task<int> run_client(elio::runtime::scheduler& sched,
                     const config& cfg) {
    auto addr = make_addr(cfg.host.c_str(), cfg.port);
    elio::rdma_cm::event_channel cm_ch;
    auto ep = co_await elio::rdma_ibverbs::connect(
        cm_ch, reinterpret_cast<sockaddr*>(&addr), sizeof(addr),
        make_ep_cfg(cfg));
    ep.start_cq_pump(sched);

    const char* mode_str = cfg.mode == test_mode::send  ? "SEND/RECV"
                         : cfg.mode == test_mode::write ? "RDMA WRITE"
                         :                                "RDMA READ";
    std::printf("connected to %s:%u  mode=%s  msg=%zu  count=%zu  depth=%zu\n",
                cfg.host.c_str(), cfg.port, mode_str,
                cfg.msg_size, cfg.count, cfg.depth);

    auto result = session_result::failed_safe;
    switch (cfg.mode) {
        case test_mode::send:
            result = co_await send_client(ep, cfg);
            break;
        case test_mode::write:
            result = co_await write_client(ep, cfg);
            break;
        case test_mode::read:
            result = co_await read_client(ep, cfg);
            break;
    }

    // A failed pipelined session can still own provider work through a
    // pre-posted receive or remaining WRs. Its helper has already destroyed the
    // QP fail-closed while the associated MRs and buffers were still alive;
    // completed error paths are safe to shut down gracefully.
    if (result != session_result::failed_outstanding) {
        co_await ep.shutdown();
    }
    co_return result == session_result::success ? 0 : 1;
}

void usage(const char* prog) {
    std::printf(
        "Usage:\n"
        "  %s -s [-p PORT]\n"
        "  %s -c HOST [-p PORT] [-m SIZE] [-n COUNT] [-q DEPTH] [-t MODE]\n"
        "\n"
        "Options:\n"
        "  -s            server mode\n"
        "  -c HOST       client mode\n"
        "  -p PORT       port (default 18515)\n"
        "  -m SIZE       message size in bytes (default 4096)\n"
        "  -n COUNT      number of messages (default 100000)\n"
        "  -q DEPTH      pipeline depth (default 32)\n"
        "  -t MODE       send | write | read (default send)\n",
        prog, prog);
}

}  // namespace

int main(int argc, char* argv[]) {
    // Probe the verbs ABI.
    {
        int n = 0;
        ibv_device** list = ::ibv_get_device_list(&n);
        if (list) ::ibv_free_device_list(list);
        if (n == 0) {
            std::printf("no RDMA devices; rdma_perf requires Soft-RoCE "
                        "or a real HCA.\n");
            return 0;
        }
    }

    config cfg;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "-s") {
            cfg.is_server = true;
        } else if (arg == "-c" && i + 1 < argc) {
            cfg.host = argv[++i];
        } else if (arg == "-p" && i + 1 < argc) {
            cfg.port = static_cast<std::uint16_t>(std::atoi(argv[++i]));
        } else if (arg == "-m" && i + 1 < argc) {
            cfg.msg_size = static_cast<std::size_t>(std::atol(argv[++i]));
        } else if (arg == "-n" && i + 1 < argc) {
            cfg.count = static_cast<std::size_t>(std::atol(argv[++i]));
        } else if (arg == "-q" && i + 1 < argc) {
            cfg.depth = static_cast<std::size_t>(std::atol(argv[++i]));
        } else if (arg == "-t" && i + 1 < argc) {
            std::string mode = argv[++i];
            if (mode == "send")       cfg.mode = test_mode::send;
            else if (mode == "write") cfg.mode = test_mode::write;
            else if (mode == "read")  cfg.mode = test_mode::read;
            else {
                std::fprintf(stderr, "unknown mode: %s\n", mode.c_str());
                usage(argv[0]);
                return 1;
            }
        } else if (arg == "-h" || arg == "--help") {
            usage(argv[0]);
            return 0;
        } else {
            std::fprintf(stderr, "unknown option: %s\n", arg.c_str());
            usage(argv[0]);
            return 1;
        }
    }

    if (!cfg.is_server && cfg.host.empty()) {
        usage(argv[0]);
        return 1;
    }

    elio::runtime::scheduler sched(2);
    sched.start();

    std::atomic<int> exit_code{0};
    std::atomic<bool> done{false};

    if (cfg.is_server) {
        sched.go([&]() -> task<void> {
            co_await run_server(sched, cfg);
        });
        // Server runs until killed.
        std::printf("press Ctrl+C to stop\n");
        pause();
    } else {
        sched.go([&]() -> task<void> {
            exit_code.store(co_await run_client(sched, cfg));
            done.store(true);
        });
        auto deadline = std::chrono::steady_clock::now()
                      + std::chrono::seconds(300);
        while (!done.load() &&
               std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
        }
    }

    sched.shutdown(std::chrono::seconds(5));
    return exit_code.load();
}

#endif  // ELIO_HAS_RDMA_*
