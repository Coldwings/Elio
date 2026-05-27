#pragma once

/// @file connection.hpp
/// @brief connection<Backend> — wraps a single QP and a dispatcher.
///
/// Stage S2: skeleton only. Holds the user-supplied QP pointer (opaque
/// `void*`, typically an `ibv_qp*` in production), a reference to the
/// dispatcher that the corresponding completion poller will deliver
/// CQEs to, and a per-connection configuration (currently just
/// `max_inline_data`, used by S5b's inline send path).
///
/// The four data-path member functions (`send`, `recv`, `rdma_write`,
/// `rdma_read`) are declared but their bodies land in S3 (send/recv)
/// and S4 (rdma_write/rdma_read). At S2 calling them is a compile
/// error — the awaiter return types are forward-declared only.
///
/// **Specialisation** for the polymorphic backend (`connection<polymorphic_backend>`)
/// dispatches data-path calls through the vtable rather than the
/// static-traits surface. The implementation is the same; only the
/// trait constraint changes.

#include <elio/rdma/backend_traits.hpp>
#include <elio/rdma/completion.hpp>
#include <elio/rdma/types.hpp>

#include <cstddef>

namespace elio::rdma {

/// Per-connection configuration. Plain aggregate; users construct one
/// alongside the QP.
struct connection_config {
    /// Maximum payload eligible for inline send (S5b). Backends must
    /// reject `send_flags::inline_send` for payloads larger than this.
    /// Defaults to 0 (inline disabled) so older backends without inline
    /// support work without modification.
    std::size_t max_inline_data = 0;
};

namespace detail {

// Awaiter types are declared in operations.hpp (S3+). Forward-declare
// here so member-function signatures can name them.
template <typename Backend> struct send_awaitable;
template <typename Backend> struct recv_awaitable;
template <typename Backend> struct rdma_write_awaitable;
template <typename Backend> struct rdma_read_awaitable;

}  // namespace detail

/// Primary template: static-traits backend. `Backend` must satisfy
/// `backend_traits<Backend>`; the constraint is checked at the call
/// site of each data-path method so users only see the requirement
/// when they actually issue a WR.
template <typename Backend>
class connection {
public:
    /// @param qp          Opaque QP pointer. The backend interprets it
    ///                    (typically `static_cast<ibv_qp*>(qp)`).
    /// @param disp        Dispatcher that will receive CQEs for this
    ///                    connection's CQ. Must outlive the connection.
    /// @param cfg         Per-connection configuration.
    connection(void* qp, dispatcher& disp,
               connection_config cfg = {}) noexcept
        : qp_(qp), dispatcher_(&disp), config_(cfg) {}

    connection(const connection&) = delete;
    connection& operator=(const connection&) = delete;
    connection(connection&&) noexcept = default;
    connection& operator=(connection&&) noexcept = default;
    ~connection() = default;

    [[nodiscard]] void* qp() const noexcept { return qp_; }
    [[nodiscard]] dispatcher& dispatcher_ref() const noexcept {
        return *dispatcher_;
    }
    [[nodiscard]] const connection_config& config() const noexcept {
        return config_;
    }

    // Data-path member functions land in S3 / S4. Declarations only —
    // attempting to call these at S2 produces a clean "incomplete
    // type" error rather than a confusing template instantiation
    // failure. Each will be defined as a function returning the
    // corresponding awaitable type from operations.hpp.

    // detail::send_awaitable<Backend>      send(buffer_view buf, send_flags flags = {}) noexcept;
    // detail::recv_awaitable<Backend>      recv(buffer_view buf) noexcept;
    // detail::rdma_write_awaitable<Backend> rdma_write(buffer_view local,
    //                                                  remote_buffer remote,
    //                                                  send_flags flags = {}) noexcept;
    // detail::rdma_read_awaitable<Backend>  rdma_read(buffer_view local,
    //                                                 remote_buffer remote) noexcept;

private:
    void*              qp_         = nullptr;
    dispatcher*        dispatcher_ = nullptr;
    connection_config  config_{};
};

/// Specialisation for the polymorphic backend — same interface, dispatch
/// goes through the vtable. The user holds a `polymorphic_backend*`
/// alongside the QP pointer.
template <>
class connection<polymorphic_backend> {
public:
    connection(void* qp, polymorphic_backend& backend, dispatcher& disp,
               connection_config cfg = {}) noexcept
        : qp_(qp), backend_(&backend), dispatcher_(&disp), config_(cfg) {}

    connection(const connection&) = delete;
    connection& operator=(const connection&) = delete;
    connection(connection&&) noexcept = default;
    connection& operator=(connection&&) noexcept = default;
    ~connection() = default;

    [[nodiscard]] void* qp() const noexcept { return qp_; }
    [[nodiscard]] polymorphic_backend& backend() const noexcept {
        return *backend_;
    }
    [[nodiscard]] dispatcher& dispatcher_ref() const noexcept {
        return *dispatcher_;
    }
    [[nodiscard]] const connection_config& config() const noexcept {
        return config_;
    }

private:
    void*                qp_         = nullptr;
    polymorphic_backend* backend_    = nullptr;
    dispatcher*          dispatcher_ = nullptr;
    connection_config    config_{};
};

}  // namespace elio::rdma
