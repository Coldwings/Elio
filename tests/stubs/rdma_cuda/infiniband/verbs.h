#pragma once

#include <cstddef>
#include <cstdint>

struct ibv_pd {};
struct ibv_qp {};
struct ibv_srq {};
struct ibv_cq {};
struct ibv_comp_channel {};

enum ibv_wc_status {
    IBV_WC_SUCCESS,
    IBV_WC_LOC_LEN_ERR,
    IBV_WC_LOC_QP_OP_ERR,
    IBV_WC_LOC_EEC_OP_ERR,
    IBV_WC_LOC_PROT_ERR,
    IBV_WC_WR_FLUSH_ERR,
    IBV_WC_MW_BIND_ERR,
    IBV_WC_BAD_RESP_ERR,
    IBV_WC_LOC_ACCESS_ERR,
    IBV_WC_REM_INV_REQ_ERR,
    IBV_WC_REM_ACCESS_ERR,
    IBV_WC_REM_OP_ERR,
    IBV_WC_RETRY_EXC_ERR,
    IBV_WC_RNR_RETRY_EXC_ERR,
    IBV_WC_LOC_RDD_VIOL_ERR,
    IBV_WC_REM_INV_RD_REQ_ERR,
    IBV_WC_REM_ABORT_ERR,
    IBV_WC_INV_EECN_ERR,
    IBV_WC_INV_EEC_STATE_ERR,
    IBV_WC_FATAL_ERR,
    IBV_WC_RESP_TIMEOUT_ERR,
};

struct ibv_wc {
    std::uint64_t wr_id = 0;
    ibv_wc_status status = IBV_WC_SUCCESS;
    std::uint32_t byte_len = 0;
    std::uint32_t imm_data = 0;
    std::uint32_t wc_flags = 0;
};

struct ibv_mr {
    void*         addr = nullptr;
    std::size_t   length = 0;
    std::uint32_t lkey = 0;
    std::uint32_t rkey = 0;
};

struct ibv_sge {
    std::uint64_t addr = 0;
    std::uint32_t length = 0;
    std::uint32_t lkey = 0;
};

struct ibv_send_wr {
    std::uint64_t wr_id = 0;
    ibv_sge*      sg_list = nullptr;
    int           num_sge = 0;
    int           opcode = 0;
    std::uint32_t imm_data = 0;
    unsigned int  send_flags = 0;

    union {
        struct {
            std::uint64_t remote_addr;
            std::uint32_t rkey;
        } rdma;
        struct {
            std::uint64_t remote_addr;
            std::uint32_t rkey;
            std::uint64_t compare_add;
            std::uint64_t swap;
        } atomic;
    } wr{};
};

struct ibv_recv_wr {
    std::uint64_t wr_id = 0;
    ibv_sge*      sg_list = nullptr;
    int           num_sge = 0;
};

inline constexpr int IBV_WR_SEND = 0;
inline constexpr int IBV_WR_SEND_WITH_IMM = 1;
inline constexpr int IBV_WR_RDMA_WRITE = 2;
inline constexpr int IBV_WR_RDMA_WRITE_WITH_IMM = 3;
inline constexpr int IBV_WR_RDMA_READ = 4;
inline constexpr int IBV_WR_ATOMIC_CMP_AND_SWP = 5;
inline constexpr int IBV_WR_ATOMIC_FETCH_AND_ADD = 6;

inline constexpr unsigned int IBV_SEND_SIGNALED = 1u << 0;
inline constexpr unsigned int IBV_SEND_SOLICITED = 1u << 1;
inline constexpr unsigned int IBV_SEND_INLINE = 1u << 2;
inline constexpr unsigned int IBV_SEND_FENCE = 1u << 3;
inline constexpr std::uint32_t IBV_WC_WITH_IMM = 1u << 1;

extern "C" ibv_mr* elio_rdma_cuda_test_ibv_reg_mr(
    ibv_pd* pd, void* addr, std::size_t length, int access);
extern "C" int elio_rdma_cuda_test_ibv_dereg_mr(ibv_mr* mr);

inline ibv_mr* ibv_reg_mr(ibv_pd* pd, void* addr, std::size_t length,
                          int access) {
    return elio_rdma_cuda_test_ibv_reg_mr(pd, addr, length, access);
}

inline int ibv_dereg_mr(ibv_mr* mr) {
    return elio_rdma_cuda_test_ibv_dereg_mr(mr);
}

inline int ibv_post_send(ibv_qp*, ibv_send_wr*, ibv_send_wr**) {
    return 0;
}

inline int ibv_post_recv(ibv_qp*, ibv_recv_wr*, ibv_recv_wr**) {
    return 0;
}

inline int ibv_post_srq_recv(ibv_srq*, ibv_recv_wr*, ibv_recv_wr**) {
    return 0;
}

inline int ibv_get_cq_event(ibv_comp_channel*, ibv_cq**, void**) {
    return -1;
}

inline void ibv_ack_cq_events(ibv_cq*, unsigned int) {}

inline int ibv_req_notify_cq(ibv_cq*, int) {
    return 0;
}

inline int ibv_poll_cq(ibv_cq*, int, ibv_wc*) {
    return 0;
}
