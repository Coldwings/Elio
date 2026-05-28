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
#include <span>

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

// Awaiter types are defined in operations.hpp (S3+). Forward-declare
// here so member-function signatures can name them; operations.hpp is
// included at the bottom of this header so inline method bodies can
// instantiate them.
template <typename Backend> class send_awaitable;
template <typename Backend> class recv_awaitable;
template <typename Backend> class rdma_write_awaitable;
template <typename Backend> class rdma_read_awaitable;
template <typename Backend> class rdma_cas_awaitable;
template <typename Backend> class rdma_faa_awaitable;

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

    // Data-path member functions. Bodies are defined inline at the
    // bottom of this header, after operations.hpp has been included
    // (the awaiter types are incomplete here). Each operation has a
    // single-`buffer_view` convenience overload and an `std::span<const
    // sge>` overload for multi-segment scatter / gather (S5a).
    [[nodiscard]] detail::send_awaitable<Backend>
        send(buffer_view buf, send_flags flags = {}) noexcept;
    [[nodiscard]] detail::send_awaitable<Backend>
        send(std::span<const sge> sges, send_flags flags = {}) noexcept;
    [[nodiscard]] detail::send_awaitable<Backend>
        send_with_imm(buffer_view buf, std::uint32_t imm_data,
                      send_flags flags = {}) noexcept;
    [[nodiscard]] detail::send_awaitable<Backend>
        send_with_imm(std::span<const sge> sges, std::uint32_t imm_data,
                      send_flags flags = {}) noexcept;
    [[nodiscard]] detail::recv_awaitable<Backend>
        recv(buffer_view buf) noexcept;
    [[nodiscard]] detail::recv_awaitable<Backend>
        recv(std::span<const sge> sges) noexcept;
    [[nodiscard]] detail::rdma_write_awaitable<Backend>
        rdma_write(buffer_view local, remote_buffer remote,
                   send_flags flags = {}) noexcept;
    [[nodiscard]] detail::rdma_write_awaitable<Backend>
        rdma_write(std::span<const sge> locals, remote_buffer remote,
                   send_flags flags = {}) noexcept;
    [[nodiscard]] detail::rdma_write_awaitable<Backend>
        rdma_write_with_imm(buffer_view local, remote_buffer remote,
                            std::uint32_t imm_data,
                            send_flags flags = {}) noexcept;
    [[nodiscard]] detail::rdma_write_awaitable<Backend>
        rdma_write_with_imm(std::span<const sge> locals,
                            remote_buffer remote,
                            std::uint32_t imm_data,
                            send_flags flags = {}) noexcept;
    [[nodiscard]] detail::rdma_read_awaitable<Backend>
        rdma_read(buffer_view local, remote_buffer remote) noexcept;
    [[nodiscard]] detail::rdma_read_awaitable<Backend>
        rdma_read(std::span<const sge> locals,
                  remote_buffer remote) noexcept;

    // 8-byte ATOMIC ops (S15). Single buffer_view only — ibverbs
    // atomic WRs require exactly one 8-byte SGE. Backends must
    // satisfy backend_with_atomic<Backend>; the static_assert fires
    // at the call site so unsupported backends still let users
    // construct connection<> for non-atomic paths.
    [[nodiscard]] detail::rdma_cas_awaitable<Backend>
        cas(buffer_view local, remote_buffer remote,
            std::uint64_t compare, std::uint64_t swap,
            send_flags flags = {}) noexcept;
    [[nodiscard]] detail::rdma_faa_awaitable<Backend>
        fetch_add(buffer_view local, remote_buffer remote,
                  std::uint64_t add,
                  send_flags flags = {}) noexcept;

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

    // Data-path member functions. Bodies are inline below this class
    // after operations.hpp is included. Dispatch goes through the
    // polymorphic backend's virtual methods. Each operation has the
    // same single-buffer / multi-SGE overload pair as the primary
    // template (S5a). The *_with_imm variants set send_flags::with_imm
    // and carry imm_data through to the backend (S11).
    [[nodiscard]] detail::send_awaitable<polymorphic_backend>
        send(buffer_view buf, send_flags flags = {}) noexcept;
    [[nodiscard]] detail::send_awaitable<polymorphic_backend>
        send(std::span<const sge> sges, send_flags flags = {}) noexcept;
    [[nodiscard]] detail::send_awaitable<polymorphic_backend>
        send_with_imm(buffer_view buf, std::uint32_t imm_data,
                      send_flags flags = {}) noexcept;
    [[nodiscard]] detail::send_awaitable<polymorphic_backend>
        send_with_imm(std::span<const sge> sges, std::uint32_t imm_data,
                      send_flags flags = {}) noexcept;
    [[nodiscard]] detail::recv_awaitable<polymorphic_backend>
        recv(buffer_view buf) noexcept;
    [[nodiscard]] detail::recv_awaitable<polymorphic_backend>
        recv(std::span<const sge> sges) noexcept;
    [[nodiscard]] detail::rdma_write_awaitable<polymorphic_backend>
        rdma_write(buffer_view local, remote_buffer remote,
                   send_flags flags = {}) noexcept;
    [[nodiscard]] detail::rdma_write_awaitable<polymorphic_backend>
        rdma_write(std::span<const sge> locals, remote_buffer remote,
                   send_flags flags = {}) noexcept;
    [[nodiscard]] detail::rdma_write_awaitable<polymorphic_backend>
        rdma_write_with_imm(buffer_view local, remote_buffer remote,
                            std::uint32_t imm_data,
                            send_flags flags = {}) noexcept;
    [[nodiscard]] detail::rdma_write_awaitable<polymorphic_backend>
        rdma_write_with_imm(std::span<const sge> locals,
                            remote_buffer remote,
                            std::uint32_t imm_data,
                            send_flags flags = {}) noexcept;
    [[nodiscard]] detail::rdma_read_awaitable<polymorphic_backend>
        rdma_read(buffer_view local, remote_buffer remote) noexcept;
    [[nodiscard]] detail::rdma_read_awaitable<polymorphic_backend>
        rdma_read(std::span<const sge> locals,
                  remote_buffer remote) noexcept;

    // 8-byte ATOMIC ops (S15). Backends without an
    // post_atomic_* override get the polymorphic_backend default
    // (-ENOTSUP) surfaced as wr_flush_error on the awaiter.
    [[nodiscard]] detail::rdma_cas_awaitable<polymorphic_backend>
        cas(buffer_view local, remote_buffer remote,
            std::uint64_t compare, std::uint64_t swap,
            send_flags flags = {}) noexcept;
    [[nodiscard]] detail::rdma_faa_awaitable<polymorphic_backend>
        fetch_add(buffer_view local, remote_buffer remote,
                  std::uint64_t add,
                  send_flags flags = {}) noexcept;

private:
    void*                qp_         = nullptr;
    polymorphic_backend* backend_    = nullptr;
    dispatcher*          dispatcher_ = nullptr;
    connection_config    config_{};
};

}  // namespace elio::rdma

// Pull in the awaiter definitions so the inline method bodies below
// can construct them. operations.hpp does NOT include connection.hpp
// (only forward-declares connection<>), so this is safe.
#include <elio/rdma/operations.hpp>

namespace elio::rdma {

// Out-of-line definitions for connection<Backend>::send / recv. Kept
// inline so the whole module stays header-only.
template <typename Backend>
inline detail::send_awaitable<Backend>
connection<Backend>::send(buffer_view buf, send_flags flags) noexcept {
    static_assert(backend_traits<Backend>,
                  "Backend must satisfy elio::rdma::backend_traits");
    return detail::send_awaitable<Backend>(qp_, /*backend=*/nullptr,
                                           buf, flags,
                                           config_.max_inline_data);
}

template <typename Backend>
inline detail::send_awaitable<Backend>
connection<Backend>::send(std::span<const sge> sges,
                          send_flags flags) noexcept {
    static_assert(backend_traits<Backend>,
                  "Backend must satisfy elio::rdma::backend_traits");
    return detail::send_awaitable<Backend>(qp_, /*backend=*/nullptr,
                                           sges, flags,
                                           config_.max_inline_data);
}

template <typename Backend>
inline detail::send_awaitable<Backend>
connection<Backend>::send_with_imm(buffer_view buf, std::uint32_t imm_data,
                                   send_flags flags) noexcept {
    static_assert(backend_traits<Backend>,
                  "Backend must satisfy elio::rdma::backend_traits");
    flags.with_imm = true;
    return detail::send_awaitable<Backend>(qp_, /*backend=*/nullptr,
                                           buf, flags,
                                           config_.max_inline_data,
                                           imm_data);
}

template <typename Backend>
inline detail::send_awaitable<Backend>
connection<Backend>::send_with_imm(std::span<const sge> sges,
                                   std::uint32_t imm_data,
                                   send_flags flags) noexcept {
    static_assert(backend_traits<Backend>,
                  "Backend must satisfy elio::rdma::backend_traits");
    flags.with_imm = true;
    return detail::send_awaitable<Backend>(qp_, /*backend=*/nullptr,
                                           sges, flags,
                                           config_.max_inline_data,
                                           imm_data);
}

template <typename Backend>
inline detail::recv_awaitable<Backend>
connection<Backend>::recv(buffer_view buf) noexcept {
    static_assert(backend_traits<Backend>,
                  "Backend must satisfy elio::rdma::backend_traits");
    return detail::recv_awaitable<Backend>(qp_, /*backend=*/nullptr, buf);
}

template <typename Backend>
inline detail::recv_awaitable<Backend>
connection<Backend>::recv(std::span<const sge> sges) noexcept {
    static_assert(backend_traits<Backend>,
                  "Backend must satisfy elio::rdma::backend_traits");
    return detail::recv_awaitable<Backend>(qp_, /*backend=*/nullptr, sges);
}

template <typename Backend>
inline detail::rdma_write_awaitable<Backend>
connection<Backend>::rdma_write(buffer_view local, remote_buffer remote,
                                send_flags flags) noexcept {
    static_assert(backend_traits<Backend>,
                  "Backend must satisfy elio::rdma::backend_traits");
    return detail::rdma_write_awaitable<Backend>(
        qp_, /*backend=*/nullptr, local, remote, flags,
        config_.max_inline_data);
}

template <typename Backend>
inline detail::rdma_write_awaitable<Backend>
connection<Backend>::rdma_write(std::span<const sge> locals,
                                remote_buffer remote,
                                send_flags flags) noexcept {
    static_assert(backend_traits<Backend>,
                  "Backend must satisfy elio::rdma::backend_traits");
    return detail::rdma_write_awaitable<Backend>(
        qp_, /*backend=*/nullptr, locals, remote, flags,
        config_.max_inline_data);
}

template <typename Backend>
inline detail::rdma_write_awaitable<Backend>
connection<Backend>::rdma_write_with_imm(buffer_view local,
                                         remote_buffer remote,
                                         std::uint32_t imm_data,
                                         send_flags flags) noexcept {
    static_assert(backend_traits<Backend>,
                  "Backend must satisfy elio::rdma::backend_traits");
    flags.with_imm = true;
    return detail::rdma_write_awaitable<Backend>(
        qp_, /*backend=*/nullptr, local, remote, flags,
        config_.max_inline_data, imm_data);
}

template <typename Backend>
inline detail::rdma_write_awaitable<Backend>
connection<Backend>::rdma_write_with_imm(std::span<const sge> locals,
                                         remote_buffer remote,
                                         std::uint32_t imm_data,
                                         send_flags flags) noexcept {
    static_assert(backend_traits<Backend>,
                  "Backend must satisfy elio::rdma::backend_traits");
    flags.with_imm = true;
    return detail::rdma_write_awaitable<Backend>(
        qp_, /*backend=*/nullptr, locals, remote, flags,
        config_.max_inline_data, imm_data);
}

template <typename Backend>
inline detail::rdma_read_awaitable<Backend>
connection<Backend>::rdma_read(buffer_view local,
                               remote_buffer remote) noexcept {
    static_assert(backend_traits<Backend>,
                  "Backend must satisfy elio::rdma::backend_traits");
    return detail::rdma_read_awaitable<Backend>(
        qp_, /*backend=*/nullptr, local, remote);
}

template <typename Backend>
inline detail::rdma_read_awaitable<Backend>
connection<Backend>::rdma_read(std::span<const sge> locals,
                               remote_buffer remote) noexcept {
    static_assert(backend_traits<Backend>,
                  "Backend must satisfy elio::rdma::backend_traits");
    return detail::rdma_read_awaitable<Backend>(
        qp_, /*backend=*/nullptr, locals, remote);
}

inline detail::send_awaitable<polymorphic_backend>
connection<polymorphic_backend>::send(buffer_view buf,
                                      send_flags flags) noexcept {
    return detail::send_awaitable<polymorphic_backend>(
        qp_, backend_, buf, flags, config_.max_inline_data);
}

inline detail::send_awaitable<polymorphic_backend>
connection<polymorphic_backend>::send(std::span<const sge> sges,
                                      send_flags flags) noexcept {
    return detail::send_awaitable<polymorphic_backend>(
        qp_, backend_, sges, flags, config_.max_inline_data);
}

inline detail::send_awaitable<polymorphic_backend>
connection<polymorphic_backend>::send_with_imm(buffer_view buf,
                                               std::uint32_t imm_data,
                                               send_flags flags) noexcept {
    flags.with_imm = true;
    return detail::send_awaitable<polymorphic_backend>(
        qp_, backend_, buf, flags, config_.max_inline_data, imm_data);
}

inline detail::send_awaitable<polymorphic_backend>
connection<polymorphic_backend>::send_with_imm(std::span<const sge> sges,
                                               std::uint32_t imm_data,
                                               send_flags flags) noexcept {
    flags.with_imm = true;
    return detail::send_awaitable<polymorphic_backend>(
        qp_, backend_, sges, flags, config_.max_inline_data, imm_data);
}

inline detail::recv_awaitable<polymorphic_backend>
connection<polymorphic_backend>::recv(buffer_view buf) noexcept {
    return detail::recv_awaitable<polymorphic_backend>(
        qp_, backend_, buf);
}

inline detail::recv_awaitable<polymorphic_backend>
connection<polymorphic_backend>::recv(std::span<const sge> sges) noexcept {
    return detail::recv_awaitable<polymorphic_backend>(
        qp_, backend_, sges);
}

inline detail::rdma_write_awaitable<polymorphic_backend>
connection<polymorphic_backend>::rdma_write(buffer_view local,
                                            remote_buffer remote,
                                            send_flags flags) noexcept {
    return detail::rdma_write_awaitable<polymorphic_backend>(
        qp_, backend_, local, remote, flags,
        config_.max_inline_data);
}

inline detail::rdma_write_awaitable<polymorphic_backend>
connection<polymorphic_backend>::rdma_write(std::span<const sge> locals,
                                            remote_buffer remote,
                                            send_flags flags) noexcept {
    return detail::rdma_write_awaitable<polymorphic_backend>(
        qp_, backend_, locals, remote, flags,
        config_.max_inline_data);
}

inline detail::rdma_write_awaitable<polymorphic_backend>
connection<polymorphic_backend>::rdma_write_with_imm(
    buffer_view local, remote_buffer remote,
    std::uint32_t imm_data, send_flags flags) noexcept {
    flags.with_imm = true;
    return detail::rdma_write_awaitable<polymorphic_backend>(
        qp_, backend_, local, remote, flags,
        config_.max_inline_data, imm_data);
}

inline detail::rdma_write_awaitable<polymorphic_backend>
connection<polymorphic_backend>::rdma_write_with_imm(
    std::span<const sge> locals, remote_buffer remote,
    std::uint32_t imm_data, send_flags flags) noexcept {
    flags.with_imm = true;
    return detail::rdma_write_awaitable<polymorphic_backend>(
        qp_, backend_, locals, remote, flags,
        config_.max_inline_data, imm_data);
}

inline detail::rdma_read_awaitable<polymorphic_backend>
connection<polymorphic_backend>::rdma_read(buffer_view local,
                                           remote_buffer remote) noexcept {
    return detail::rdma_read_awaitable<polymorphic_backend>(
        qp_, backend_, local, remote);
}

inline detail::rdma_read_awaitable<polymorphic_backend>
connection<polymorphic_backend>::rdma_read(std::span<const sge> locals,
                                           remote_buffer remote) noexcept {
    return detail::rdma_read_awaitable<polymorphic_backend>(
        qp_, backend_, locals, remote);
}

// --- ATOMIC: connection<Backend> ---

template <typename Backend>
inline detail::rdma_cas_awaitable<Backend>
connection<Backend>::cas(buffer_view local, remote_buffer remote,
                         std::uint64_t compare, std::uint64_t swap,
                         send_flags flags) noexcept {
    static_assert(backend_with_atomic<Backend>,
                  "Backend must satisfy elio::rdma::backend_with_atomic "
                  "to use connection::cas (provide static post_atomic_cas).");
    return detail::rdma_cas_awaitable<Backend>(
        qp_, /*backend=*/nullptr, local, remote, compare, swap, flags);
}

template <typename Backend>
inline detail::rdma_faa_awaitable<Backend>
connection<Backend>::fetch_add(buffer_view local, remote_buffer remote,
                               std::uint64_t add,
                               send_flags flags) noexcept {
    static_assert(backend_with_atomic<Backend>,
                  "Backend must satisfy elio::rdma::backend_with_atomic "
                  "to use connection::fetch_add (provide static "
                  "post_atomic_fetch_add).");
    return detail::rdma_faa_awaitable<Backend>(
        qp_, /*backend=*/nullptr, local, remote, add, flags);
}

// --- ATOMIC: connection<polymorphic_backend> ---

inline detail::rdma_cas_awaitable<polymorphic_backend>
connection<polymorphic_backend>::cas(buffer_view local, remote_buffer remote,
                                     std::uint64_t compare, std::uint64_t swap,
                                     send_flags flags) noexcept {
    return detail::rdma_cas_awaitable<polymorphic_backend>(
        qp_, backend_, local, remote, compare, swap, flags);
}

inline detail::rdma_faa_awaitable<polymorphic_backend>
connection<polymorphic_backend>::fetch_add(buffer_view local,
                                           remote_buffer remote,
                                           std::uint64_t add,
                                           send_flags flags) noexcept {
    return detail::rdma_faa_awaitable<polymorphic_backend>(
        qp_, backend_, local, remote, add, flags);
}

}  // namespace elio::rdma
