#pragma once

/// @file cm_listener.hpp
/// @brief Server-side CM listener.
///
/// Wraps the librdmacm listen / accept handshake:
///   1. `rdma_create_id` (RDMA_PS_TCP)
///   2. `rdma_bind_addr` to the supplied sockaddr
///   3. `rdma_listen(backlog)`
///   4. on each accept call: wait for `RDMA_CM_EVENT_CONNECT_REQUEST`,
///      hand the new id to the caller. The caller is responsible for
///      `rdma_create_qp(new_id, ...)` and `accept_connect(new_id, ...)`
///      to finalise the handshake.
///
/// Splitting the connect-request consumption from QP creation
/// matches the spec's protocol-agnostic principle: callers control
/// the QP init attributes and pd. `accept_connect` then issues
/// `rdma_accept` and waits for `RDMA_CM_EVENT_ESTABLISHED`.

#include <elio/coro/cancel_token.hpp>
#include <elio/coro/task.hpp>
#include <elio/rdma_cm/cm_connect.hpp>  // for cm_status
#include <elio/rdma_cm/cm_id.hpp>
#include <elio/rdma_cm/event_channel.hpp>

#include <rdma/rdma_cma.h>
#include <sys/socket.h>

#include <cerrno>
#include <cstring>
#include <stdexcept>
#include <string>
#include <utility>

namespace elio::rdma_cm {

class cm_listener {
public:
    /// Bind and listen on `addr`. `backlog` defaults to 128 to match
    /// librdmacm's typical default. Throws on librdmacm failure;
    /// most users want to catch and fail the application up-front.
    cm_listener(event_channel& ch, const struct sockaddr* addr,
                socklen_t addr_len, int backlog = 128,
                rdma_port_space port_space = RDMA_PS_TCP)
        : channel_(&ch) {
        rdma_cm_id* raw = nullptr;
        if (::rdma_create_id(ch.native(), &raw, /*context=*/nullptr,
                             port_space) != 0) {
            const int e = errno;
            throw std::runtime_error(
                std::string("rdma_create_id failed: ")
                + std::strerror(e));
        }
        listen_id_ = cm_id{raw};

        (void)addr_len;  // librdmacm reads the family from the sockaddr
        if (::rdma_bind_addr(listen_id_.native(),
                             const_cast<struct sockaddr*>(addr)) != 0) {
            const int e = errno;
            throw std::runtime_error(
                std::string("rdma_bind_addr failed: ")
                + std::strerror(e));
        }
        if (::rdma_listen(listen_id_.native(), backlog) != 0) {
            const int e = errno;
            throw std::runtime_error(
                std::string("rdma_listen failed: ")
                + std::strerror(e));
        }
    }

    cm_listener(const cm_listener&) = delete;
    cm_listener& operator=(const cm_listener&) = delete;

    cm_listener(cm_listener&& other) noexcept
        : channel_(std::exchange(other.channel_, nullptr)),
          listen_id_(std::move(other.listen_id_)) {}

    cm_listener& operator=(cm_listener&& other) noexcept {
        if (this != &other) {
            listen_id_ = std::move(other.listen_id_);
            channel_   = std::exchange(other.channel_, nullptr);
        }
        return *this;
    }

    ~cm_listener() = default;

    [[nodiscard]] rdma_cm_id* native() const noexcept {
        return listen_id_.native();
    }

    /// Wait for the next connect request on this listener. On
    /// success returns a new `cm_id` representing the client
    /// connection; the caller is responsible for creating its QP
    /// and calling `accept_connect()` to finalise.
    ///
    /// On cancellation or error returns a null cm_id; populate
    /// `out_status` to learn why.
    [[nodiscard]] coro::task<cm_id> accept(
        cm_status* out_status = nullptr,
        coro::cancel_token token = {}) {
        if (!channel_) {
            if (out_status) {
                out_status->status = detail::make_cm_status(EINVAL).status;
            }
            co_return cm_id{};
        }
        while (!token.is_cancelled()) {
            rdma_cm_event* event = co_await channel_->next_event(token);
            if (!event) {
                if (out_status) {
                    out_status->status =
                        detail::make_cm_status(ECANCELED).status;
                }
                co_return cm_id{};
            }
            const auto type = event->event;
            rdma_cm_id* new_id = event->id;
            const int status = event->status;
            channel_->ack(event);

            if (type == RDMA_CM_EVENT_CONNECT_REQUEST) {
                if (out_status) out_status->status = 0;
                co_return cm_id{new_id};
            }
            // Other event types on the listener channel (e.g.
            // DEVICE_REMOVAL) are swallowed; the application can
            // re-poll. Errors leave the listener usable as long as
            // the channel survives.
            if (status && out_status) {
                out_status->status =
                    detail::make_cm_status(status).status;
            }
        }
        if (out_status) {
            out_status->status = detail::make_cm_status(ECANCELED).status;
        }
        co_return cm_id{};
    }

private:
    event_channel* channel_ = nullptr;
    cm_id          listen_id_{};
};

/// Server-side counterpart to `complete_connect()`: after the
/// application has created a QP on the request-id, accept the
/// pending connect and wait for ESTABLISHED.
inline coro::task<cm_status> accept_connect(
    event_channel& ch, cm_id& id,
    const rdma_conn_param* conn_param = nullptr,
    coro::cancel_token token = {}) {
    if (auto st = detail::cm_id_status(id); !st.ok()) co_return st;

    if (::rdma_accept(id.native(),
                      const_cast<rdma_conn_param*>(conn_param)) != 0) {
        co_return detail::make_cm_status(errno);
    }
    rdma_cm_event* event = co_await ch.next_event(token);
    if (!event) co_return detail::make_cm_status(ECANCELED);
    const auto type   = event->event;
    const auto status = event->status;
    ch.ack(event);
    if (type != RDMA_CM_EVENT_ESTABLISHED) {
        co_return detail::make_cm_status(status ? status : EPROTO);
    }
    co_return cm_status{0};
}

}  // namespace elio::rdma_cm
