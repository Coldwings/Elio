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

#include <array>
#include <cerrno>
#include <cstring>
#include <span>

namespace elio::rdma_ibverbs {

/// Static-traits backend over libibverbs. Each post_* helper takes
/// the QP / SRQ pointer the user wrapped into the connection /
/// memory_region / srq object as `void*`. The same cast convention
/// the spec recommends for production user backends.
struct ibverbs_backend {
    static constexpr std::size_t kMaxSge = 16;

    static int post_send(void* qp_ptr,
                         std::span<const elio::rdma::sge> sges,
                         elio::rdma::send_flags flags,
                         std::uint32_t imm_data,
                         elio::rdma::wr_id id) noexcept {
        auto* qp = static_cast<ibv_qp*>(qp_ptr);
        auto converted = convert_sges_(sges);
        if (converted.rc != 0) {
            return converted.rc;
        }
        ibv_send_wr wr{};
        wr.wr_id      = id;
        wr.sg_list    = converted.count > 0 ? converted.data.data() : nullptr;
        wr.num_sge    = static_cast<int>(converted.count);
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
        auto converted = convert_sges_(sges);
        if (converted.rc != 0) {
            return converted.rc;
        }
        ibv_recv_wr wr{};
        wr.wr_id   = id;
        wr.sg_list = converted.count > 0 ? converted.data.data() : nullptr;
        wr.num_sge = static_cast<int>(converted.count);
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
        auto converted = convert_sges_(sges);
        if (converted.rc != 0) {
            return converted.rc;
        }
        ibv_send_wr wr{};
        wr.wr_id              = id;
        wr.sg_list            = converted.count > 0 ? converted.data.data() : nullptr;
        wr.num_sge            = static_cast<int>(converted.count);
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
        auto converted = convert_sges_(sges);
        if (converted.rc != 0) {
            return converted.rc;
        }
        ibv_send_wr wr{};
        wr.wr_id               = id;
        wr.sg_list             = converted.count > 0 ? converted.data.data() : nullptr;
        wr.num_sge             = static_cast<int>(converted.count);
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

    static int post_atomic_cas(void* qp_ptr,
                               std::span<const elio::rdma::sge> sges,
                               elio::rdma::remote_buffer rb,
                               std::uint64_t compare,
                               std::uint64_t swap,
                               elio::rdma::send_flags flags,
                               elio::rdma::wr_id id) noexcept {
        auto* qp = static_cast<ibv_qp*>(qp_ptr);
        auto converted = convert_sges_(sges);
        if (converted.rc != 0) {
            return converted.rc;
        }
        ibv_send_wr wr{};
        wr.wr_id              = id;
        wr.sg_list            = converted.count > 0 ? converted.data.data() : nullptr;
        wr.num_sge            = static_cast<int>(converted.count);
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
        auto converted = convert_sges_(sges);
        if (converted.rc != 0) {
            return converted.rc;
        }
        ibv_send_wr wr{};
        wr.wr_id              = id;
        wr.sg_list            = converted.count > 0 ? converted.data.data() : nullptr;
        wr.num_sge            = static_cast<int>(converted.count);
        wr.opcode             = IBV_WR_ATOMIC_FETCH_AND_ADD;
        wr.send_flags         = make_send_flags_(flags);
        wr.wr.atomic.remote_addr = rb.addr;
        wr.wr.atomic.rkey        = rb.rkey;
        wr.wr.atomic.compare_add = add;
        ibv_send_wr* bad = nullptr;
        const int rc = ::ibv_post_send(qp, &wr, &bad);
        return rc == 0 ? 0 : -rc;
    }

private:
    struct sge_buf {
        std::array<ibv_sge, kMaxSge> data{};
        std::size_t count = 0;
        int rc = 0;
    };
    static sge_buf convert_sges_(std::span<const elio::rdma::sge> sges) noexcept {
        sge_buf buf;
        if (sges.size() > kMaxSge) {
            buf.rc = -EMSGSIZE;
            return buf;
        }
        buf.count = sges.size();
        for (std::size_t i = 0; i < buf.count; ++i) {
            buf.data[i] = ibv_sge{
                .addr   = reinterpret_cast<std::uint64_t>(sges[i].addr),
                .length = sges[i].length,
                .lkey   = sges[i].lkey,
            };
        }
        return buf;
    }

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
