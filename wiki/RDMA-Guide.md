# RDMA Guide

Elio ships an **optional** RDMA-Core abstraction layer that lets you
issue RDMA verbs in coroutine-friendly synchronous-looking style
without coupling the library to `libibverbs` or `librdmacm`. You
supply the backend (a small set of `post_*` functions that map onto
your real verbs library), Elio supplies the awaiter machinery,
lifecycle, scatter/gather, inline, SRQ, MR RAII, and CQ pump.

## When to enable

Pass `-DELIO_ENABLE_RDMA=ON` to CMake. The umbrella header is
`<elio/rdma/rdma.hpp>` and the CMake target is `elio_rdma` — both
header-only, no extra runtime dependencies. The preprocessor macro
`ELIO_HAS_RDMA` is defined when the module is enabled.

If you additionally want the optional `librdmacm` CM bootstrap
helper, also pass `-DELIO_ENABLE_RDMA_CM=ON`. That builds the
`elio_rdma_cm` target, which links `librdmacm`. The macro
`ELIO_HAS_RDMA_CM` is defined when that target is enabled.

```cmake
target_link_libraries(my_app PRIVATE elio_rdma)            # core data path
target_link_libraries(my_app PRIVATE elio_rdma_cm)         # + CM helper
```

## The two backend interfaces

The data path is parameterised on a `Backend` type. You pick one of
two ways to inject yours:

### Static traits (zero overhead)

Any type that satisfies the `elio::rdma::backend_traits` C++20
concept works:

```cpp
struct my_backend {
    static int post_send(void* qp, std::span<const elio::rdma::sge> sges,
                         elio::rdma::send_flags flags,
                         std::uint32_t imm_data,
                         elio::rdma::wr_id id) noexcept {
        // ... call ibv_post_send on (ibv_qp*)qp ...
    }
    static int post_recv(void* qp, std::span<const elio::rdma::sge> sges,
                         elio::rdma::wr_id id) noexcept { /* ibv_post_recv */ }
    static int post_rdma_write(void* qp, std::span<const elio::rdma::sge> sges,
                               elio::rdma::remote_buffer rb,
                               elio::rdma::send_flags flags,
                               std::uint32_t imm_data,
                               elio::rdma::wr_id id) noexcept { /* ... */ }
    static int post_rdma_read(void* qp, std::span<const elio::rdma::sge> sges,
                              elio::rdma::remote_buffer rb,
                              elio::rdma::wr_id id) noexcept { /* ... */ }
};
```

Each method returns `0` on success or a negative errno on failure.
The awaiter surfaces failures back through `wc_result::status =
wr_flush_error` with `imm_data` carrying the offending errno.

`connection<my_backend>` dispatches through these calls. The compiler
inlines through; no vtable, no branch.

### Polymorphic backend (runtime replaceable)

Derive from `elio::rdma::polymorphic_backend` to support backend
switching at runtime or to mix multiple backends in one process:

```cpp
struct my_poly_backend : elio::rdma::polymorphic_backend {
    int post_send(void* qp, std::span<const elio::rdma::sge>,
                  elio::rdma::send_flags, std::uint32_t,
                  elio::rdma::wr_id) noexcept override;
    int post_recv(...) override;
    int post_rdma_write(void* qp, std::span<const elio::rdma::sge>,
                        elio::rdma::remote_buffer,
                        elio::rdma::send_flags, std::uint32_t,
                        elio::rdma::wr_id) noexcept override;
    int post_rdma_read(...) override;
    // Optional: override post_srq_recv, register_mr, dereg_mr,
    // lkey_of, rkey_of. Defaults return -ENOTSUP / nullptr / 0.
};

elio::rdma::connection<elio::rdma::polymorphic_backend> conn{
    qp_ptr, my_backend_instance, dispatcher};
```

## Data-path API surface

`connection<Backend>` exposes:

```cpp
co_await conn.send(buffer_view buf, send_flags flags = {});
co_await conn.send(std::span<const sge> sges, send_flags flags = {});
co_await conn.recv(buffer_view buf);
co_await conn.recv(std::span<const sge> sges);
co_await conn.rdma_write(buffer_view local, remote_buffer remote,
                         send_flags flags = {});
co_await conn.rdma_write(std::span<const sge> locals, remote_buffer remote,
                         send_flags flags = {});
co_await conn.rdma_read(buffer_view local, remote_buffer remote);
co_await conn.rdma_read(std::span<const sge> locals, remote_buffer remote);
```

Every `co_await` returns `wc_result`:

```cpp
struct wc_result {
    wc_status     status;     // success or normalized failure code
    std::uint32_t byte_len;   // bytes transferred (RECV / RDMA READ)
    std::uint32_t imm_data;   // immediate value (RECV with IMM)
    std::uint32_t wc_flags;   // backend flag bits (e.g. WITH_IMM)

    bool ok() const noexcept;
};
```

**Errors are returned, not thrown** — same shape as Elio's
`io_result` / `cancel_result`.

Constructing one of these operations is lazy: `auto op = conn.recv(buf);`
does not post a work request by itself. The WR is posted when the awaiter is
`co_await`ed. When receive preposting or a real send/read/write pipeline is
required, call `.start()` explicitly:

```cpp
auto recv_op = conn.recv(buffer).start();  // posts immediately

// ...issue peer-visible work...

auto wc = co_await std::move(recv_op);     // waits for the already-posted WR
```

`start()` returns the same move-only awaiter type, so started operations can be
stored in a queue and awaited later. Synchronous post failures and completions
that arrive before the later `co_await` are stored and returned by that await.
For `std::span<const sge>` overloads, the span array must outlive the operation
that posts the WR: either the `co_await` for lazy operations or `.start()` for
eager operations. The underlying payload buffers must outlive the hardware
operation itself.

`buffer_view::length` is `std::size_t`, but one verbs SGE can only carry a
32-bit length. The single-buffer `buffer_view` convenience overloads fail
before posting with `wc_status::local_length_error` when the length is above
`UINT32_MAX`. Split larger local transfers into an explicit
`std::span<const sge>` scatter list. Low-level helpers such as `sge::from()`
remain verbs-adjacent conversions and require callers to pass a representable
length.

### Shared receive queues

```cpp
elio::rdma::srq<my_backend> recv_pool{srq_ptr, dispatcher};
co_await recv_pool.recv(buffer_view{...});
```

Backends that want SRQ support add `static int post_srq_recv(...)`;
the `backend_with_srq` concept gates the API. The polymorphic
backend has a default that returns `-ENOTSUP`.

### SEND / RDMA WRITE with IMM

When the peer needs an in-band 32-bit signal alongside the payload
(or, as in the typical OOB-completion pattern, when an RDMA_WRITE
finishes and the writer needs to tell the reader "go look at your
buffer"), use the `*_with_imm` variants:

```cpp
// Client side: post recv for the OOB notify first.
auto notify_awaiter = conn.recv(notify_mr.view()).start();

// ...peer does an RDMA_WRITE...

// Then peer SENDs a zero-length frame with imm = payload size.
co_await conn.send_with_imm(notify_mr.view(0, 0), payload_len);

// Notify side observes the imm in wc_result.imm_data.
auto wc = co_await std::move(notify_awaiter);
auto written = wc.imm_data;
```

`rdma_write_with_imm` is the same shape; the IMM lands on the
peer's RECV CQE on whatever QP it has bound for the WRITE target.
`send_flags::with_imm` is set automatically by these methods; you
can still pass an explicit `send_flags` value to mix in `solicited`
/ `fence` / etc.

### 8-byte ATOMIC ops (CAS / FAA)

Two hardware-atomic operations against a remote 8-byte location:

```cpp
// compare-and-swap: if remote == compare, write swap. The OLD remote
// value lands in `local` (8 raw big-endian bytes).
auto r = co_await conn.cas(local_mr.view(), remote,
                           /*compare=*/expected, /*swap=*/desired);
if (r.ok() && r.old_value_host() == expected) {
    // CAS succeeded — remote now holds `desired`.
}

// fetch-and-add: atomically remote += add; OLD remote value lands in local.
auto r = co_await conn.fetch_add(local_mr.view(), remote, /*add=*/1);
auto previous = r.old_value_host();
```

Returned by `atomic_result`:

```cpp
struct atomic_result {
    wc_result   wc;     // standard CQE fields
    buffer_view local;  // where the old 8 bytes landed
    bool ok() const noexcept;
    std::uint64_t old_value_host() const noexcept;  // be64toh of the buffer
    std::uint64_t old_value_raw()  const noexcept;  // raw 8 bytes (be wire fmt)
};
```

IB delivers the old value big-endian. `old_value_host()` does the
conversion; `old_value_raw()` returns the raw bytes verbatim for
backends that don't follow the canonical byte order (mlx5's
`IBV_EXP_ATOMIC_HCA_REPLY_BE`, vendor-specific reply ordering).

Constraints:
- The local `buffer_view` must point at exactly 8 bytes; Elio validates this
  before posting and returns `wc_status::local_length_error` otherwise.
- Remote `addr` must be 8-byte aligned.
- Only valid on RC QPs; UD has no atomics.

Backend opt-in: implement the static `post_atomic_cas` /
`post_atomic_fetch_add` methods to satisfy the
`backend_with_atomic<Backend>` concept (compile-time gate at the
call site of `cas` / `fetch_add`). For `polymorphic_backend`,
override the two virtuals; not overriding them is fine — calls
yield `wr_flush_error` with `imm_data = 95` (ENOTSUP).

### Inline send

`send_flags::inline_send` requests inline payload staging. The
awaiter validates against `connection_config::max_inline_data` BEFORE
calling the backend and rejects oversized inline requests with
`wc_status::local_length_error` so you fail fast with a clean error:

```cpp
elio::rdma::connection<my_backend> conn{
    qp_ptr, dispatcher,
    elio::rdma::connection_config{.max_inline_data = 256}};

elio::rdma::send_flags flags{};
flags.inline_send = true;
auto wc = co_await conn.send(small_buf, flags);
if (wc.status == elio::rdma::wc_status::local_length_error) {
    // wc.imm_data carries the rejected payload size
}
```

## Memory region RAII

If your backend implements `register_mr` / `dereg_mr` / `lkey_of` /
`rkey_of`, you can use `memory_region<Backend>` for RAII:

```cpp
elio::rdma::memory_region<my_backend> mr{
    pd, buffer, length, IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE};

auto wc = co_await conn.send(mr.view());        // lkey filled in
auto remote = mr.remote();                       // for peer to RDMA WRITE at us
```

`view(offset, length)` carves a sub-range with the same lkey;
`remote(offset, length)` does the same for the peer-visible side.
`remote_buffer::length` is also 32-bit; `memory_region::remote()` and
`remote(offset, length)` are fast-path helpers, so callers must advertise only
lengths that fit in `uint32_t`. For larger registered regions, advertise
explicit sub-ranges or another application-level segmentation scheme.

## Driving completion: dispatcher + cq_pump

A `dispatcher` is the bridge between a CQ-poll loop and the
suspended awaiters. Two ways to drive it:

### Default: cq_pump coroutine

Bind an `ibv_comp_channel` fd to your scheduler's io_context:

```cpp
elio::rdma::dispatcher disp;
elio::coro::cancel_source pump_stop;

scheduler.go([&]() -> elio::coro::task<void> {
    co_await elio::rdma::cq_pump(
        comp_channel->fd, disp,
        [&](elio::rdma::dispatcher& d) noexcept {
            // Real ibverbs sequence:
            //   ibv_get_cq_event, ibv_req_notify_cq,
            //   ibv_poll_cq in a loop, d.deliver(...) per CQE,
            //   ibv_ack_cq_events.
        },
        pump_stop.get_token());
});
```

The pump checks the cancel token before and after each poll. To stop
cleanly:

```cpp
pump_stop.cancel();
// Then write a wake byte to the fd to unblock the in-flight poll.
```

### Manual: drive dispatcher.deliver directly

For busy-poll, DPDK-style, or shared-CQ setups, call
`dispatcher.deliver(wr_id, status, byte_len, imm_data, wc_flags)`
from any thread. It's safe to call from outside Elio's scheduler;
the coroutine resumes via `runtime::schedule_handle`, which falls
back to inline resume when no scheduler is current.

## op_state lifecycle

The awaiter and the dispatcher race for ownership of a per-WR
`op_state` heap node:

* The awaiter holds the node via `unique_ptr`.
* On `co_await` it posts the WR with `wr_id =
  dispatcher::make_wr_id(op_state*)`.
* When the CQE arrives, `dispatcher.deliver(wr_id, ...)` CASes
  `pending → completed`, fills the result, and resumes the coroutine.
* If an owner explicitly destroys the awaiter before the CQE arrives, the
  destructor CASes `pending → orphaned`; the dispatcher's later CQE arrival
  sees `orphaned` and silently frees the node.

Exactly one party frees the heap node; the coroutine is resumed at
most once. This is the same UAF-safe pattern PR #69 introduced for
io_uring.

Task cancellation in Elio is cooperative: requesting cancellation does not
force-destroy a suspended child coroutine, and RDMA operation awaiters do not
currently accept a cancellation token. Keep the operation's coroutine frame
and its dispatcher/CQ driver alive until `await_resume` consumes the
completion. Force-destroying the frame is unsupported once the dispatcher has
changed the state to `completed` and queued its handle. The awaiter terminates
the process in that case because the scheduler has no safe way to revoke the
queued handle; continuing would leave a stale handle and permit a
use-after-free. Before completion wins, explicit destruction remains safe via
the `pending → orphaned` handoff described above.

## The high-level `endpoint` wrapper (libibverbs path)

If you're using libibverbs anyway and don't want to wire up PD / CQ
/ comp_channel / QP yourself, enable both
`-DELIO_ENABLE_RDMA_IBVERBS=ON` and `-DELIO_ENABLE_RDMA_CM=ON` and
use `elio::rdma_ibverbs::endpoint`:

```cpp
#include <elio/rdma_ibverbs/rdma_ibverbs.hpp>

elio::rdma_cm::event_channel cm_ch;
auto ep = co_await elio::rdma_ibverbs::connect(
    cm_ch, dst_addr, sizeof(*dst_addr),
    elio::rdma_ibverbs::endpoint_config{
        .max_send_wr = 8, .max_recv_wr = 8});
ep.start_cq_pump(sched);

auto mr = ep.register_buffer(buf, len, IBV_ACCESS_LOCAL_WRITE);
auto wc = co_await ep.conn().send(mr.view());
```

`endpoint` bundles:
* `ibv_pd` (allocated against the cm_id's verbs context),
* `ibv_comp_channel` + armed `ibv_cq`,
* `rdma_create_qp` against the cm_id,
* a `dispatcher` + `connection<ibverbs_backend>` over that QP,
* a `cq_pump` coroutine started on demand via `start_cq_pump`,
* `register_buffer(...)` returning `memory_region<ibverbs_backend>`
  bound to the endpoint's PD.

Server side uses `acceptor`:

```cpp
elio::rdma_ibverbs::acceptor ac{cm_ch, bind_addr, len};
auto ep = co_await ac.accept();    // accepts ONE connection
ep.start_cq_pump(sched);
// ... data path identical to client
```

For full control over QP attributes, set
`endpoint_config::custom_qp_init_attr` to a pre-populated struct;
the wrapper hands it to `rdma_create_qp` unchanged (it will still
fill `send_cq` / `recv_cq` if you left them null).

**Shutdown contract**: the destructor cancels the cq_pump, destroys
the QP first (its flush CQEs wake the pump), waits up to 1s for
the pump to observe the cancel, then tears down CQ / comp_channel /
PD. If the pump doesn't exit in time (custom drain that blocks
indefinitely) the verbs resources are intentionally leaked rather
than risk a use-after-free.

Worked example: `examples/rdma_req_resp_ibverbs.cpp` runs a
client / server in one process — client SEND request → server
RDMA_WRITE response → server SEND_WITH_IMM "done" — using
`endpoint` + `acceptor` + `send_with_imm`.

## Connection bootstrap (librdmacm)

If you enabled `ELIO_ENABLE_RDMA_CM=ON`, use `<elio/rdma_cm/rdma_cm.hpp>`:

```cpp
elio::rdma_cm::event_channel ch;
elio::rdma_cm::cm_status status;

// Client: resolve address + route, then create QP and finalise.
auto id = co_await elio::rdma_cm::resolve(
    ch, dst_addr, sizeof(*dst_addr), {}, &status);
if (!status.ok()) { /* handle errno */ }

// Application creates the QP on id.native() using its own pd /
// qp_init_attr — Elio doesn't touch ibverbs directly here.
ibv_qp_init_attr qp_init = ...;
::rdma_create_qp(id.native(), pd, &qp_init);

co_await elio::rdma_cm::complete_connect(ch, id);

// Now run the data path:
elio::rdma::connection<my_backend> conn{id.qp(), disp};
co_await conn.send(buf);
```

Server side mirrors with `cm_listener` + `accept_connect`.

## Worked example

`examples/rdma_pingpong_mock.cpp` is a single-process ping-pong on a
mock backend that pairs `post_send` with the peer's posted recvs
in-memory — no hardware required. Build and run:

```bash
cmake -B build -DELIO_ENABLE_RDMA=ON
cmake --build build --target rdma_pingpong_mock
./build/examples/rdma_pingpong_mock
```

It demonstrates: `connection<Backend>`, paired send/recv,
`dispatcher::deliver`, `cq_pump` against an eventfd, and a
two-task coroutine pattern (echo on one side, request/reply on the
other).

## Integration testing on Soft-RoCE (rxe)

RDMA mock tests are included in the default test binary (`elio_tests`)
only when RDMA support is configured with `ELIO_ENABLE_RDMA=ON`. These
`[rdma]` tests run purely against mock backends and need no hardware:

```bash
cmake -B build -DELIO_ENABLE_RDMA=ON
cmake --build build --target elio_tests
./build/tests/elio_tests "[rdma]"
```

End-to-end validation against a real verbs stack lives in a separate
binary gated behind `ELIO_ENABLE_RDMA_IBVERBS_TESTS=ON`. The test binary
links the ibverbs target, so configure both `ELIO_ENABLE_RDMA_IBVERBS=ON`
and `ELIO_ENABLE_RDMA_IBVERBS_TESTS=ON`. Build it like:

```bash
cmake -B build \
  -DELIO_ENABLE_RDMA=ON \
  -DELIO_ENABLE_RDMA_IBVERBS=ON \
  -DELIO_ENABLE_RDMA_IBVERBS_TESTS=ON
cmake --build build --target elio_rdma_integration_tests
./build/tests/elio_rdma_integration_tests "[rdma_integration]"
```

The test gracefully `SKIP`s when no usable RDMA device is present —
that's the OrbStack case (kernel exposes rxe metadata but userspace
`uverbs` returns `EPERM`), and any non-RDMA Linux box behaves the
same way.

To make the test actually run, set up Soft-RoCE locally on a stock
Linux host (kernel module is `rdma_rxe`, in-tree since 4.8):

```bash
sudo modprobe rdma_rxe
sudo rdma link add rxe0 type rxe netdev <your-nic>      # e.g. eth0
# /dev/infiniband/uverbs0 should appear automatically. If udev didn't
# create the cdev for some reason, mknod it manually:
#   sudo mknod /dev/infiniband/uverbs0 c 231 192
ibv_devinfo                                              # sanity check
```

The integration test exercises the full Elio data path against rxe:
two RC QPs in-process, brought up to RTS via direct attribute
exchange (no CM), a registered MR per side, one SEND from QP_A
matched by a posted RECV on QP_B, dispatched through
`elio::rdma::connection<ibverbs_backend>` (a real `ibv_post_send` /
`ibv_post_recv` shim that lives under `tests/integration_rdma/`).

## Out of scope (deferred)

* XRC / Reliable Datagram QPs.
* RoCEv2 GID helpers / VLAN tagging.
* `mlx5` direct-verbs fast path.
* Send-with-invalidate and memory windows (MW).
* End-to-end RDMA performance tuning guide.

These can land as additive PRs without breaking the current API.
