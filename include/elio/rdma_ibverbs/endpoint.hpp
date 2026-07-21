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
    /// {
    ///     auto mr = ep.register_buffer(buf, len, IBV_ACCESS_LOCAL_WRITE);
    ///     auto wc = co_await ep.conn().send(mr.view());
    /// } // The completed awaiter and MR are gone before shutdown.
    /// co_await ep.shutdown();
/// @endcode
///
/// `endpoint` is NOT for users who need fine-grained control over PD
/// allocation, CQ sharing, or QP attributes — they should stay with
/// the low-level `elio::rdma::connection<Backend>` surface. Both
/// styles coexist; mixing within a process is fine.
///
/// **Shutdown contract**: after all posted operations have completed and all
/// endpoint memory regions have been destroyed, `shutdown()` cancels and joins
/// the cq_pump asynchronously before tearing down the CQ / comp_channel / PD.
/// The pump scheduler must remain operational until that await completes, and
/// starting shutdown permanently ends data-path use. The destructor does not
/// wait for the pump. If it observes an active pump, or if checked QP
/// destruction fails, the affected ownership graph is intentionally leaked
/// rather than risk a use-after-free or continue destroying dependencies.

#if !defined(ELIO_HAS_RDMA_CM) || !ELIO_HAS_RDMA_CM
// endpoint depends on elio_rdma_cm for the CM bootstrap step. Build
// with -DELIO_ENABLE_RDMA_CM=ON to enable it; without that this
// header is a no-op so users who only need the backend / cq_drain
// helpers can still include the umbrella.
namespace elio::rdma_ibverbs { /* endpoint API not available */ }
#else

#include <elio/coro/cancel_token.hpp>
#include <elio/coro/detail/completion_waiter.hpp>
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

#include <fcntl.h>

#include <atomic>
#include <cerrno>
#include <cstdint>
#include <cstring>
#include <exception>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>

#if defined(ELIO_HAS_RDMA_CUDA) && ELIO_HAS_RDMA_CUDA
#include <elio/rdma_cuda/gpu_memory_region.hpp>
#endif

namespace elio::rdma_ibverbs {

namespace endpoint_detail {

inline void set_nonblocking(int fd) {
    const int flags = ::fcntl(fd, F_GETFL);
    if (flags < 0) {
        const int error = errno;
        throw std::runtime_error(
            std::string("endpoint: fcntl(F_GETFL) failed: ")
            + std::strerror(error));
    }
    if ((flags & O_NONBLOCK) == 0
        && ::fcntl(fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        const int error = errno;
        throw std::runtime_error(
            std::string("endpoint: fcntl(F_SETFL) failed: ")
            + std::strerror(error));
    }
}

inline void require_endpoint_active(bool shutdown_started,
                                    const char* operation) {
    if (shutdown_started) {
        throw std::logic_error(
            std::string("endpoint: ") + operation
            + " is not allowed after shutdown has started");
    }
}

[[nodiscard]] inline bool pump_resources_are_idle(bool pump_started,
                                                  bool pump_exited,
                                                  bool pump_frame_destroyed) noexcept {
    return !pump_started || pump_exited || pump_frame_destroyed;
}

class pump_exit_state {
public:
    class wait_awaitable {
    public:
        explicit wait_awaitable(pump_exit_state& state) noexcept
            : state_(&state), waiter_(state.waiter_) {}

        [[nodiscard]] bool await_ready() const noexcept {
            return state_->is_exited();
        }

        bool await_suspend(std::coroutine_handle<> handle) noexcept {
            return state_->waiter_.register_waiter(
                waiter_, handle, [this] { return state_->is_exited(); });
        }

        void await_resume() const noexcept {}

    private:
        pump_exit_state* state_;
        elio::coro::detail::completion_waiter waiter_;
    };

    [[nodiscard]] wait_awaitable wait() noexcept {
        return wait_awaitable(*this);
    }

    [[nodiscard]] bool is_exited() const noexcept {
        return exited_.load(std::memory_order_acquire);
    }

    void mark_exited() noexcept {
        exited_.store(true, std::memory_order_release);
        if (auto waiter = waiter_.take()) {
            elio::runtime::schedule_handle(waiter);
        }
    }

private:
    std::atomic<bool> exited_{false};
    elio::coro::detail::completion_waiter_slot waiter_;
};

template<typename Resource, typename Destroy>
void destroy_resource_or_throw(Resource*& resource,
                               Destroy&& destroy,
                               const char* operation) {
    if (!resource) return;

    errno = 0;
    const int result = std::forward<Destroy>(destroy)(resource);
    if (result != 0) {
        const int error = result > 0
            ? result
            : (errno != 0 ? errno : (result < -1 ? -result : EIO));
        errno = error;
        throw std::runtime_error(
            std::string("endpoint: ") + operation + " failed: "
            + std::strerror(error));
    }
    resource = nullptr;
}

} // namespace endpoint_detail

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
    /// propagates. The transferred cm_id must not already have an attached QP.
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
        if (id_.qp()) {
            throw std::runtime_error(
                "endpoint: cm_id already has an attached QP");
        }
        try {
            build_resources_();
            conn_ =
                std::make_unique<elio::rdma::connection<ibverbs_backend>>(
                    qp_, *disp_, elio::rdma::connection_config{
                                      .max_inline_data = cfg.max_inline_data});
        } catch (...) {
            destroy_resources_();
            throw;
        }
    }

    endpoint(const endpoint&) = delete;
    endpoint& operator=(const endpoint&) = delete;

    // Movable: every owned object that needs a stable address is behind a
    // unique_ptr (dispatcher, connection) or shared ownership (pump/cancel
    // state). Raw pointers are nulled in the source so its destructor doesn't
    // double-free.
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
          pump_state_(std::move(other.pump_state_)),
          pump_handle_(std::exchange(other.pump_handle_, std::nullopt)),
          shutdown_started_(std::exchange(other.shutdown_started_, true)) {}

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
            pump_handle_ = std::exchange(other.pump_handle_, std::nullopt);
            shutdown_started_ = std::exchange(other.shutdown_started_, true);
        }
        return *this;
    }

    ~endpoint() noexcept {
        request_pump_stop_();
        if (!destroy_qp_noexcept_()) {
            // rdma_cm's void-returning wrapper discards ibv_destroy_qp()
            // failures and then lets cm_id destruction continue. Preserve the
            // entire dependency graph, including the CM ID, instead.
            relinquish_all_resources_();
            return;
        }

        if (pump_resources_are_idle_()) {
            destroy_remaining_resources_();
        } else {
            // The pump still owns raw references to the CQ, completion channel,
            // and dispatcher. A destructor cannot join it without blocking the
            // current scheduler worker, so preserve the previous fail-closed
            // policy and intentionally leak those resources.
            (void)disp_.release();
        }
    }

    /// Returns the data-path connection. Lifetime is tied to the
    /// endpoint; do not move out.
    [[nodiscard]] elio::rdma::connection<ibverbs_backend>& conn() {
        require_active_("conn");
        return *conn_;
    }

    /// Register a buffer with this endpoint's PD. The returned MR is
    /// move-only and auto-deregisters at scope exit.
    [[nodiscard]] elio::rdma::memory_region<ibverbs_backend>
    register_buffer(void* addr, std::size_t length, int access) {
        require_active_("register_buffer");
        return elio::rdma::memory_region<ibverbs_backend>{
            pd_, addr, length, access};
    }

    /// Atomic CAS / FAA shortcuts that forward to the underlying
    /// connection (S15). The local buffer must be 8 bytes and the
    /// remote address must be 8-byte aligned.
    [[nodiscard]] auto cas(elio::rdma::buffer_view local,
                           elio::rdma::remote_buffer remote,
                           std::uint64_t compare, std::uint64_t swap,
                           elio::rdma::send_flags flags = {}) {
        require_active_("cas");
        return conn_->cas(local, remote, compare, swap, flags);
    }
    [[nodiscard]] auto fetch_add(elio::rdma::buffer_view local,
                                 elio::rdma::remote_buffer remote,
                                 std::uint64_t add,
                                 elio::rdma::send_flags flags = {}) {
        require_active_("fetch_add");
        return conn_->fetch_add(local, remote, add, flags);
    }

    /// Spawn the cq_pump on the given scheduler. Idempotent: a
    /// second call is a no-op.
    void start_cq_pump(elio::runtime::scheduler& sched) {
        require_active_("start_cq_pump");
        if (pump_handle_) return;
        pump_state_ = std::make_shared<endpoint_detail::pump_exit_state>();
        auto state = pump_state_;
        auto token = pump_cancel_.get_token();
        auto cq = cq_;
        auto comp_ch = comp_ch_;
        auto* disp = disp_.get();
        pump_handle_.emplace(sched.go_joinable(pump_runner{
            std::move(state), std::move(token), cq, comp_ch, disp}));
    }

    /// Cancel the cq_pump's loop. Does not wait; use shutdown() to join the
    /// pump and release its verbs resources without blocking a worker thread.
    void stop_cq_pump() noexcept {
        request_pump_stop_();
    }

    /// Gracefully stop and join the CQ pump, then release the QP, CQ,
    /// completion channel, and PD. The endpoint must remain alive until this
    /// task completes. Every posted operation and its awaiter must have
    /// completed, and all registered memory regions must be destroyed first.
    /// The scheduler passed to start_cq_pump() must remain operational until
    /// this task completes; force-stopping it can orphan in-flight I/O. Once
    /// this task starts executing the endpoint is terminal: only a later
    /// serialized shutdown() retry, raw null-state inspection, or destruction
    /// is supported. Resource-destruction failures retain ownership for that
    /// retry. Concurrent shutdown calls are not supported.
    [[nodiscard]] elio::coro::task<void> shutdown() {
        shutdown_started_ = true;
        request_pump_stop_();

        std::exception_ptr pump_error;
        if (pump_handle_) {
            co_await pump_state_->wait();
            try {
                if (pump_handle_->is_ready()) {
                    pump_handle_->await_resume();
                }
            } catch (...) {
                pump_error = std::current_exception();
            }
        }

        // The callable object publishes exit from its destructor after its last
        // raw-resource access. That covers normal completion, exceptions,
        // admission rejection, and runner-frame destruction while the pump
        // scheduler remains operational. Only consume the join result when the
        // join state completed; destruction does not manufacture success.
        destroy_qp_or_throw_();
        std::exception_ptr teardown_error;
        if (pump_resources_are_idle_()) {
            try {
                destroy_remaining_resources_or_throw_();
            } catch (...) {
                teardown_error = std::current_exception();
            }
        }

        if (teardown_error) {
            std::rethrow_exception(teardown_error);
        }

        if (pump_error) {
            std::rethrow_exception(pump_error);
        }
    }

    /// Terminal fail-closed escape hatch for fatal paths whose remaining
    /// provider-posted operations were launched eagerly with .start() and have
    /// never installed a coroutine waiter. Call this while those eager
    /// awaitables, their memory regions, and payload buffers are still alive.
    /// The QP is destroyed synchronously before returning, then the CQ,
    /// completion channel, PD, and dispatcher are intentionally relinquished
    /// so those eager objects can unwind without racing resource destruction.
    /// This does not make a suspended operation frame safe to destroy: once
    /// delivery snapshots its waiter, that coroutine must resume and consume
    /// the result. This operation never waits, permanently ends endpoint use,
    /// and intentionally leaks the relinquished resources. It returns true only
    /// after QP destruction succeeds. A false result retains the QP and every
    /// dependency: keep all operation/MR/buffer lifetimes intact, repair the
    /// provider state through the raw accessors, and retry (or terminate).
    /// Prefer shutdown() after normal completion.
    [[nodiscard]] bool abandon_outstanding() noexcept {
        shutdown_started_ = true;
        request_pump_stop_();
        if (!destroy_qp_noexcept_()) {
            return false;
        }

        relinquish_remaining_resources_();
        return true;
    }

#if defined(ELIO_HAS_RDMA_CUDA) && ELIO_HAS_RDMA_CUDA
    [[nodiscard]] elio::rdma_cuda::gpu_memory_region
    register_gpu_buffer(std::size_t size, int access) {
        require_active_("register_gpu_buffer");
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
    [[nodiscard]] elio::rdma::dispatcher& dispatcher_ref() {
        require_active_("dispatcher_ref");
        return *disp_;
    }
    [[nodiscard]] const endpoint_config& config() const noexcept {
        return config_;
    }

private:
    struct pump_runner {
        std::shared_ptr<endpoint_detail::pump_exit_state> state;
        elio::coro::cancel_token token;
        ibv_cq* cq;
        ibv_comp_channel* comp_ch;
        elio::rdma::dispatcher* disp;

        pump_runner(std::shared_ptr<endpoint_detail::pump_exit_state> state_arg,
                    elio::coro::cancel_token token_arg,
                    ibv_cq* cq_arg,
                    ibv_comp_channel* comp_ch_arg,
                    elio::rdma::dispatcher* disp_arg) noexcept
            : state(std::move(state_arg)),
              token(std::move(token_arg)),
              cq(cq_arg),
              comp_ch(comp_ch_arg),
              disp(disp_arg) {}

        pump_runner(const pump_runner&) = delete;
        pump_runner& operator=(const pump_runner&) = delete;

        pump_runner(pump_runner&& other) noexcept
            : state(std::move(other.state)),
              token(std::move(other.token)),
              cq(std::exchange(other.cq, nullptr)),
              comp_ch(std::exchange(other.comp_ch, nullptr)),
              disp(std::exchange(other.disp, nullptr)) {}

        ~pump_runner() {
            if (state) {
                state->mark_exited();
            }
        }

        [[nodiscard]] elio::coro::task<void> operator()() {
            co_await elio::rdma::cq_pump(comp_ch->fd, *disp,
                                         make_cq_drain(cq, comp_ch), token);
        }
    };

    void request_pump_stop_() noexcept {
        if (!pump_handle_) return;
        try {
            pump_cancel_.cancel();
        } catch (...) {
            // The endpoint-owned token is only registered with library I/O
            // waits, but teardown must remain noexcept even if that invariant
            // changes in the future.
        }
    }

    void require_active_(const char* operation) const {
        endpoint_detail::require_endpoint_active(
            shutdown_started_, operation);
    }

    [[nodiscard]] bool pump_resources_are_idle_() const noexcept {
        const bool pump_exited = pump_state_ && pump_state_->is_exited();
        return endpoint_detail::pump_resources_are_idle(
            pump_handle_.has_value(), pump_exited,
            pump_handle_ && pump_handle_->is_destroyed());
    }

    void destroy_qp_or_throw_() {
        if (!qp_) {
            return;
        }
        auto* owned_qp = qp_;
        if (!id_ || id_.native()->qp != owned_qp) {
            throw std::runtime_error(
                "endpoint: CM ID and endpoint QP ownership diverged");
        }
        endpoint_detail::destroy_resource_or_throw(
            qp_, [](ibv_qp* qp) { return ::ibv_destroy_qp(qp); },
            "ibv_destroy_qp");
        if (id_ && id_.native()->qp == owned_qp) {
            // Mirror rdma_destroy_qp() only after this exact provider QP was
            // destroyed, so cm_id cannot later attempt a double destroy.
            id_.native()->qp = nullptr;
        }
    }

    [[nodiscard]] bool destroy_qp_noexcept_() noexcept {
        try {
            destroy_qp_or_throw_();
            return true;
        } catch (...) {
            return false;
        }
    }

    void relinquish_remaining_resources_() noexcept {
        cq_ = nullptr;
        comp_ch_ = nullptr;
        pd_ = nullptr;
        (void)disp_.release();
    }

    void relinquish_all_resources_() noexcept {
        qp_ = nullptr;
        relinquish_remaining_resources_();
        (void)id_.release();
    }

    void destroy_remaining_resources_() noexcept {
        try {
            destroy_remaining_resources_or_throw_();
        } catch (...) {
            // Destructors and constructor rollback cannot report provider
            // failures. Stop at the first dependency that remains live and
            // intentionally retain the rest rather than invalidating it.
        }
    }

    void destroy_remaining_resources_or_throw_() {
        endpoint_detail::destroy_resource_or_throw(
            cq_, [](ibv_cq* cq) { return ::ibv_destroy_cq(cq); },
            "ibv_destroy_cq");
        endpoint_detail::destroy_resource_or_throw(
            comp_ch_,
            [](ibv_comp_channel* channel) {
                return ::ibv_destroy_comp_channel(channel);
            },
            "ibv_destroy_comp_channel");
        endpoint_detail::destroy_resource_or_throw(
            pd_, [](ibv_pd* pd) { return ::ibv_dealloc_pd(pd); },
            "ibv_dealloc_pd");
    }

    void build_resources_() {
        pd_ = ::ibv_alloc_pd(id_.verbs());
        if (!pd_) throw_verbs_("ibv_alloc_pd");
        comp_ch_ = ::ibv_create_comp_channel(id_.verbs());
        if (!comp_ch_) throw_verbs_("ibv_create_comp_channel");
        endpoint_detail::set_nonblocking(comp_ch_->fd);
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
        if (!destroy_qp_noexcept_()) {
            relinquish_all_resources_();
            return;
        }
        destroy_remaining_resources_();
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
    std::shared_ptr<endpoint_detail::pump_exit_state>    pump_state_;
    std::optional<elio::coro::join_handle<void>>         pump_handle_;
    bool                                                 shutdown_started_ = false;
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
