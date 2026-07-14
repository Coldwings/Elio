/// @file rdma_pingpong_mock.cpp
/// @brief Single-process RDMA SEND/RECV ping-pong on a mock backend.
///
/// This example does NOT require any real RDMA hardware or
/// libibverbs. The "mock backend" implements `backend_traits` by
/// pairing SEND WRs against RECV WRs in-process: when a SEND is
/// posted, the matching RECV (if one is queued) is satisfied by
/// memcpy and both sides' CQEs are delivered via `dispatcher.deliver`.
///
/// The point of this example is to demonstrate end-to-end use of
/// every Elio surface (`connection`, `send/recv`, `wc_result`,
/// `dispatcher`, `cq_pump`) on something CI-buildable. To run
/// against real ibverbs, swap the mock for a backend that calls
/// `ibv_post_send` / `ibv_post_recv` and use the cq_pump coroutine
/// against a real `ibv_comp_channel` fd.

#if !ELIO_HAS_RDMA
int main() { return 0; }
#else

#include <elio/elio.hpp>
#include <elio/rdma/rdma.hpp>

#include <sys/eventfd.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <deque>
#include <iostream>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <thread>
#include <vector>

using elio::coro::cancel_source;
using elio::coro::task;
using elio::rdma::buffer_view;
using elio::rdma::connection;
using elio::rdma::cq_pump;
using elio::rdma::dispatcher;
using elio::rdma::remote_buffer;
using elio::rdma::send_flags;
using elio::rdma::sge;
using elio::rdma::wc_status;
using elio::rdma::wr_id;
using elio::runtime::scheduler;

namespace {

// One side of the mock connection. Holds a posted-RECV queue plus an
// inbound SEND queue waiting to be matched against future RECVs.
struct mock_qp {
    struct posted_recv {
        sge   buf;        // single SGE; the example doesn't need multi
        wr_id id;
    };
    struct pending_send {
        std::vector<char> payload;
        wr_id             id;
    };

    std::mutex                mu;
    std::deque<posted_recv>   recv_q;     // waiting for a SEND
    std::deque<pending_send>  inbox_q;    // SEND arrived before RECV
    dispatcher*               local_disp  = nullptr;
    dispatcher*               peer_disp   = nullptr;
    mock_qp*                  peer        = nullptr;
    int                       wake_fd     = -1;  // peer's wake_fd
};

void wake(int fd) noexcept {
    std::uint64_t one = 1;
    auto n = ::write(fd, &one, sizeof(one));
    (void)n;
}

// `mock_backend` is a static-traits backend that interprets the
// QP-opaque `void* qp` as a mock_qp*. Each post_* call records the
// WR and (for sends) attempts to satisfy a matching recv on the
// peer's queue.
struct mock_backend {
    static int post_send(void* qp_ptr, std::span<const sge> sges,
                         send_flags /*flags*/, std::uint32_t /*imm*/,
                         wr_id id) noexcept {
        auto* q = static_cast<mock_qp*>(qp_ptr);
        mock_qp::pending_send msg;
        msg.id = id;
        for (const auto& s : sges) {
            msg.payload.insert(msg.payload.end(),
                               static_cast<char*>(s.addr),
                               static_cast<char*>(s.addr) + s.length);
        }
        // Match against peer's posted recvs.
        mock_qp::posted_recv recv_to_serve{};
        bool matched = false;
        {
            std::lock_guard lg{q->peer->mu};
            if (!q->peer->recv_q.empty()) {
                recv_to_serve = q->peer->recv_q.front();
                q->peer->recv_q.pop_front();
                matched = true;
            } else {
                q->peer->inbox_q.push_back(std::move(msg));
            }
        }
        if (matched) {
            // Deliver SEND completion locally.
            const std::uint32_t sent_bytes =
                static_cast<std::uint32_t>(msg.payload.size());
            q->local_disp->deliver(id, wc_status::success, sent_bytes);

            // Copy into the peer's posted recv buffer, deliver RECV CQE.
            const std::uint32_t copied = std::min(
                static_cast<std::uint32_t>(msg.payload.size()),
                static_cast<std::uint32_t>(recv_to_serve.buf.length));
            std::memcpy(recv_to_serve.buf.addr, msg.payload.data(), copied);
            q->peer->local_disp->deliver(recv_to_serve.id,
                                         wc_status::success, copied);
            // Wake the peer's cq pump (in case it's blocked in poll).
            wake(q->peer->wake_fd);
        } else {
            // Defer SEND completion until matched, but here we still
            // need to wake the peer so its drain pulls from the inbox.
            wake(q->peer->wake_fd);
        }
        return 0;
    }

    static int post_recv(void* qp_ptr, std::span<const sge> sges,
                         wr_id id) noexcept {
        auto* q = static_cast<mock_qp*>(qp_ptr);
        mock_qp::posted_recv r{sges.empty() ? sge{} : sges.front(), id};
        mock_qp::pending_send msg{};
        bool matched = false;
        {
            std::lock_guard lg{q->mu};
            if (!q->inbox_q.empty()) {
                msg = std::move(q->inbox_q.front());
                q->inbox_q.pop_front();
                matched = true;
            } else {
                q->recv_q.push_back(r);
            }
        }
        if (matched) {
            // Peer's SEND has been waiting. Deliver its CQE and ours.
            const std::uint32_t copied = std::min(
                static_cast<std::uint32_t>(msg.payload.size()),
                static_cast<std::uint32_t>(r.buf.length));
            std::memcpy(r.buf.addr, msg.payload.data(), copied);
            q->local_disp->deliver(id, wc_status::success, copied);
            q->peer->local_disp->deliver(
                msg.id, wc_status::success,
                static_cast<std::uint32_t>(msg.payload.size()));
            wake(q->wake_fd);
            wake(q->peer->wake_fd);
        }
        return 0;
    }

    // The mock doesn't implement one-sided ops or MR / SRQ in this
    // example — those would need extra plumbing.
    static int post_rdma_write(void*, std::span<const sge>,
                               remote_buffer, send_flags,
                               std::uint32_t, wr_id) noexcept {
        return -95;  // -ENOTSUP
    }
    static int post_rdma_read(void*, std::span<const sge>,
                              remote_buffer, wr_id) noexcept {
        return -95;
    }
};

// Eventfd-based wake fd handed to the local cq_pump so post_* on the
// peer side can wake us out of `async_poll_read`. The drain itself
// just reads the eventfd value; the dispatcher CQE deliveries are
// done synchronously inside post_send / post_recv (a real backend
// would queue them and let the cq_pump pull from a CQE ring).
struct mock_cq {
    int fd = ::eventfd(0, EFD_NONBLOCK);
};

}  // namespace

int main() {
    constexpr int kRounds = 16;
    constexpr std::size_t kPayload = 128;

    scheduler sched(2);
    sched.start();

    // Two CQ wake fds + two dispatchers + two mock_qps wired peer-to-peer.
    mock_cq cq_a, cq_b;
    dispatcher disp_a, disp_b;
    mock_qp qp_a, qp_b;

    qp_a.local_disp = &disp_a;
    qp_a.peer_disp  = &disp_b;
    qp_a.peer       = &qp_b;
    qp_a.wake_fd    = cq_a.fd;

    qp_b.local_disp = &disp_b;
    qp_b.peer_disp  = &disp_a;
    qp_b.peer       = &qp_a;
    qp_b.wake_fd    = cq_b.fd;

    cancel_source pump_cancel;

    // CQ-pump drain: just consume the eventfd counter. The actual
    // dispatcher.deliver() calls already happened synchronously
    // inside post_*; this pump is only here to demonstrate the
    // cq_pump shape on a real fd.
    auto drain = [](dispatcher& /*d*/) noexcept {
        // No-op: in this mock the deliveries were inline.
    };

    sched.go([&]() -> task<void> {
        co_await cq_pump(cq_a.fd, disp_a, drain, pump_cancel.get_token());
    });
    sched.go([&]() -> task<void> {
        co_await cq_pump(cq_b.fd, disp_b, drain, pump_cancel.get_token());
    });

    connection<mock_backend> conn_a{&qp_a, disp_a};
    connection<mock_backend> conn_b{&qp_b, disp_b};

    std::atomic<int> rounds_done{0};

    // B side: echoes back whatever it receives.
    sched.go([&]() -> task<void> {
        std::vector<char> rx_buf(kPayload);
        std::vector<char> tx_buf(kPayload);
        for (int i = 0; i < kRounds; ++i) {
            auto rx = co_await conn_b.recv(
                buffer_view{rx_buf.data(), rx_buf.size(), /*lkey=*/0});
            if (!rx.ok()) {
                std::cerr << "B recv error round " << i
                          << " status=" << static_cast<int>(rx.status)
                          << "\n";
                co_return;
            }
            std::memcpy(tx_buf.data(), rx_buf.data(), rx.byte_len);
            auto tx = co_await conn_b.send(
                buffer_view{tx_buf.data(), rx.byte_len, /*lkey=*/0});
            if (!tx.ok()) {
                std::cerr << "B send error round " << i << "\n";
                co_return;
            }
        }
    });

    // A side: sends "ping N", expects the same back.
    sched.go([&]() -> task<void> {
        std::vector<char> tx_buf(kPayload, 0);
        std::vector<char> rx_buf(kPayload, 0);
        for (int i = 0; i < kRounds; ++i) {
            // Post the recv first so the echoed reply has a buffer.
            // In real RDMA the order matters — recv WRs must be
            // posted before the peer's send completes.
            auto recv_awaitable =
                conn_a.recv(buffer_view{rx_buf.data(), rx_buf.size(),
                                        /*lkey=*/0})
                    .start();

            std::string msg = "ping " + std::to_string(i);
            std::memcpy(tx_buf.data(), msg.data(), msg.size());

            auto send_wc = co_await conn_a.send(
                buffer_view{tx_buf.data(), msg.size(), /*lkey=*/0});
            if (!send_wc.ok()) {
                std::cerr << "A send error\n";
                co_return;
            }

            auto recv_wc = co_await std::move(recv_awaitable);
            if (!recv_wc.ok()) {
                std::cerr << "A recv error\n";
                co_return;
            }
            std::string echoed(rx_buf.data(), recv_wc.byte_len);
            std::cout << "round " << i << ": echoed '" << echoed << "'\n";
            rounds_done.fetch_add(1);
        }
    });

    auto deadline = std::chrono::steady_clock::now()
                  + std::chrono::seconds(5);
    while (rounds_done.load() < kRounds
           && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    pump_cancel.cancel();
    wake(cq_a.fd);
    wake(cq_b.fd);
    sched.shutdown(std::chrono::milliseconds(5000));

    ::close(cq_a.fd);
    ::close(cq_b.fd);

    if (rounds_done.load() != kRounds) {
        std::cerr << "FAILED: only " << rounds_done.load()
                  << " of " << kRounds << " rounds completed\n";
        return 1;
    }
    std::cout << "OK: " << kRounds << " round-trips on mock backend\n";
    return 0;
}

#endif  // ELIO_HAS_RDMA
