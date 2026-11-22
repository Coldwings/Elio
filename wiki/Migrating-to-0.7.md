# Migrating to 0.7

Before 1.0, the CMake package treats each minor version as a separate
compatibility line. After reviewing these changes, switch consumers that
request `find_package(Elio 0.6 REQUIRED)` to `find_package(Elio 0.7 REQUIRED)`.

## RPC receive data

Rebuild both sides of any binary boundary that exchanges `rpc::buffer_ref`,
`rpc::buffer_view`, or types containing them. Their C++ layouts changed.
Use copy and move operations rather than copying their object representation.
The RPC wire format did not change.

Ordinary typed calls need no source change. An unbound response blob keeps its
message alive even when copied out of the result. A raw pointer or span
extracted from the blob does not keep the message alive.

If the caller already owns the destination, bind a response field with
`rpc::into<&Reply::data>(dst)`. A contiguous result borrows the written
prefix. For scatter, bind an iovec span and an output count; a successful
call sets the count and leaves the bound field empty because it cannot
represent noncontiguous bytes. Scatter requires a nothrow-move-constructible
reply. Keep storage and counts exclusive until the awaited call returns,
including on cancellation. A short successful response leaves the tail
untouched; an error can leave partial data. See [[RPC Framework]] for
multiple bindings and codec requirements.

## RDMA waiters

Move direct data-path waits in custom coroutine promises into an Elio
`coro::task<T>` or `coro::generator<T>`. This includes operations already
posted with `.start()`. Ordinary CQ pollers can still call
`dispatcher::deliver()`; only the waiting coroutine is restricted. See
[[RDMA Guide]].

## Worker-bound external objects

Acquire a move-only worker lease with `co_await runtime::lease_current_worker()`.
Check acquisition and release the lease only after worker-affine resources no
longer need service. A lease contributes to graceful drain; it does not poll
an external queue, expose the backend ring, or make forced shutdown safe for
application-owned objects. See [[Worker Leases]].
