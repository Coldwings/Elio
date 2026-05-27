#pragma once

/// @file ibverbs_backend.hpp
/// @brief Thin static-traits backend that forwards to libibverbs.
///        Used only by the optional rxe integration test (S10).
///
/// This is NOT shipped as part of the public Elio API — the
/// abstraction's whole point is that the user supplies their own
/// backend. This file exists so the integration test can exercise a
/// real verbs stack end-to-end with a known-good implementation.

#include <elio/rdma/types.hpp>

#include <infiniband/verbs.h>

#include <endian.h>

#include <cerrno>
#include <cstring>
#include <span>
#include <vector>

namespace elio_rdma_test {

/// Static-traits backend over libibverbs. Each post_* helper takes
/// the QP / SRQ pointer the user wrapped into the connection /
/// memory_region / srq object as `void*`. The same cast convention
/// the spec recommends for production user backends.
struct ibverbs_backend {
    static int post_send(void* qp_ptr,
                         std::span<const elio::rdma::sge> sges,
                         elio::rdma::send_flags flags,
                         std::uint32_t imm_data,
                         elio::rdma::wr_id id) noexcept {
        auto* qp = static_cast<ibv_qp*>(qp_ptr);
        std::vector<ibv_sge> raw;
        raw.reserve(sges.size());
        for (const auto& s : sges) {
            raw.push_back(ibv_sge{
                .addr   = reinterpret_cast<std::uint64_t>(s.addr),
                .length = s.length,
                .lkey   = s.lkey,
            });
        }
        ibv_send_wr wr{};
        wr.wr_id      = id;
        wr.sg_list    = raw.empty() ? nullptr : raw.data();
        wr.num_sge    = static_cast<int>(raw.size());
        wr.opcode     = flags.with_imm ? IBV_WR_SEND_WITH_IMM : IBV_WR_SEND;
        if (flags.with_imm) {
            wr.imm_data = ::htobe32(imm_data);
        }
        wr.send_flags = make_send_flags_(flags);
        ibv_send_wr* bad = nullptr;
        const int rc = ::ibv_post_send(qp, &wr, &bad);
        return rc == 0 ? 0 : -rc;
    }

    static int post_recv(void* qp_ptr,
                         std::span<const elio::rdma::sge> sges,
                         elio::rdma::wr_id id) noexcept {
        auto* qp = static_cast<ibv_qp*>(qp_ptr);
        std::vector<ibv_sge> raw;
        raw.reserve(sges.size());
        for (const auto& s : sges) {
            raw.push_back(ibv_sge{
                .addr   = reinterpret_cast<std::uint64_t>(s.addr),
                .length = s.length,
                .lkey   = s.lkey,
            });
        }
        ibv_recv_wr wr{};
        wr.wr_id   = id;
        wr.sg_list = raw.empty() ? nullptr : raw.data();
        wr.num_sge = static_cast<int>(raw.size());
        ibv_recv_wr* bad = nullptr;
        const int rc = ::ibv_post_recv(qp, &wr, &bad);
        return rc == 0 ? 0 : -rc;
    }

    static int post_rdma_write(void* qp_ptr,
                               std::span<const elio::rdma::sge> sges,
                               elio::rdma::remote_buffer rb,
                               elio::rdma::send_flags flags,
                               std::uint32_t imm_data,
                               elio::rdma::wr_id id) noexcept {
        auto* qp = static_cast<ibv_qp*>(qp_ptr);
        std::vector<ibv_sge> raw;
        raw.reserve(sges.size());
        for (const auto& s : sges) {
            raw.push_back(ibv_sge{
                .addr   = reinterpret_cast<std::uint64_t>(s.addr),
                .length = s.length,
                .lkey   = s.lkey,
            });
        }
        ibv_send_wr wr{};
        wr.wr_id              = id;
        wr.sg_list            = raw.empty() ? nullptr : raw.data();
        wr.num_sge            = static_cast<int>(raw.size());
        wr.opcode             = flags.with_imm
            ? IBV_WR_RDMA_WRITE_WITH_IMM : IBV_WR_RDMA_WRITE;
        if (flags.with_imm) {
            wr.imm_data = ::htobe32(imm_data);
        }
        wr.send_flags         = make_send_flags_(flags);
        wr.wr.rdma.remote_addr = rb.addr;
        wr.wr.rdma.rkey        = rb.rkey;
        ibv_send_wr* bad = nullptr;
        const int rc = ::ibv_post_send(qp, &wr, &bad);
        return rc == 0 ? 0 : -rc;
    }

    static int post_rdma_read(void* qp_ptr,
                              std::span<const elio::rdma::sge> sges,
                              elio::rdma::remote_buffer rb,
                              elio::rdma::wr_id id) noexcept {
        auto* qp = static_cast<ibv_qp*>(qp_ptr);
        std::vector<ibv_sge> raw;
        raw.reserve(sges.size());
        for (const auto& s : sges) {
            raw.push_back(ibv_sge{
                .addr   = reinterpret_cast<std::uint64_t>(s.addr),
                .length = s.length,
                .lkey   = s.lkey,
            });
        }
        ibv_send_wr wr{};
        wr.wr_id               = id;
        wr.sg_list             = raw.empty() ? nullptr : raw.data();
        wr.num_sge             = static_cast<int>(raw.size());
        wr.opcode              = IBV_WR_RDMA_READ;
        wr.send_flags          = IBV_SEND_SIGNALED;
        wr.wr.rdma.remote_addr = rb.addr;
        wr.wr.rdma.rkey        = rb.rkey;
        ibv_send_wr* bad = nullptr;
        const int rc = ::ibv_post_send(qp, &wr, &bad);
        return rc == 0 ? 0 : -rc;
    }

    static void* register_mr(void* pd_ptr, void* addr, std::size_t length,
                             int access) noexcept {
        auto* pd = static_cast<ibv_pd*>(pd_ptr);
        return ::ibv_reg_mr(pd, addr, length, access);
    }
    static void dereg_mr(void* mr_ptr) noexcept {
        if (mr_ptr) {
            (void)::ibv_dereg_mr(static_cast<ibv_mr*>(mr_ptr));
        }
    }
    static std::uint32_t lkey_of(void* mr_ptr) noexcept {
        return mr_ptr ? static_cast<ibv_mr*>(mr_ptr)->lkey : 0;
    }
    static std::uint32_t rkey_of(void* mr_ptr) noexcept {
        return mr_ptr ? static_cast<ibv_mr*>(mr_ptr)->rkey : 0;
    }

private:
    static unsigned int make_send_flags_(elio::rdma::send_flags f) noexcept {
        unsigned int out = 0;
        if (f.signaled)    out |= IBV_SEND_SIGNALED;
        if (f.solicited)   out |= IBV_SEND_SOLICITED;
        if (f.inline_send) out |= IBV_SEND_INLINE;
        if (f.fence)       out |= IBV_SEND_FENCE;
        return out;
    }
};

/// Map an `ibv_wc_status` onto Elio's normalised `wc_status`. The
/// integration test uses this to translate the raw CQE into a
/// `dispatcher::deliver(...)` call.
inline elio::rdma::wc_status translate_status(ibv_wc_status s) noexcept {
    using S = elio::rdma::wc_status;
    switch (s) {
        case IBV_WC_SUCCESS:               return S::success;
        case IBV_WC_LOC_LEN_ERR:           return S::local_length_error;
        case IBV_WC_LOC_QP_OP_ERR:         return S::local_qp_error;
        case IBV_WC_LOC_EEC_OP_ERR:        return S::local_eec_error;
        case IBV_WC_LOC_PROT_ERR:          return S::local_protection_error;
        case IBV_WC_WR_FLUSH_ERR:          return S::wr_flush_error;
        case IBV_WC_MW_BIND_ERR:           return S::memory_window_bind_error;
        case IBV_WC_BAD_RESP_ERR:          return S::bad_response;
        case IBV_WC_LOC_ACCESS_ERR:        return S::local_access_error;
        case IBV_WC_REM_INV_REQ_ERR:       return S::remote_invalid_request;
        case IBV_WC_REM_ACCESS_ERR:        return S::remote_access_error;
        case IBV_WC_REM_OP_ERR:            return S::remote_op_error;
        case IBV_WC_RETRY_EXC_ERR:         return S::retry_exceeded;
        case IBV_WC_RNR_RETRY_EXC_ERR:     return S::rnr_retry_exceeded;
        case IBV_WC_LOC_RDD_VIOL_ERR:      return S::local_rdd_violation;
        case IBV_WC_REM_INV_RD_REQ_ERR:    return S::remote_invalid_rd_request;
        case IBV_WC_REM_ABORT_ERR:         return S::remote_aborted;
        case IBV_WC_INV_EECN_ERR:          return S::invalid_eecn;
        case IBV_WC_INV_EEC_STATE_ERR:     return S::invalid_eec_state;
        case IBV_WC_FATAL_ERR:             return S::fatal;
        case IBV_WC_RESP_TIMEOUT_ERR:      return S::response_timeout;
        default:                           return S::general;
    }
}

}  // namespace elio_rdma_test
