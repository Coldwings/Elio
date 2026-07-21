/// @file rdma_req_resp_ibverbs.cpp
/// @brief Single-process client / server demo:
///        client SEND request → server RDMA_WRITE response →
///        server SEND_WITH_IMM "done" notify.
///
/// Showcases the high-level surface added in S11–S13:
///   * `elio::rdma_ibverbs::endpoint`   bundles pd / cq / qp /
///                                      dispatcher / cq_pump
///   * `elio::rdma_ibverbs::acceptor`   server-side CM accept+QP
///   * `elio::rdma_ibverbs::connect`    client-side CM resolve+QP
///   * `send_with_imm`                  OOB completion notification
///
/// Both client and server run in the same process for ease of
/// running. Requires a working uverbs ABI; SKIPs cleanly on hosts
/// without one (OrbStack, no-RDMA boxes).

#if !ELIO_HAS_RDMA || !ELIO_HAS_RDMA_IBVERBS || !ELIO_HAS_RDMA_CM
int main() { return 0; }
#else

#include <elio/elio.hpp>
#include <elio/rdma/rdma.hpp>
#include <elio/rdma_cm/rdma_cm.hpp>
#include <elio/rdma_ibverbs/rdma_ibverbs.hpp>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <rdma/rdma_cma.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <exception>
#include <iostream>
#include <string>
#include <thread>
#include <utility>

namespace {

struct request_header {
    std::uint64_t resp_addr;
    std::uint32_t resp_length;
    std::uint32_t resp_rkey;
    std::uint32_t client_id;   // demo business field
    std::uint32_t pad;
};

constexpr std::uint16_t kPort = 18515;
constexpr std::uint32_t kImmDoneTag = 0xABCDEFu;

void abandon_outstanding_or_terminate(
    elio::rdma_ibverbs::endpoint& ep) noexcept {
    if (!ep.abandon_outstanding()) {
        std::terminate();
    }
}

elio::coro::task<void> serve_request(
    elio::rdma_ibverbs::endpoint& ep) {
    std::array<char, 256> req_buf{};
    std::array<char, 4096> payload_buf{};
    std::array<std::uint32_t, 1> notify_buf{};

    auto req_mr = ep.register_buffer(
        req_buf.data(), req_buf.size(), IBV_ACCESS_LOCAL_WRITE);
    auto payload_mr = ep.register_buffer(
        payload_buf.data(), payload_buf.size(), IBV_ACCESS_LOCAL_WRITE);
    auto notify_mr = ep.register_buffer(
        notify_buf.data(), sizeof(notify_buf), IBV_ACCESS_LOCAL_WRITE);

    auto req_wc = co_await ep.conn().recv(req_mr.view());
    if (!req_wc.ok()) {
        std::cerr << "server: recv failed status="
                  << static_cast<int>(req_wc.status) << "\n";
        co_return;
    }
    auto* hdr = reinterpret_cast<request_header*>(req_buf.data());
    std::cout << "server: got request from client "
              << hdr->client_id << "\n";

    std::string body = "response for client " + std::to_string(hdr->client_id);
    std::memcpy(payload_buf.data(), body.data(), body.size());
    elio::rdma::remote_buffer dst{
        hdr->resp_addr, hdr->resp_length, hdr->resp_rkey};

    auto wr = co_await ep.conn().rdma_write(
        payload_mr.view(0, body.size()), dst);
    if (!wr.ok()) {
        std::cerr << "server: rdma_write failed status="
                  << static_cast<int>(wr.status) << "\n";
        co_return;
    }

    auto notify = co_await ep.conn().send_with_imm(
        notify_mr.view(0, 0), static_cast<std::uint32_t>(body.size()));
    if (!notify.ok()) {
        std::cerr << "server: notify send failed status="
                  << static_cast<int>(notify.status) << "\n";
    }
}

elio::coro::task<void> server(elio::runtime::scheduler& sched) {
    sockaddr_in bind_addr{};
    bind_addr.sin_family      = AF_INET;
    bind_addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    bind_addr.sin_port        = htons(kPort);

    elio::rdma_cm::event_channel cm_ch;
    elio::rdma_ibverbs::acceptor ac{
        cm_ch, reinterpret_cast<sockaddr*>(&bind_addr), sizeof(bind_addr)};

    while (true) {
        auto ep = co_await ac.accept(
            elio::rdma_ibverbs::endpoint_config{
                .max_send_wr = 4, .max_recv_wr = 4});
        ep.start_cq_pump(sched);

        // The helper frame owns every MR and completed operation. It is
        // destroyed before terminal endpoint shutdown begins.
        co_await serve_request(ep);
        co_await ep.shutdown();
        co_return;  // single-shot demo
    }
}

elio::coro::task<bool> run_client_request(
    elio::rdma_ibverbs::endpoint& ep) {
    std::array<char, 64> req_buf{};
    std::array<char, 4096> resp_buf{};
    std::array<std::uint32_t, 1> notify_buf{};

    auto req_mr = ep.register_buffer(
        req_buf.data(), req_buf.size(), IBV_ACCESS_LOCAL_WRITE);
    auto resp_mr = ep.register_buffer(
        resp_buf.data(), resp_buf.size(),
        IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE);
    auto notify_mr = ep.register_buffer(
        notify_buf.data(), sizeof(notify_buf), IBV_ACCESS_LOCAL_WRITE);

    auto notify_awaiter = ep.conn().recv(notify_mr.view()).start();

    auto* hdr = reinterpret_cast<request_header*>(req_buf.data());
    auto resp_remote = resp_mr.remote();
    hdr->resp_addr = resp_remote.addr;
    hdr->resp_length = resp_remote.length;
    hdr->resp_rkey = resp_remote.rkey;
    hdr->client_id = 7;

    auto send_wc = co_await ep.conn().send(
        req_mr.view(0, sizeof(request_header)));
    if (!send_wc.ok()) {
        std::cerr << "client: send failed status="
                  << static_cast<int>(send_wc.status) << "\n";
        // The pre-posted notify receive may still be provider-owned. Destroy
        // the QP while this frame still owns its awaiter, MRs, and buffers;
        // the terminal fail-closed path intentionally leaks the remaining
        // endpoint resources.
        abandon_outstanding_or_terminate(ep);
        co_return false;
    }

    auto notify_wc = co_await std::move(notify_awaiter);
    if (!notify_wc.ok()) {
        std::cerr << "client: notify recv failed status="
                  << static_cast<int>(notify_wc.status) << "\n";
        co_return true;
    }
    const auto resp_len = notify_wc.imm_data;
    std::cout << "client: server wrote " << resp_len
              << " bytes: '"
              << std::string(resp_buf.data(), resp_len) << "'\n";
    co_return true;
}

elio::coro::task<void> client(elio::runtime::scheduler& sched) {
    sockaddr_in dst_addr{};
    dst_addr.sin_family      = AF_INET;
    dst_addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    dst_addr.sin_port        = htons(kPort);

    elio::rdma_cm::event_channel cm_ch;
    auto ep = co_await elio::rdma_ibverbs::connect(
        cm_ch, reinterpret_cast<sockaddr*>(&dst_addr), sizeof(dst_addr),
        elio::rdma_ibverbs::endpoint_config{
            .max_send_wr = 4, .max_recv_wr = 4});
    ep.start_cq_pump(sched);

    if (co_await run_client_request(ep)) {
        co_await ep.shutdown();
    }
}

}  // namespace

int main() {
    // We can probe the verbs ABI by trying to enumerate devices.
    // If none are usable (OrbStack, no RDMA stack) we print and exit
    // cleanly instead of failing the example build's smoke run.
    int n = 0;
    ibv_device** list = ::ibv_get_device_list(&n);
    if (list) ::ibv_free_device_list(list);
    if (n == 0) {
        std::cout << "no RDMA devices visible; example is a no-op on "
                     "this host. Build with rxe loaded to run it.\n";
        return 0;
    }

    elio::runtime::scheduler sched(2);
    sched.start();

    std::atomic<bool> server_started{false};

    // Start the server first so its bind/listen lands before the
    // client tries to connect.
    sched.go([&]() -> elio::coro::task<void> {
        server_started.store(true);
        co_await server(sched);
    });

    // Tiny wait so server's listener is up.
    auto deadline = std::chrono::steady_clock::now()
                  + std::chrono::seconds(2);
    while (!server_started.load()
           && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(50));

    sched.go([&]() -> elio::coro::task<void> {
        co_await client(sched);
    });

    std::this_thread::sleep_for(std::chrono::seconds(2));
    sched.shutdown(std::chrono::seconds(5));
    return 0;
}

#endif  // ELIO_HAS_RDMA_*
