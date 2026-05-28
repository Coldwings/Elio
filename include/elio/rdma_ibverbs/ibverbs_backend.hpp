#pragma once

/// @file ibverbs_backend.hpp
/// @brief Public reference backend that forwards to libibverbs.
///
/// Lives under the optional `elio_rdma_ibverbs` target. Satisfies
/// `elio::rdma::backend_traits` and `backend_with_mr`; users who
/// don't want to roll their own backend can `using my_backend =
/// elio::rdma_ibverbs::ibverbs_backend;` and be done. Composes
/// naturally with `elio::rdma_ibverbs::endpoint` (S13).

#include <elio/rdma/types.hpp>

#include <infiniband/verbs.h>

#include <endian.h>

#include <cerrno>
#include <cstring>
#include <span>
#include <vector>

namespace elio::rdma_ibverbs {

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

    // ATOMIC (S15). ibverbs delivers the OLD remote value into the
    // local SGE as 8 raw bytes big-endian on the wire; users wanting
    // a host-endian view should go through `atomic_result::
    // old_value_host()` rather than reading the buffer directly.
    static int post_atomic_cas(void* qp_ptr,
                               std::span<const elio::rdma::sge> sges,
                               elio::rdma::remote_buffer rb,
                               std::uint64_t compare,
                               std::uint64_t swap,
                               elio::rdma::send_flags flags,
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
        wr.opcode             = IBV_WR_ATOMIC_CMP_AND_SWP;
        wr.send_flags         = make_send_flags_(flags);
        wr.wr.atomic.remote_addr = rb.addr;
        wr.wr.atomic.rkey        = rb.rkey;
        wr.wr.atomic.compare_add = compare;
        wr.wr.atomic.swap        = swap;
        ibv_send_wr* bad = nullptr;
        const int rc = ::ibv_post_send(qp, &wr, &bad);
        return rc == 0 ? 0 : -rc;
    }
    static int post_atomic_fetch_add(void* qp_ptr,
                                     std::span<const elio::rdma::sge> sges,
                                     elio::rdma::remote_buffer rb,
                                     std::uint64_t add,
                                     elio::rdma::send_flags flags,
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
        wr.opcode             = IBV_WR_ATOMIC_FETCH_AND_ADD;
        wr.send_flags         = make_send_flags_(flags);
        wr.wr.atomic.remote_addr = rb.addr;
        wr.wr.atomic.rkey        = rb.rkey;
        wr.wr.atomic.compare_add = add;   // FAA reuses the compare_add slot
        ibv_send_wr* bad = nullptr;
        const int rc = ::ibv_post_send(qp, &wr, &bad);
        return rc == 0 ? 0 : -rc;
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

// `translate_status` lives in cq_drain.hpp now.

}  // namespace elio::rdma_ibverbs
