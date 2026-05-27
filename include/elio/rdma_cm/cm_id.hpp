#pragma once

/// @file cm_id.hpp
/// @brief RAII wrapper around `rdma_cm_id*`.
///
/// `rdma_cm_id` represents a single CM endpoint — listener,
/// connecting client, or established connection. The wrapper owns
/// one id, calls `rdma_destroy_id` on destruction, and forwards
/// the few accessors callers commonly need.

#include <rdma/rdma_cma.h>

#include <utility>

namespace elio::rdma_cm {

class cm_id {
public:
    cm_id() noexcept = default;

    /// Takes ownership of `id` (may be null).
    explicit cm_id(rdma_cm_id* id) noexcept : id_(id) {}

    cm_id(const cm_id&) = delete;
    cm_id& operator=(const cm_id&) = delete;

    cm_id(cm_id&& other) noexcept : id_(other.id_) {
        other.id_ = nullptr;
    }

    cm_id& operator=(cm_id&& other) noexcept {
        if (this != &other) {
            destroy_();
            id_ = other.id_;
            other.id_ = nullptr;
        }
        return *this;
    }

    ~cm_id() noexcept { destroy_(); }

    [[nodiscard]] rdma_cm_id* native() const noexcept { return id_; }

    /// QP attached to this CM id by `rdma_create_qp` (or
    /// `rdma_accept` on the listener side). May be null if no QP
    /// has been created yet.
    [[nodiscard]] ibv_qp* qp() const noexcept {
        return id_ ? id_->qp : nullptr;
    }
    [[nodiscard]] ibv_pd* pd() const noexcept {
        return id_ ? id_->pd : nullptr;
    }
    [[nodiscard]] ibv_context* verbs() const noexcept {
        return id_ ? id_->verbs : nullptr;
    }

    [[nodiscard]] explicit operator bool() const noexcept {
        return id_ != nullptr;
    }

    /// Relinquish ownership without destroying. Caller is
    /// responsible for the eventual `rdma_destroy_id`.
    rdma_cm_id* release() noexcept {
        auto* tmp = id_;
        id_ = nullptr;
        return tmp;
    }

private:
    void destroy_() noexcept {
        if (id_) {
            // rdma_destroy_id closes the QP if one is attached;
            // applications that need finer control should release()
            // first.
            (void)::rdma_destroy_id(id_);
            id_ = nullptr;
        }
    }

    rdma_cm_id* id_ = nullptr;
};

}  // namespace elio::rdma_cm
