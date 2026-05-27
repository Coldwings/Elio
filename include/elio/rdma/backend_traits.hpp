#pragma once

/// @file backend_traits.hpp
/// @brief Backend hook surface — template traits + polymorphic class.
///
/// Two coexisting injection mechanisms:
///
///   1. **Template traits** (`elio::rdma::backend_traits` concept).
///      Zero-overhead static dispatch via `connection<Backend>`. Every
///      `Backend` argument is a type that exposes static `post_*` member
///      functions. The compiler inlines through.
///
///   2. **Pure-virtual class** (`polymorphic_backend`). Runtime-replaceable
///      backend. `connection<polymorphic_backend>` (defined in
///      connection.hpp, stage S3+) is a specialisation that dispatches via
///      the vtable, so a single binary can mix multiple backends.
///
/// Both mechanisms accept the same operation set; the polymorphic class
/// is literally just a virtual mirror of the concept.
///
/// **Operation contract**: every `post_*` must be `noexcept` and return
/// an `int` that is 0 on success or negative on failure. Negative values
/// follow the POSIX convention: `-EAGAIN` for "send queue full",
/// `-ENOMEM` for inline buffer overflow, etc. The dispatcher does not
/// inspect the value; the awaiter (S3+) propagates it via `wc_result`
/// on a synthetic completion when posting itself failed.
///
/// **Optional extensions** are split into separate concepts so a minimal
/// backend (e.g. tests) need not implement them:
///   - `backend_with_mr<B>`  — adds `register_mr` / `dereg_mr` for the
///                              optional `memory_region<B>` RAII (S6).
///   - `backend_with_srq<B>` — adds `post_srq_recv` for shared receive
///                              queues (S5c).

#include <elio/rdma/types.hpp>

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <span>
#include <type_traits>

namespace elio::rdma {

/// Static-traits concept: any `Backend` type that provides the four
/// data-path operations via static member functions. `connection<B>`
/// will require `backend_traits<B>` for non-polymorphic instantiations.
template <typename B>
concept backend_traits = requires(void* qp,
                                  std::span<const sge> sges,
                                  remote_buffer rb,
                                  send_flags flags,
                                  std::uint32_t imm_data,
                                  wr_id id) {
    { B::post_send(qp, sges, flags, imm_data, id) } noexcept
        -> std::same_as<int>;
    { B::post_recv(qp, sges, id) } noexcept -> std::same_as<int>;
    { B::post_rdma_write(qp, sges, rb, flags, imm_data, id) } noexcept
        -> std::same_as<int>;
    { B::post_rdma_read(qp, sges, rb, id) } noexcept
        -> std::same_as<int>;
};

/// Optional extension: backend supports memory-region registration.
/// `memory_region<B>` (S6) requires this. The four members map 1:1 to
/// the underlying ibverbs surface:
///   * `register_mr(pd, addr, len, access) -> void*` — allocates the
///     MR; backend chooses the representation (typically `ibv_mr*`).
///   * `dereg_mr(void*)` — releases the MR.
///   * `lkey_of(void*) -> uint32_t` — local key, used in SGEs the
///     local side posts.
///   * `rkey_of(void*) -> uint32_t` — remote key, advertised to the
///     peer for one-sided RDMA WRITE / READ targets.
template <typename B>
concept backend_with_mr = backend_traits<B> &&
    requires(void* pd, void* addr, std::size_t length, int access, void* mr) {
        { B::register_mr(pd, addr, length, access) } noexcept
            -> std::convertible_to<void*>;
        { B::dereg_mr(mr) } noexcept;
        { B::lkey_of(mr) } noexcept -> std::convertible_to<std::uint32_t>;
        { B::rkey_of(mr) } noexcept -> std::convertible_to<std::uint32_t>;
    };

/// Optional extension: backend supports shared receive queues.
template <typename B>
concept backend_with_srq = backend_traits<B> &&
    requires(void* srq, std::span<const sge> sges, wr_id id) {
        { B::post_srq_recv(srq, sges, id) } noexcept -> std::same_as<int>;
    };

/// Runtime-replaceable backend. Users derive from this and pass an
/// instance (typically a long-lived singleton) to
/// `connection<polymorphic_backend>` constructors.
///
/// The optional extension methods (`register_mr`, `dereg_mr`,
/// `post_srq_recv`) have safe no-op defaults that return error codes
/// indicating "not supported", so a derived class can leave them
/// unimplemented if the corresponding feature is not needed.
///
/// The dtor is virtual; expected lifetime: at least as long as any
/// connection holding a reference. Most users hold the polymorphic
/// backend in a long-lived `std::unique_ptr` owned by the application.
struct polymorphic_backend {
    polymorphic_backend() noexcept = default;
    virtual ~polymorphic_backend() = default;

    polymorphic_backend(const polymorphic_backend&) = delete;
    polymorphic_backend& operator=(const polymorphic_backend&) = delete;
    polymorphic_backend(polymorphic_backend&&) = delete;
    polymorphic_backend& operator=(polymorphic_backend&&) = delete;

    // Data path: required. The `imm_data` parameter is meaningful
    // only when `flags.with_imm` is set; backends should ignore it
    // otherwise and select the non-IMM opcode.
    virtual int post_send(void* qp,
                          std::span<const sge> sges,
                          send_flags flags,
                          std::uint32_t imm_data,
                          wr_id id) noexcept = 0;
    virtual int post_recv(void* qp,
                          std::span<const sge> sges,
                          wr_id id) noexcept = 0;
    virtual int post_rdma_write(void* qp,
                                std::span<const sge> sges,
                                remote_buffer rb,
                                send_flags flags,
                                std::uint32_t imm_data,
                                wr_id id) noexcept = 0;
    virtual int post_rdma_read(void* qp,
                               std::span<const sge> sges,
                               remote_buffer rb,
                               wr_id id) noexcept = 0;

    // Optional: MR management. Default = "not supported".
    //
    // `register_mr` returning nullptr is the sentinel for backends
    // that don't support MRs; `lkey_of` / `rkey_of` return 0 by
    // default so memory_region's accessors don't UB on a null MR.
    virtual void* register_mr(void* /*pd*/, void* /*addr*/,
                              std::size_t /*length*/,
                              int /*access*/) noexcept {
        return nullptr;
    }
    virtual void dereg_mr(void* /*mr*/) noexcept {}
    virtual std::uint32_t lkey_of(void* /*mr*/) noexcept { return 0; }
    virtual std::uint32_t rkey_of(void* /*mr*/) noexcept { return 0; }

    // Optional: SRQ. Default = "not supported" (-ENOTSUP).
    virtual int post_srq_recv(void* /*srq*/,
                              std::span<const sge> /*sges*/,
                              wr_id /*id*/) noexcept {
        return -95;  // -ENOTSUP. <errno.h> avoided to keep this header light.
    }
};

/// A trivial wrapper that lets `polymorphic_backend*` satisfy the
/// `backend_traits` static-dispatch surface by forwarding to virtual
/// calls. `connection<polymorphic_backend>` (S3+) uses a specialisation
/// instead of this adapter — the adapter is provided mainly for
/// uniformity in generic test fixtures.
struct polymorphic_traits_adapter {
    polymorphic_backend* impl = nullptr;

    int post_send(void* qp, std::span<const sge> sges,
                  send_flags flags, std::uint32_t imm_data,
                  wr_id id) const noexcept {
        return impl->post_send(qp, sges, flags, imm_data, id);
    }
    int post_recv(void* qp, std::span<const sge> sges,
                  wr_id id) const noexcept {
        return impl->post_recv(qp, sges, id);
    }
    int post_rdma_write(void* qp, std::span<const sge> sges,
                        remote_buffer rb, send_flags flags,
                        std::uint32_t imm_data, wr_id id) const noexcept {
        return impl->post_rdma_write(qp, sges, rb, flags, imm_data, id);
    }
    int post_rdma_read(void* qp, std::span<const sge> sges,
                       remote_buffer rb, wr_id id) const noexcept {
        return impl->post_rdma_read(qp, sges, rb, id);
    }
};

}  // namespace elio::rdma
