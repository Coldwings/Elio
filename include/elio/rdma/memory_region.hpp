#pragma once

/// @file memory_region.hpp
/// @brief RAII wrapper around a backend-registered MR (stage S6).
///
/// `memory_region<Backend>` registers a buffer on construction and
/// deregisters it on destruction. It owns one MR; copies are
/// disabled, moves transfer ownership. The wrapper exposes:
///
///   * `view()` / `view(offset, length)` — `buffer_view` with the
///     correct `lkey` filled in, ready to pass into `connection::send`
///     / `recv` / `rdma_write` (local side) / `rdma_read` (local
///     side).
///   * `remote()` / `remote(offset, length)` — `remote_buffer`
///     suitable to advertise to the peer for one-sided RDMA ops the
///     peer initiates against this MR.
///   * `native()` — the opaque MR handle returned by
///     `Backend::register_mr` (typically `ibv_mr*`).
///
/// **Availability**: requires `backend_with_mr<Backend>` for the
/// primary template. The polymorphic specialisation works against
/// any `polymorphic_backend`; if the user's subclass doesn't
/// override `register_mr`, construction yields a `memory_region`
/// whose `ok()` returns false (no UB).
///
/// **Lifetime contract**: the underlying `addr` buffer must outlive
/// the `memory_region`. The MR registration pins the pages, but the
/// allocation itself is the user's. The protection domain (`pd`)
/// argument must also outlive the MR.

#include <elio/rdma/backend_traits.hpp>
#include <elio/rdma/types.hpp>

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <utility>

namespace elio::rdma {

/// Primary template — static-traits backend. Constrained on
/// `backend_with_mr<Backend>` at the constructor; users who depend on
/// MR support get a clean error if the backend lacks the hooks.
template <typename Backend>
class memory_region {
public:
    memory_region() noexcept requires backend_with_mr<Backend> = default;

    memory_region(void* pd, void* addr, std::size_t length,
                  int access) noexcept
        requires backend_with_mr<Backend>
        : mr_(Backend::register_mr(pd, addr, length, access)),
          addr_(addr), length_(length) {}

    memory_region(const memory_region&) = delete;
    memory_region& operator=(const memory_region&) = delete;

    memory_region(memory_region&& other) noexcept
        : mr_(other.mr_), addr_(other.addr_), length_(other.length_) {
        other.mr_     = nullptr;
        other.addr_   = nullptr;
        other.length_ = 0;
    }

    memory_region& operator=(memory_region&& other) noexcept {
        if (this != &other) {
            if (mr_) Backend::dereg_mr(mr_);
            mr_     = other.mr_;
            addr_   = other.addr_;
            length_ = other.length_;
            other.mr_     = nullptr;
            other.addr_   = nullptr;
            other.length_ = 0;
        }
        return *this;
    }

    ~memory_region() noexcept {
        if (mr_) Backend::dereg_mr(mr_);
    }

    [[nodiscard]] bool ok() const noexcept { return mr_ != nullptr; }
    [[nodiscard]] void* native() const noexcept { return mr_; }
    [[nodiscard]] void* addr() const noexcept { return addr_; }
    [[nodiscard]] std::size_t length() const noexcept { return length_; }

    [[nodiscard]] std::uint32_t lkey() const noexcept {
        return Backend::lkey_of(mr_);
    }
    [[nodiscard]] std::uint32_t rkey() const noexcept {
        return Backend::rkey_of(mr_);
    }

    /// `buffer_view` over the entire registered region.
    [[nodiscard]] buffer_view view() const noexcept {
        return buffer_view{addr_, length_, lkey()};
    }

    /// Sub-range `buffer_view`. `[offset, offset + sub_length)` must
    /// lie within `[0, length_)`. Bounds-checking is the caller's
    /// responsibility — this is a verbs-adjacent fast path.
    [[nodiscard]] buffer_view view(std::size_t offset,
                                   std::size_t sub_length) const noexcept {
        return buffer_view{static_cast<char*>(addr_) + offset,
                           sub_length, lkey()};
    }

    /// `remote_buffer` describing the entire MR for peer one-sided ops.
    [[nodiscard]] remote_buffer remote() const noexcept {
        assert(length_ <= std::uint32_t(-1) &&
               "MR length exceeds uint32_t range for remote_buffer");
        return remote_buffer{
            .addr = reinterpret_cast<std::uint64_t>(addr_),
            .length = static_cast<std::uint32_t>(length_),
            .rkey = rkey(),
        };
    }

    /// Sub-range `remote_buffer`.
    [[nodiscard]] remote_buffer remote(std::size_t offset,
                                       std::size_t sub_length) const noexcept {
        assert(sub_length <= std::uint32_t(-1) &&
               "sub_length exceeds uint32_t range for remote_buffer");
        return remote_buffer{
            .addr = reinterpret_cast<std::uint64_t>(addr_) + offset,
            .length = static_cast<std::uint32_t>(sub_length),
            .rkey = rkey(),
        };
    }

private:
    void*       mr_     = nullptr;
    void*       addr_   = nullptr;
    std::size_t length_ = 0;
};

/// Specialisation for the polymorphic backend. Holds an instance
/// pointer so the destructor can dispatch through the vtable. The
/// instance must outlive the memory_region.
template <>
class memory_region<polymorphic_backend> {
public:
    memory_region(polymorphic_backend& backend, void* pd, void* addr,
                  std::size_t length, int access) noexcept
        : backend_(&backend),
          mr_(backend.register_mr(pd, addr, length, access)),
          addr_(addr), length_(length) {}

    memory_region(const memory_region&) = delete;
    memory_region& operator=(const memory_region&) = delete;

    memory_region(memory_region&& other) noexcept
        : backend_(std::exchange(other.backend_, nullptr)),
          mr_(std::exchange(other.mr_, nullptr)),
          addr_(std::exchange(other.addr_, nullptr)),
          length_(std::exchange(other.length_, 0)) {}

    memory_region& operator=(memory_region&& other) noexcept {
        if (this != &other) {
            if (mr_ && backend_) backend_->dereg_mr(mr_);
            backend_ = std::exchange(other.backend_, nullptr);
            mr_      = std::exchange(other.mr_, nullptr);
            addr_    = std::exchange(other.addr_, nullptr);
            length_  = std::exchange(other.length_, 0);
        }
        return *this;
    }

    ~memory_region() noexcept {
        if (mr_ && backend_) backend_->dereg_mr(mr_);
    }

    [[nodiscard]] bool ok() const noexcept { return mr_ != nullptr; }
    [[nodiscard]] void* native() const noexcept { return mr_; }
    [[nodiscard]] void* addr() const noexcept { return addr_; }
    [[nodiscard]] std::size_t length() const noexcept { return length_; }

    [[nodiscard]] std::uint32_t lkey() const noexcept {
        return backend_ ? backend_->lkey_of(mr_) : 0;
    }
    [[nodiscard]] std::uint32_t rkey() const noexcept {
        return backend_ ? backend_->rkey_of(mr_) : 0;
    }

    [[nodiscard]] buffer_view view() const noexcept {
        return buffer_view{addr_, length_, lkey()};
    }
    [[nodiscard]] buffer_view view(std::size_t offset,
                                   std::size_t sub_length) const noexcept {
        return buffer_view{static_cast<char*>(addr_) + offset,
                           sub_length, lkey()};
    }

    [[nodiscard]] remote_buffer remote() const noexcept {
        assert(length_ <= std::uint32_t(-1) &&
               "MR length exceeds uint32_t range for remote_buffer");
        return remote_buffer{
            .addr = reinterpret_cast<std::uint64_t>(addr_),
            .length = static_cast<std::uint32_t>(length_),
            .rkey = rkey(),
        };
    }
    [[nodiscard]] remote_buffer remote(std::size_t offset,
                                       std::size_t sub_length) const noexcept {
        assert(sub_length <= std::uint32_t(-1) &&
               "sub_length exceeds uint32_t range for remote_buffer");
        return remote_buffer{
            .addr = reinterpret_cast<std::uint64_t>(addr_) + offset,
            .length = static_cast<std::uint32_t>(sub_length),
            .rkey = rkey(),
        };
    }

private:
    polymorphic_backend* backend_ = nullptr;
    void*                mr_      = nullptr;
    void*                addr_    = nullptr;
    std::size_t          length_  = 0;
};

}  // namespace elio::rdma
