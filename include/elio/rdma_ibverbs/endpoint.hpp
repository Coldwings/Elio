#pragma once

/// @file endpoint.hpp
/// @brief High-level RDMA endpoint wrapping pd / cq / comp_channel /
///        qp / dispatcher / cq_pump as a single RAII object.
///
/// Requires the librdmacm helper (`elio_rdma_cm`) for CM bootstrap
/// — only available when both ELIO_HAS_RDMA_IBVERBS and
/// ELIO_HAS_RDMA_CM are defined.
///
/// `endpoint` lets users stand up a complete RDMA data path without
/// touching libibverbs or librdmacm directly. The bare-minimum
/// usage:
///
/// @code{.cpp}
/// // Client (uses elio_rdma_cm to drive resolve+connect)
/// elio::rdma_cm::event_channel cm_ch;
/// auto ep = co_await elio::rdma_ibverbs::connect(
///     cm_ch, dst_addr, sizeof(*dst_addr),
///     elio::rdma_ibverbs::endpoint_config{.max_recv_wr = 8});
/// ep.start_cq_pump(sched);
///
/// auto mr = ep.register_buffer(buf, len, IBV_ACCESS_LOCAL_WRITE);
/// auto wc = co_await ep.conn().send(mr.view());
/// @endcode
///
/// `endpoint` is NOT for users who need fine-grained control over PD
/// allocation, CQ sharing, or QP attributes — they should stay with
/// the low-level `elio::rdma::connection<Backend>` surface. Both
/// styles coexist; mixing within a process is fine.
///
/// **Shutdown contract**: the destructor sets the cq_pump cancel
/// token AND destroys the QP first (which generates flush CQEs that
/// wake the pump). It then waits up to ~1s for the pump to observe
/// the cancel and exit before tearing down the CQ / comp_channel /
/// PD. If the pump doesn't exit in time (e.g. a custom drain that
/// blocks indefinitely), the verbs resources are intentionally
/// leaked rather than risk a use-after-free on the polling thread.

#if !defined(ELIO_HAS_RDMA_CM) || !ELIO_HAS_RDMA_CM
// endpoint depends on elio_rdma_cm for the CM bootstrap step. Build
// with -DELIO_ENABLE_RDMA_CM=ON to enable it; without that this
// header is a no-op so users who only need the backend / cq_drain
// helpers can still include the umbrella.
namespace elio::rdma_ibverbs { /* endpoint API not available */ }
#else

#include <elio/coro/cancel_token.hpp>
#include <elio/coro/task.hpp>
#include <elio/rdma/cq_pump.hpp>
#include <elio/rdma/connection.hpp>
#include <elio/rdma/memory_region.hpp>
#include <elio/rdma_cm/cm_connect.hpp>
#include <elio/rdma_cm/cm_id.hpp>
#include <elio/rdma_cm/cm_listener.hpp>
#include <elio/rdma_cm/event_channel.hpp>
#include <elio/rdma_ibverbs/cq_drain.hpp>
#include <elio/rdma_ibverbs/ibverbs_backend.hpp>
#include <elio/runtime/scheduler.hpp>

#include <infiniband/verbs.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>

#if defined(ELIO_HAS_RDMA_CUDA) && ELIO_HAS_RDMA_CUDA
#include <elio/rdma_cuda/gpu_memory_region.hpp>
#endif

namespace elio::rdma_ibverbs {

/// Per-endpoint creation knobs. Most users only touch the cap fields.
/// To go fully custom, set `custom_qp_init_attr` to a pre-populated
/// pointer and the cap / qp_type fields are ignored.
struct endpoint_config {
    std::uint32_t max_send_wr      = 16;
    std::uint32_t max_recv_wr      = 16;
    std::uint32_t max_send_sge     = 1;
    std::uint32_t max_recv_sge     = 1;
    std::uint32_t max_inline_data  = 0;
    ibv_qp_type   qp_type          = IBV_QPT_RC;
    std::uint32_t cq_size          = 64;

    /// If non-null, the wrapper passes this struct to rdma_create_qp
    /// verbatim, ignoring the cap / qp_type fields above. The user is
    /// responsible for setting send_cq / recv_cq fields to point at
    /// the endpoint's own CQ (use the `ibv_cq* cq` argument the
    /// wrapper will populate before consulting this struct — see
    /// `with_cqs` helper below).
    ibv_qp_init_attr* custom_qp_init_attr = nullptr;
};

class endpoint {
public:
    /// Take ownership of an already-resolved cm_id and build the rest
    /// of the verbs stack on top of it. The cm_id MUST have an
    /// associated verbs context (i.e. the resolve phase has completed
    /// — id.verbs() != nullptr).
    ///
    /// Throws `std::runtime_error` if any verbs setup step fails;
    /// partial resources are cleaned up before the exception
    /// propagates.
    endpoint(rdma_cm::cm_id id, endpoint_config cfg = {})
        : id_(std::move(id)), config_(cfg),
          disp_(std::make_unique<elio::rdma::dispatcher>()) {
        if (!id_) {
            throw std::runtime_error("endpoint: cm_id is null");
        }
        if (!id_.verbs()) {
            throw std::runtime_error(
                "endpoint: cm_id has no verbs context "
                "(resolve_addr not yet completed)");
        }
        try {
            build_resources_();
        } catch (...) {
            destroy_resources_();
            throw;
        }
        conn_ = std::make_unique<elio::rdma::connection<ibverbs_backend>>(
            qp_, *disp_,
            elio::rdma::connection_config{.max_inline_data = cfg.max_inline_data});
    }

    endpoint(const endpoint&) = delete;
    endpoint& operator=(const endpoint&) = delete;

    // Movable: every owned object that needs a stable address is
    // behind a unique_ptr (dispatcher, connection, pump_state) or a
    // shared_ptr (cancel_source state). Raw pointers are nulled in
    // the source so its destructor doesn't double-free.
    endpoint(endpoint&& other) noexcept
        : id_(std::move(other.id_)),
          config_(other.config_),
          pd_(std::exchange(other.pd_, nullptr)),
          comp_ch_(std::exchange(other.comp_ch_, nullptr)),
          cq_(std::exchange(other.cq_, nullptr)),
          qp_(std::exchange(other.qp_, nullptr)),
          disp_(std::move(other.disp_)),
          conn_(std::move(other.conn_)),
          pump_cancel_(std::move(other.pump_cancel_)),
          pump_state_(std::move(other.pump_state_)) {}

    endpoint& operator=(endpoint&& other) noexcept {
        if (this != &other) {
            endpoint tmp(std::move(*this));
            id_          = std::move(other.id_);
            config_      = other.config_;
            pd_          = std::exchange(other.pd_, nullptr);
            comp_ch_     = std::exchange(other.comp_ch_, nullptr);
            cq_          = std::exchange(other.cq_, nullptr);
            qp_          = std::exchange(other.qp_, nullptr);
            disp_        = std::move(other.disp_);
            conn_        = std::move(other.conn_);
            pump_cancel_ = std::move(other.pump_cancel_);
            pump_state_  = std::move(other.pump_state_);
        }
        return *this;
    }

    ~endpoint() {
        if (pump_state_) {
            pump_cancel_.cancel();
        }
        // Destroying QP first generates flush CQEs that wake the pump
        // and unblock its async_poll_read. Use rdma_destroy_qp (not
        // ibv_destroy_qp) so the cm_id's internal qp pointer is nulled;
        // otherwise rdma_destroy_id would double-free it.
        if (qp_) {
            ::rdma_destroy_qp(id_.native());
            qp_ = nullptr;
        }
        // Wait briefly for the pump to observe the cancel and exit.
        // If it doesn't (custom drain that blocks indefinitely, or no
        // flush CQEs because the QP had no posted WRs), we leak the
        // remaining verbs resources AND the dispatcher rather than
        // risk a UAF when ibv_destroy_cq or ~dispatcher runs while the
        // pump coroutine is still referencing them.
        bool pump_clean = true;
        if (pump_state_) {
            auto deadline = std::chrono::steady_clock::now()
                          + std::chrono::milliseconds(1000);
            while (!pump_state_->done.load(std::memory_order_acquire)
                   && std::chrono::steady_clock::now() < deadline) {
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
            pump_clean = pump_state_->done.load(std::memory_order_acquire);
        }
        if (pump_clean) {
            if (cq_) ::ibv_destroy_cq(cq_);
            if (comp_ch_) ::ibv_destroy_comp_channel(comp_ch_);
            if (pd_) ::ibv_dealloc_pd(pd_);
        } else {
            (void)disp_.release();
        }
        // cm_id RAII handles itself.
    }

    /// Returns the data-path connection. Lifetime is tied to the
    /// endpoint; do not move out.
    [[nodiscard]] elio::rdma::connection<ibverbs_backend>& conn() noexcept {
        return *conn_;
    }

    /// Register a buffer with this endpoint's PD. The returned MR is
    /// move-only and auto-deregisters at scope exit.
    [[nodiscard]] elio::rdma::memory_region<ibverbs_backend>
    register_buffer(void* addr, std::size_t length, int access) {
        return elio::rdma::memory_region<ibverbs_backend>{
            pd_, addr, length, access};
    }

    /// Atomic CAS / FAA shortcuts that forward to the underlying
    /// connection (S15). The local buffer must be 8 bytes and the
    /// remote address must be 8-byte aligned.
    [[nodiscard]] auto cas(elio::rdma::buffer_view local,
                           elio::rdma::remote_buffer remote,
                           std::uint64_t compare, std::uint64_t swap,
                           elio::rdma::send_flags flags = {}) noexcept {
        return conn_->cas(local, remote, compare, swap, flags);
    }
    [[nodiscard]] auto fetch_add(elio::rdma::buffer_view local,
                                 elio::rdma::remote_buffer remote,
                                 std::uint64_t add,
                                 elio::rdma::send_flags flags = {}) noexcept {
        return conn_->fetch_add(local, remote, add, flags);
    }

    /// Spawn the cq_pump on the given scheduler. Idempotent: a
    /// second call is a no-op.
    void start_cq_pump(elio::runtime::scheduler& sched) {
        if (pump_state_) return;
        pump_state_ = std::make_shared<pump_state>();
        auto state = pump_state_;
        auto token = pump_cancel_.get_token();
        auto cq = cq_;
        auto comp_ch = comp_ch_;
        auto* disp = disp_.get();
        sched.go([state, token, cq, comp_ch, disp]() -> elio::coro::task<void> {
            co_await elio::rdma::cq_pump(comp_ch->fd, *disp,
                                         make_cq_drain(cq, comp_ch),
                                         token);
            state->done.store(true, std::memory_order_release);
        });
    }

    /// Cancel the cq_pump's loop. Does not wait — the pump exits at
    /// its next iteration. For a synchronous join, rely on the
    /// destructor's bounded wait OR ensure outstanding WRs flush
    /// before this call so a CQE wakes the pump.
    void stop_cq_pump() noexcept {
        if (pump_state_) pump_cancel_.cancel();
    }

#if defined(ELIO_HAS_RDMA_CUDA) && ELIO_HAS_RDMA_CUDA
    [[nodiscard]] elio::rdma_cuda::gpu_memory_region
    register_gpu_buffer(std::size_t size, int access) {
        return elio::rdma_cuda::gpu_memory_region{pd_, size, access};
    }
#endif

    /// Raw resource accessors for users who need to dip into the
    /// underlying ibverbs / rdma_cm surface.
    [[nodiscard]] ibv_pd*           pd()       const noexcept { return pd_; }
    [[nodiscard]] ibv_cq*           cq()       const noexcept { return cq_; }
    [[nodiscard]] ibv_qp*           qp()       const noexcept { return qp_; }
    [[nodiscard]] ibv_comp_channel* comp_ch()  const noexcept { return comp_ch_; }
    [[nodiscard]] rdma_cm::cm_id&   cm_id()    noexcept       { return id_; }
    [[nodiscard]] elio::rdma::dispatcher& dispatcher_ref() noexcept {
        return *disp_;
    }
    [[nodiscard]] const endpoint_config& config() const noexcept {
        return config_;
    }

private:
    struct pump_state {
        std::atomic<bool> done{false};
    };

    void build_resources_() {
        pd_ = ::ibv_alloc_pd(id_.verbs());
        if (!pd_) throw_verbs_("ibv_alloc_pd");
        comp_ch_ = ::ibv_create_comp_channel(id_.verbs());
        if (!comp_ch_) throw_verbs_("ibv_create_comp_channel");
        cq_ = ::ibv_create_cq(id_.verbs(),
                              static_cast<int>(config_.cq_size),
                              /*ctx=*/nullptr, comp_ch_, /*comp_vec=*/0);
        if (!cq_) throw_verbs_("ibv_create_cq");
        if (::ibv_req_notify_cq(cq_, 0) != 0) {
            throw_verbs_("ibv_req_notify_cq");
        }

        ibv_qp_init_attr fallback{};
        ibv_qp_init_attr* attr = config_.custom_qp_init_attr;
        if (!attr) {
            fallback.send_cq          = cq_;
            fallback.recv_cq          = cq_;
            fallback.qp_type          = config_.qp_type;
            fallback.cap.max_send_wr  = config_.max_send_wr;
            fallback.cap.max_recv_wr  = config_.max_recv_wr;
            fallback.cap.max_send_sge = config_.max_send_sge;
            fallback.cap.max_recv_sge = config_.max_recv_sge;
            fallback.cap.max_inline_data = config_.max_inline_data;
            attr = &fallback;
        } else {
            // If the user forgot to wire the CQs, fix it up. Better
            // to be quietly helpful than to fail with a confusing
            // EINVAL from rdma_create_qp.
            if (!attr->send_cq) attr->send_cq = cq_;
            if (!attr->recv_cq) attr->recv_cq = cq_;
        }
        if (::rdma_create_qp(id_.native(), pd_, attr) != 0) {
            throw_verbs_("rdma_create_qp");
        }
        qp_ = id_.native()->qp;
    }

    void destroy_resources_() noexcept {
        if (qp_) {
            ::rdma_destroy_qp(id_.native());
            qp_ = nullptr;
        }
        if (cq_)      { ::ibv_destroy_cq(cq_); cq_ = nullptr; }
        if (comp_ch_) { ::ibv_destroy_comp_channel(comp_ch_); comp_ch_ = nullptr; }
        if (pd_)      { ::ibv_dealloc_pd(pd_); pd_ = nullptr; }
    }

    [[noreturn]] void throw_verbs_(const char* op) {
        std::string msg(op);
        msg += " failed: ";
        msg += std::strerror(errno);
        throw std::runtime_error(msg);
    }

    rdma_cm::cm_id                                       id_;
    endpoint_config                                      config_{};
    ibv_pd*                                              pd_       = nullptr;
    ibv_comp_channel*                                    comp_ch_  = nullptr;
    ibv_cq*                                              cq_       = nullptr;
    ibv_qp*                                              qp_       = nullptr;
    std::unique_ptr<elio::rdma::dispatcher>              disp_;
    std::unique_ptr<elio::rdma::connection<ibverbs_backend>> conn_;
    elio::coro::cancel_source                            pump_cancel_;
    std::shared_ptr<pump_state>                          pump_state_;
};

// ---------------------------------------------------------------------
// Free-function bootstrap helpers. Each wraps the corresponding
// elio_rdma_cm step and hands back a fully-constructed endpoint.
// ---------------------------------------------------------------------

/// Client-side one-liner: resolve + create QP + connect. Returns an
/// endpoint on success; throws `std::runtime_error` on failure with
/// the underlying cm_status written to `out_status` if supplied
/// (nullptr OK).
[[nodiscard]] inline elio::coro::task<endpoint>
connect(rdma_cm::event_channel& ch,
        const struct sockaddr* dst,
        socklen_t dst_len,
        endpoint_config cfg = {},
        rdma_cm::connect_options opts = {},
        rdma_cm::cm_status* out_status = nullptr,
        elio::coro::cancel_token token = {}) {
    rdma_cm::cm_status st{};
    auto id = co_await rdma_cm::resolve(ch, dst, dst_len, opts, &st, token);
    if (!st.ok() || !id) {
        if (out_status) *out_status = st;
        throw std::runtime_error(
            std::string("rdma_ibverbs::connect: resolve failed (status=")
            + std::to_string(st.status) + ")");
    }
    endpoint ep{std::move(id), cfg};
    auto cst = co_await rdma_cm::complete_connect(ch, ep.cm_id(),
                                                  nullptr, token);
    if (!cst.ok()) {
        if (out_status) *out_status = cst;
        throw std::runtime_error(
            std::string("rdma_ibverbs::connect: complete_connect failed (status=")
            + std::to_string(cst.status) + ")");
    }
    if (out_status) out_status->status = 0;
    co_return ep;
}

/// Server-side acceptor. Bind + listen on construction; each
/// `accept()` call returns one fully-handshaked endpoint.
class acceptor {
public:
    acceptor(rdma_cm::event_channel& ch,
             const struct sockaddr* bind_addr,
             socklen_t bind_len,
             int backlog = 128,
             rdma_port_space port_space = RDMA_PS_TCP)
        : ch_(&ch),
          listener_(ch, bind_addr, bind_len, backlog, port_space) {}

    acceptor(const acceptor&) = delete;
    acceptor& operator=(const acceptor&) = delete;

    [[nodiscard]] elio::coro::task<endpoint>
    accept(endpoint_config cfg = {},
           rdma_cm::cm_status* out_status = nullptr,
           elio::coro::cancel_token token = {}) {
        rdma_cm::cm_status st{};
        auto id = co_await listener_.accept(&st, token);
        if (!id) {
            if (out_status) *out_status = st;
            throw std::runtime_error(
                std::string("rdma_ibverbs::acceptor: cm accept failed: ")
                + std::to_string(st.status));
        }
        endpoint ep{std::move(id), cfg};
        auto cst = co_await rdma_cm::accept_connect(*ch_, ep.cm_id(),
                                                    nullptr, token);
        if (!cst.ok()) {
            if (out_status) *out_status = cst;
            throw std::runtime_error(
                std::string("rdma_ibverbs::acceptor: accept_connect failed: ")
                + std::to_string(cst.status));
        }
        if (out_status) out_status->status = 0;
        co_return ep;
    }

private:
    rdma_cm::event_channel* ch_;
    rdma_cm::cm_listener    listener_;
};

}  // namespace elio::rdma_ibverbs

#endif  // ELIO_HAS_RDMA_CM
