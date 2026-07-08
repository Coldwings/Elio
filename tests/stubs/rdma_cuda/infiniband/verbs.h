#pragma once

#include <cstddef>
#include <cstdint>

struct ibv_pd {};
struct ibv_qp {};

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
