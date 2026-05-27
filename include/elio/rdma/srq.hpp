#pragma once

/// @file srq.hpp
/// @brief Shared Receive Queue (SRQ) abstraction (stage S5c).
///
/// A SRQ is an ibverbs object that can serve receive WRs to one or
/// more QPs. Posting a receive WR to a SRQ instead of a per-QP RQ
/// lets a fleet of QPs share buffer credits, which matters at scale
/// (one pool of recv buffers rather than N pools).
///
/// `srq<Backend>` is parameterised on a backend type the same way
/// `connection<Backend>` is, with the same static-traits / polymorphic
/// duo:
///
///   * `srq<B>` where `B` satisfies `backend_with_srq<B>` — static
///     dispatch through `B::post_srq_recv`.
///   * `srq<polymorphic_backend>` — runtime dispatch through the
///     `polymorphic_backend::post_srq_recv` virtual. The default
///     polymorphic implementation returns `-ENOTSUP`, surfaced to
///     callers as a `wr_flush_error` completion through the
///     awaiter's standard post-failure path.
///
/// Lifetime: the user owns the underlying SRQ resource (typically a
/// long-lived `ibv_srq*`) and ensures it outlives every `srq<>` value
/// referencing it AND every in-flight `srq_recv_awaitable` posted
/// against it. The wrapper itself is a thin handle, cheap to copy /
/// move.
///
/// Posting:
///
/// @code{.cpp}
/// elio::rdma::srq<my_backend> recv_pool{srq_ptr, disp};
/// auto wc = co_await recv_pool.recv(buffer_view{...});
/// @endcode

#include <elio/rdma/backend_traits.hpp>
#include <elio/rdma/completion.hpp>
#include <elio/rdma/operations.hpp>
#include <elio/rdma/types.hpp>

#include <span>

namespace elio::rdma {

/// Primary template — static-traits SRQ. Requires
/// `backend_with_srq<Backend>` at the call site of `recv()`; not at
/// class instantiation, so a user can construct an `srq<Backend>`
/// before they know whether they will issue WRs against it.
template <typename Backend>
class srq {
public:
    /// @param srq_ptr Opaque SRQ handle (typically `ibv_srq*`). The
    ///                backend's `post_srq_recv` casts it back.
    /// @param disp    Dispatcher that will receive CQEs originated by
    ///                this SRQ's recv WRs. Must outlive the srq.
    srq(void* srq_ptr, dispatcher& disp) noexcept
        : srq_(srq_ptr), dispatcher_(&disp) {}

    srq(const srq&) = default;
    srq& operator=(const srq&) = default;
    srq(srq&&) noexcept = default;
    srq& operator=(srq&&) noexcept = default;
    ~srq() = default;

    [[nodiscard]] void* native() const noexcept { return srq_; }
    [[nodiscard]] dispatcher& dispatcher_ref() const noexcept {
        return *dispatcher_;
    }

    [[nodiscard]] detail::srq_recv_awaitable<Backend>
    recv(buffer_view buf) noexcept {
        static_assert(backend_with_srq<Backend>,
                      "Backend must satisfy backend_with_srq to use "
                      "srq::recv (provide static post_srq_recv).");
        return detail::srq_recv_awaitable<Backend>(
            srq_, /*backend=*/nullptr, buf);
    }

    [[nodiscard]] detail::srq_recv_awaitable<Backend>
    recv(std::span<const sge> sges) noexcept {
        static_assert(backend_with_srq<Backend>,
                      "Backend must satisfy backend_with_srq to use "
                      "srq::recv (provide static post_srq_recv).");
        return detail::srq_recv_awaitable<Backend>(
            srq_, /*backend=*/nullptr, sges);
    }

private:
    void*       srq_;
    dispatcher* dispatcher_;
};

/// Specialisation for the polymorphic backend. The
/// post_srq_recv vtable slot has a default `-ENOTSUP` implementation,
/// so calling `recv()` on an SRQ whose backend hasn't overridden it
/// simply produces a wr_flush_error completion rather than a
/// compile-time rejection.
template <>
class srq<polymorphic_backend> {
public:
    srq(void* srq_ptr, polymorphic_backend& backend,
        dispatcher& disp) noexcept
        : srq_(srq_ptr), backend_(&backend), dispatcher_(&disp) {}

    srq(const srq&) = default;
    srq& operator=(const srq&) = default;
    srq(srq&&) noexcept = default;
    srq& operator=(srq&&) noexcept = default;
    ~srq() = default;

    [[nodiscard]] void* native() const noexcept { return srq_; }
    [[nodiscard]] polymorphic_backend& backend() const noexcept {
        return *backend_;
    }
    [[nodiscard]] dispatcher& dispatcher_ref() const noexcept {
        return *dispatcher_;
    }

    [[nodiscard]] detail::srq_recv_awaitable<polymorphic_backend>
    recv(buffer_view buf) noexcept {
        return detail::srq_recv_awaitable<polymorphic_backend>(
            srq_, backend_, buf);
    }

    [[nodiscard]] detail::srq_recv_awaitable<polymorphic_backend>
    recv(std::span<const sge> sges) noexcept {
        return detail::srq_recv_awaitable<polymorphic_backend>(
            srq_, backend_, sges);
    }

private:
    void*                srq_;
    polymorphic_backend* backend_;
    dispatcher*          dispatcher_;
};

}  // namespace elio::rdma
