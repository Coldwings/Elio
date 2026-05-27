#pragma once

/// @file cm_connect.hpp
/// @brief Client-side CM handshake helper.
///
/// `connect(...)` drives the four-step librdmacm sequence:
///   1. `rdma_create_id`
///   2. `rdma_resolve_addr` → wait for `RDMA_CM_EVENT_ADDR_RESOLVED`
///   3. `rdma_resolve_route` → wait for `RDMA_CM_EVENT_ROUTE_RESOLVED`
///   4. caller creates QP (`rdma_create_qp`) — *not* done here so
///      the application controls `qp_init_attr` and `pd`
///   5. `rdma_connect` → wait for `RDMA_CM_EVENT_ESTABLISHED`
///
/// Steps 1–3 are handled by `resolve()`; the application then
/// creates the QP and calls `complete_connect()`. Pulling QP setup
/// out of the helper keeps the abstraction protocol-agnostic — the
/// user owns the verbs surface as designed in Elio's RDMA layer.
///
/// Each helper takes an `event_channel&` whose ownership remains
/// with the caller. All returned `cm_id`s are derived from that
/// channel and must be destroyed before the channel.

#include <elio/coro/cancel_token.hpp>
#include <elio/coro/task.hpp>
#include <elio/rdma_cm/cm_id.hpp>
#include <elio/rdma_cm/event_channel.hpp>

#include <rdma/rdma_cma.h>
#include <sys/socket.h>

#include <cerrno>
#include <cstring>

namespace elio::rdma_cm {

/// Options affecting the resolve / connect phases. All are
/// pass-through to librdmacm.
struct connect_options {
    const struct sockaddr* src        = nullptr;  ///< optional source bind
    int                    timeout_ms = 2000;     ///< for resolve_*
    rdma_port_space        port_space = RDMA_PS_TCP;
};

/// Result of a single CM step. status==0 on success; otherwise -errno
/// from rdma_resolve_* or a synthesised value (e.g. ECANCELED if the
/// token fired, ETIMEDOUT on event mismatch).
struct cm_status {
    int status = 0;
    [[nodiscard]] bool ok() const noexcept { return status == 0; }
};

namespace detail {

inline cm_status pump_until_(event_channel& ch,
                             coro::cancel_token token,
                             rdma_cm_event_type expected,
                             coro::task<cm_status>& /*self*/) noexcept {
    // Placeholder — the real pump is inline in resolve(); kept here
    // as a marker for future refactoring.
    (void)ch; (void)token; (void)expected;
    return cm_status{ECANCELED};
}

}  // namespace detail

/// Step 1+2+3: create a cm_id and resolve the destination address +
/// route. Returns the cm_id on success; populates `out_status` with
/// the underlying errno on failure (the returned cm_id is then null).
inline coro::task<cm_id> resolve(event_channel& ch,
                                 const struct sockaddr* dst,
                                 socklen_t dst_len,
                                 connect_options opts = {},
                                 cm_status* out_status = nullptr,
                                 coro::cancel_token token = {}) {
    auto fail = [&](int err) -> cm_id {
        if (out_status) out_status->status = err;
        return cm_id{};
    };

    rdma_cm_id* raw = nullptr;
    if (::rdma_create_id(ch.native(), &raw, /*context=*/nullptr,
                         opts.port_space) != 0) {
        co_return fail(errno);
    }
    cm_id id{raw};

    (void)dst_len;  // librdmacm reads the family from the sockaddr
    if (::rdma_resolve_addr(id.native(),
                            const_cast<struct sockaddr*>(opts.src),
                            const_cast<struct sockaddr*>(dst),
                            opts.timeout_ms) != 0) {
        co_return fail(errno);
    }

    // Wait for ADDR_RESOLVED.
    {
        rdma_cm_event* event = co_await ch.next_event(token);
        if (!event) co_return fail(ECANCELED);
        const auto type   = event->event;
        const auto status = event->status;
        ch.ack(event);
        if (type != RDMA_CM_EVENT_ADDR_RESOLVED) {
            co_return fail(status ? status : EPROTO);
        }
    }

    if (::rdma_resolve_route(id.native(), opts.timeout_ms) != 0) {
        co_return fail(errno);
    }
    {
        rdma_cm_event* event = co_await ch.next_event(token);
        if (!event) co_return fail(ECANCELED);
        const auto type   = event->event;
        const auto status = event->status;
        ch.ack(event);
        if (type != RDMA_CM_EVENT_ROUTE_RESOLVED) {
            co_return fail(status ? status : EPROTO);
        }
    }

    if (out_status) out_status->status = 0;
    co_return id;
}

/// Step 5: after the application has called `rdma_create_qp` on the
/// id, issue `rdma_connect` and wait for ESTABLISHED. `conn_param`
/// is forwarded verbatim (pass nullptr for librdmacm defaults).
inline coro::task<cm_status> complete_connect(
    event_channel& ch, cm_id& id,
    const rdma_conn_param* conn_param = nullptr,
    coro::cancel_token token = {}) {
    if (!id) co_return cm_status{EINVAL};

    if (::rdma_connect(id.native(),
                       const_cast<rdma_conn_param*>(conn_param)) != 0) {
        co_return cm_status{errno};
    }

    rdma_cm_event* event = co_await ch.next_event(token);
    if (!event) co_return cm_status{ECANCELED};
    const auto type   = event->event;
    const auto status = event->status;
    ch.ack(event);
    if (type != RDMA_CM_EVENT_ESTABLISHED) {
        co_return cm_status{status ? status : EPROTO};
    }
    co_return cm_status{0};
}

}  // namespace elio::rdma_cm
