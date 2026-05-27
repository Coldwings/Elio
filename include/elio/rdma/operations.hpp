#pragma once

/// @file operations.hpp
/// @brief Data-path awaiters (send / recv / rdma_write / rdma_read).
///
/// Stage S3 lands `send_awaitable` and `recv_awaitable`.
/// Stage S4 will add `rdma_write_awaitable` and `rdma_read_awaitable`
/// with the same pattern.
///
/// Each awaiter is parameterised on the backend type. Two backend
/// styles are supported via the `backend_invoker<B>` trait below:
///
///   * For a static-traits backend (any `B` satisfying
///     `backend_traits<B>`), `backend_invoker<B>::post_*` forwards to
///     `B::post_*(...)`. Zero runtime overhead.
///   * For the runtime-replaceable `polymorphic_backend`, the
///     specialisation forwards to the instance's virtual method via a
///     `polymorphic_backend*` carried in the awaiter.
///
/// Lifetime: every awaiter owns a `std::unique_ptr<op_state>` while
/// suspended. The op_state ↔ dispatcher race (see op_state.hpp +
/// completion.hpp) decides which side frees the node on the final
/// transition.

#include <elio/rdma/backend_traits.hpp>
#include <elio/rdma/completion.hpp>
#include <elio/rdma/op_state.hpp>
#include <elio/rdma/types.hpp>

#include <coroutine>
#include <cstdint>
#include <memory>
#include <span>

namespace elio::rdma::detail {

/// Trait that lets the same awaiter type drive both the static-traits
/// backend path and the polymorphic_backend path. Primary template uses
/// static dispatch; the specialisation for `polymorphic_backend` uses
/// the carried instance pointer.
template <typename Backend>
struct backend_invoker {
    static int post_send(void* qp,
                         std::span<const sge> sges,
                         send_flags flags,
                         wr_id id,
                         [[maybe_unused]] Backend* backend) noexcept {
        return Backend::post_send(qp, sges, flags, id);
    }
    static int post_recv(void* qp,
                         std::span<const sge> sges,
                         wr_id id,
                         [[maybe_unused]] Backend* backend) noexcept {
        return Backend::post_recv(qp, sges, id);
    }
    static int post_rdma_write(void* qp,
                               std::span<const sge> sges,
                               remote_buffer rb,
                               send_flags flags,
                               wr_id id,
                               [[maybe_unused]] Backend* backend) noexcept {
        return Backend::post_rdma_write(qp, sges, rb, flags, id);
    }
    static int post_rdma_read(void* qp,
                              std::span<const sge> sges,
                              remote_buffer rb,
                              wr_id id,
                              [[maybe_unused]] Backend* backend) noexcept {
        return Backend::post_rdma_read(qp, sges, rb, id);
    }
};

template <>
struct backend_invoker<polymorphic_backend> {
    static int post_send(void* qp,
                         std::span<const sge> sges,
                         send_flags flags,
                         wr_id id,
                         polymorphic_backend* backend) noexcept {
        return backend->post_send(qp, sges, flags, id);
    }
    static int post_recv(void* qp,
                         std::span<const sge> sges,
                         wr_id id,
                         polymorphic_backend* backend) noexcept {
        return backend->post_recv(qp, sges, id);
    }
    static int post_rdma_write(void* qp,
                               std::span<const sge> sges,
                               remote_buffer rb,
                               send_flags flags,
                               wr_id id,
                               polymorphic_backend* backend) noexcept {
        return backend->post_rdma_write(qp, sges, rb, flags, id);
    }
    static int post_rdma_read(void* qp,
                              std::span<const sge> sges,
                              remote_buffer rb,
                              wr_id id,
                              polymorphic_backend* backend) noexcept {
        return backend->post_rdma_read(qp, sges, rb, id);
    }
};

/// Shared awaiter machinery — owns the op_state, runs the orphan race
/// in the destructor, exposes a typed `await_resume` returning the
/// `wc_result` filled in by the dispatcher.
///
/// Subclasses must implement `do_post()` returning the backend's int
/// status; on a non-zero return the awaiter synthesises a failed
/// `wc_result` (status = wr_flush_error) and resumes inline rather
/// than suspending.
class op_awaiter_base {
public:
    op_awaiter_base() noexcept
        : op_(std::make_unique<op_state>()) {}

    op_awaiter_base(const op_awaiter_base&) = delete;
    op_awaiter_base& operator=(const op_awaiter_base&) = delete;
    op_awaiter_base(op_awaiter_base&&) noexcept = default;
    op_awaiter_base& operator=(op_awaiter_base&&) noexcept = default;

    ~op_awaiter_base() noexcept {
        // If we still own `op_`, try to flip pending → orphaned so the
        // dispatcher's later CQE arrival frees the heap node instead of
        // us. If the dispatcher already completed (state == completed),
        // try_orphan returns false; the unique_ptr's destructor frees
        // the node — which is exactly the path that lets `await_resume`
        // return a valid `wc_result` snapshot.
        if (op_) {
            if (dispatcher::try_orphan(op_.get())) {
                // Dispatcher will take it from here.
                (void)op_.release();
            }
        }
    }

    [[nodiscard]] bool await_ready() const noexcept { return false; }

    [[nodiscard]] wc_result await_resume() noexcept {
        // op_ is guaranteed live here (the awaiter destructor hasn't
        // run yet, and the resume can only happen via dispatcher::
        // deliver which CASed to completed before the schedule_handle
        // call). Snapshot then return.
        return op_->result;
    }

protected:
    /// Called by derived class's await_suspend before posting the WR.
    /// Returns the wr_id to pass into the backend's post_* function.
    [[nodiscard]] wr_id arm_(std::coroutine_handle<> h) noexcept {
        op_->handle = h;
        return dispatcher::make_wr_id(op_.get());
    }

    /// If the backend reported a synchronous failure to post (rc != 0),
    /// synthesise a failed completion locally and return false so the
    /// awaiter resumes inline without suspending. Otherwise return true.
    [[nodiscard]] bool finalize_post_(int rc) noexcept {
        if (rc == 0) {
            return true;  // suspend; CQE will arrive later
        }
        // Post failed at submission time. There will be no CQE. Write
        // the synthesised result and flip phase → completed so the
        // awaiter destructor's try_orphan returns false (no one will
        // ever deliver for this wr_id, so we must own the free here).
        // The unique_ptr in op_awaiter_base will free `op_` on
        // destruction.
        op_->result = wc_result{
            .status   = wc_status::wr_flush_error,
            .byte_len = 0,
            .imm_data = static_cast<std::uint32_t>(-rc),  // pass back errno-ish
            .wc_flags = 0,
        };
        op_->phase.store(op_phase::completed,
                         std::memory_order_release);
        return false;  // do NOT suspend
    }

private:
    std::unique_ptr<op_state> op_;
};

/// SEND awaiter. Single SGE (S5a will add multi-SGE overload).
template <typename Backend>
class send_awaitable : public op_awaiter_base {
public:
    send_awaitable(void* qp,
                   Backend* backend_or_null,
                   buffer_view buf,
                   send_flags flags) noexcept
        : qp_(qp), backend_(backend_or_null), buf_(buf), flags_(flags) {}

    [[nodiscard]] bool await_suspend(std::coroutine_handle<> h) noexcept {
        const auto sge_val = sge::from(buf_);
        auto sges = std::span<const sge>(&sge_val, 1);
        const auto id = arm_(h);
        const int rc = backend_invoker<Backend>::post_send(
            qp_, sges, flags_, id, backend_);
        return finalize_post_(rc);
    }

private:
    void*       qp_;
    Backend*    backend_;
    buffer_view buf_;
    send_flags  flags_;
};

/// RECV awaiter. Single SGE for now.
template <typename Backend>
class recv_awaitable : public op_awaiter_base {
public:
    recv_awaitable(void* qp,
                   Backend* backend_or_null,
                   buffer_view buf) noexcept
        : qp_(qp), backend_(backend_or_null), buf_(buf) {}

    [[nodiscard]] bool await_suspend(std::coroutine_handle<> h) noexcept {
        const auto sge_val = sge::from(buf_);
        auto sges = std::span<const sge>(&sge_val, 1);
        const auto id = arm_(h);
        const int rc = backend_invoker<Backend>::post_recv(
            qp_, sges, id, backend_);
        return finalize_post_(rc);
    }

private:
    void*       qp_;
    Backend*    backend_;
    buffer_view buf_;
};

}  // namespace elio::rdma::detail
