// Stage S10 — Soft-RoCE (rxe) loopback integration test.
//
// Tag: [rdma_integration]. Skipped automatically on hosts without a
// usable rxe device — the OrbStack development host falls into that
// bucket (kernel exposes rxe metadata but userspace uverbs returns
// EPERM). On a stock Linux with rxe loaded the test creates two RC
// QPs in-process, brings them up to RTS via direct attribute
// exchange (no CM), and runs a single SEND → RECV through the full
// Elio data-path: connection<ibverbs_backend>, dispatcher,
// op_state lifecycle.
//
// Setup (one-time, on the host):
//
//     sudo modprobe rdma_rxe
//     sudo rdma link add rxe0 type rxe netdev <NIC>
//     # /dev/infiniband/uverbs0 should appear; if not, mknod by
//     # checking sysfs.
//
// Run only the integration test:
//     ./elio_tests "[rdma_integration]"

#include <catch2/catch_test_macros.hpp>

#include <elio/elio.hpp>
#include <elio/rdma/rdma.hpp>
#include <elio/rdma_ibverbs/rdma_ibverbs.hpp>

#include <infiniband/verbs.h>

#include <atomic>
#include <chrono>
#include <cstring>
#include <thread>
#include <vector>

using elio::rdma_ibverbs::ibverbs_backend;
using elio::rdma_ibverbs::translate_status;
using elio::coro::cancel_source;
using elio::coro::task;
using elio::rdma::buffer_view;
using elio::rdma::connection;
using elio::rdma::dispatcher;
using elio::rdma::memory_region;
using elio::rdma::wc_status;
using elio::runtime::scheduler;

namespace {

// Best-effort device opener. Returns nullptr if rxe isn't usable on
// this host; the test SKIPs in that case.
ibv_context* open_rxe_or_null() {
    int num_devs = 0;
    ibv_device** list = ::ibv_get_device_list(&num_devs);
    if (!list || num_devs == 0) {
        if (list) ::ibv_free_device_list(list);
        return nullptr;
    }
    ibv_context* ctx = nullptr;
    for (int i = 0; i < num_devs; ++i) {
        const char* name = ::ibv_get_device_name(list[i]);
        if (name && std::strncmp(name, "rxe", 3) == 0) {
            ctx = ::ibv_open_device(list[i]);
            if (ctx) break;
        }
    }
    // If no rxe-prefixed device, fall back to first available.
    if (!ctx) {
        ctx = ::ibv_open_device(list[0]);
    }
    ::ibv_free_device_list(list);
    return ctx;
}

struct qp_attrs {
    uint32_t qpn;
    uint16_t lid;
    union ibv_gid gid;
};

bool query_port_(ibv_context* ctx, uint8_t port, uint16_t* lid,
                 ibv_gid* gid) {
    ibv_port_attr p{};
    if (::ibv_query_port(ctx, port, &p) != 0) return false;
    *lid = p.lid;
    return ::ibv_query_gid(ctx, port, /*gid_idx=*/0, gid) == 0;
}

bool modify_to_init_(ibv_qp* qp, uint8_t port) {
    ibv_qp_attr attr{};
    attr.qp_state        = IBV_QPS_INIT;
    attr.pkey_index      = 0;
    attr.port_num        = port;
    attr.qp_access_flags = IBV_ACCESS_REMOTE_READ
                         | IBV_ACCESS_REMOTE_WRITE
                         | IBV_ACCESS_LOCAL_WRITE;
    return ::ibv_modify_qp(qp, &attr,
                           IBV_QP_STATE | IBV_QP_PKEY_INDEX
                           | IBV_QP_PORT | IBV_QP_ACCESS_FLAGS) == 0;
}

bool modify_to_rtr_(ibv_qp* qp, const qp_attrs& peer, uint8_t port) {
    ibv_qp_attr attr{};
    attr.qp_state       = IBV_QPS_RTR;
    attr.path_mtu       = IBV_MTU_1024;
    attr.dest_qp_num    = peer.qpn;
    attr.rq_psn         = 0;
    attr.max_dest_rd_atomic = 1;
    attr.min_rnr_timer  = 12;
    attr.ah_attr.is_global     = 1;
    attr.ah_attr.dlid          = peer.lid;
    attr.ah_attr.sl            = 0;
    attr.ah_attr.src_path_bits = 0;
    attr.ah_attr.port_num      = port;
    attr.ah_attr.grh.dgid          = peer.gid;
    attr.ah_attr.grh.flow_label    = 0;
    attr.ah_attr.grh.hop_limit     = 1;
    attr.ah_attr.grh.sgid_index    = 0;
    attr.ah_attr.grh.traffic_class = 0;
    return ::ibv_modify_qp(qp, &attr,
                           IBV_QP_STATE | IBV_QP_AV | IBV_QP_PATH_MTU
                           | IBV_QP_DEST_QPN | IBV_QP_RQ_PSN
                           | IBV_QP_MAX_DEST_RD_ATOMIC
                           | IBV_QP_MIN_RNR_TIMER) == 0;
}

bool modify_to_rts_(ibv_qp* qp) {
    ibv_qp_attr attr{};
    attr.qp_state      = IBV_QPS_RTS;
    attr.timeout       = 14;
    attr.retry_cnt     = 7;
    attr.rnr_retry     = 7;
    attr.sq_psn        = 0;
    attr.max_rd_atomic = 1;
    return ::ibv_modify_qp(qp, &attr,
                           IBV_QP_STATE | IBV_QP_TIMEOUT
                           | IBV_QP_RETRY_CNT | IBV_QP_RNR_RETRY
                           | IBV_QP_SQ_PSN | IBV_QP_MAX_QP_RD_ATOMIC) == 0;
}

}  // namespace

TEST_CASE("rxe loopback: SEND → RECV via Elio's connection<Backend>",
          "[rdma_integration][rxe][!benchmark]") {
    ibv_context* ctx = open_rxe_or_null();
    if (!ctx) {
        SKIP("No usable RDMA device (rxe not loaded / uverbs blocked)");
    }

    constexpr uint8_t kPort = 1;
    uint16_t lid = 0;
    ibv_gid gid{};
    if (!query_port_(ctx, kPort, &lid, &gid)) {
        ::ibv_close_device(ctx);
        SKIP("ibv_query_port / ibv_query_gid failed; rxe not usable here");
    }

    ibv_pd* pd = ::ibv_alloc_pd(ctx);
    REQUIRE(pd != nullptr);

    ibv_cq* cq_a = ::ibv_create_cq(ctx, 16, nullptr, nullptr, 0);
    ibv_cq* cq_b = ::ibv_create_cq(ctx, 16, nullptr, nullptr, 0);
    REQUIRE(cq_a != nullptr);
    REQUIRE(cq_b != nullptr);

    auto make_qp = [&](ibv_cq* cq) {
        ibv_qp_init_attr attr{};
        attr.send_cq = cq;
        attr.recv_cq = cq;
        attr.qp_type = IBV_QPT_RC;
        attr.cap.max_send_wr  = 16;
        attr.cap.max_recv_wr  = 16;
        attr.cap.max_send_sge = 1;
        attr.cap.max_recv_sge = 1;
        return ::ibv_create_qp(pd, &attr);
    };
    ibv_qp* qp_a = make_qp(cq_a);
    ibv_qp* qp_b = make_qp(cq_b);
    REQUIRE(qp_a != nullptr);
    REQUIRE(qp_b != nullptr);

    REQUIRE(modify_to_init_(qp_a, kPort));
    REQUIRE(modify_to_init_(qp_b, kPort));

    qp_attrs attr_a{qp_a->qp_num, lid, gid};
    qp_attrs attr_b{qp_b->qp_num, lid, gid};
    REQUIRE(modify_to_rtr_(qp_a, attr_b, kPort));
    REQUIRE(modify_to_rtr_(qp_b, attr_a, kPort));
    REQUIRE(modify_to_rts_(qp_a));
    REQUIRE(modify_to_rts_(qp_b));

    // Buffers + MRs.
    std::vector<char> tx_buf(64, 0);
    std::vector<char> rx_buf(64, 0);
    std::memcpy(tx_buf.data(), "hello from rxe", 14);
    memory_region<ibverbs_backend> tx_mr{
        pd, tx_buf.data(), tx_buf.size(),
        IBV_ACCESS_LOCAL_WRITE};
    memory_region<ibverbs_backend> rx_mr{
        pd, rx_buf.data(), rx_buf.size(),
        IBV_ACCESS_LOCAL_WRITE};
    REQUIRE(tx_mr.ok());
    REQUIRE(rx_mr.ok());

    dispatcher disp_a;
    dispatcher disp_b;

    // Poll-and-deliver thread. Real users would use cq_pump bound to
    // ibv_comp_channel, but for the integration test we just run a
    // tight poll loop in its own std::thread.
    std::atomic<bool> stop_pump{false};
    std::thread pump([&]() {
        while (!stop_pump.load()) {
            ibv_wc wc{};
            int n = ::ibv_poll_cq(cq_a, 1, &wc);
            if (n > 0) {
                disp_a.deliver(wc.wr_id, translate_status(wc.status),
                               wc.byte_len, wc.imm_data, 0);
            }
            n = ::ibv_poll_cq(cq_b, 1, &wc);
            if (n > 0) {
                disp_b.deliver(wc.wr_id, translate_status(wc.status),
                               wc.byte_len, wc.imm_data, 0);
            }
            std::this_thread::sleep_for(std::chrono::microseconds(50));
        }
    });

    scheduler sched(2);
    sched.start();

    connection<ibverbs_backend> conn_a{qp_a, disp_a};
    connection<ibverbs_backend> conn_b{qp_b, disp_b};

    std::atomic<int> done{0};
    std::atomic<uint32_t> recv_bytes{0};

    // Post recv first on B; then send from A. RC QPs require a posted
    // recv on the peer before the send retries indefinitely.
    sched.go([&]() -> task<void> {
        auto wc = co_await conn_b.recv(rx_mr.view());
        if (wc.ok()) recv_bytes.store(wc.byte_len);
        done.fetch_add(1);
    });
    // Tiny pause so the recv WR is posted before the send.
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    sched.go([&]() -> task<void> {
        auto wc = co_await conn_a.send(tx_mr.view(0, 14));
        if (wc.ok()) done.fetch_add(1);
    });

    auto deadline = std::chrono::steady_clock::now()
                  + std::chrono::seconds(5);
    while (done.load() < 2
           && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    stop_pump.store(true);
    pump.join();
    sched.shutdown(std::chrono::seconds(5));

    REQUIRE(done.load() == 2);
    REQUIRE(recv_bytes.load() == 14u);
    REQUIRE(std::string(rx_buf.data(), 14) == "hello from rxe");

    // Tear down.
    ::ibv_destroy_qp(qp_a);
    ::ibv_destroy_qp(qp_b);
    ::ibv_destroy_cq(cq_a);
    ::ibv_destroy_cq(cq_b);
    ::ibv_dealloc_pd(pd);
    ::ibv_close_device(ctx);
}
