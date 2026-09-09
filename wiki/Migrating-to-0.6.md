# Migrating To 0.6

Elio 0.6 changes coroutine ownership, cancellation, structured concurrency,
worker-local I/O enforcement, and several runtime contracts. Review the items
below when upgrading from 0.5.x.

## Borrowed Scatter/Gather Stream Writes

TCP `writev()` now returns `task<io_result>` and handles readiness/EINTR
internally, matching scalar `write()`. Existing `co_await stream.writev(...)`
usage remains valid; code storing the old concrete raw-awaitable type must
change. A token overload is available on TCP, TLS and `net::stream`:

```cpp
auto progress = co_await stream.writev(parts, count, token);
```

Keep the descriptor array and all payload bytes alive and unchanged until
completion. Positive progress may be short: advance your vector cursor before
the next call. TLS writes a borrowed nonempty slice rather than joining the
payload into an intermediate buffer. Zero-length entries are skipped. A single
call accepts at most 1024 descriptors and INT32_MAX aggregate bytes; split
larger logical writes. An already-cancelled token returns ECANCELED even for
empty input when a transport is present. Without cancellation, empty input
succeeds with zero on a connected stream. An empty `net::stream` with no
transport variant returns ENOTCONN even if the supplied token is cancelled,
matching its scalar read/write dispatch behavior.

Low-level `io::async_sendmsg(fd, parts, count, flags, token)` is also available,
but remains a single backend attempt: readiness and short-write handling are
its caller's responsibility. Cancellation can race positive progress and does
not undo bytes already transmitted. Await cleanup before reusing buffers or
closing the connection.

## Task Ownership And Virtual Threads

- `coro::task<T>` is move-only. Move unstarted tasks into containers, return
  values, scheduler handoffs, and other owners; do not copy them or pass an
  lvalue where ownership transfer is required.
- Normal task frames use ordinary coroutine allocation. The public
  `vthread_stack` bump allocator and its deferred cross-thread deletion
  protocol were removed. Direct allocator users must provide an allocator with
  suitable object lifetime and cross-thread free behavior.
- The logical vthread model remains. Virtual-stack ancestry, task chains,
  debugger inspection, affinity, and runtime execution context still describe
  coroutine execution independently of frame allocation.
- Directly awaited Elio task frames now share one execution context for the
  logical vthread. A token captured in a transparent child therefore remains a
  token for the surrounding chain after that child completes, and affinity
  changes are immediately shared rather than copied back at return. Use an
  explicit `spawn`/`go` root when a helper needs an independent cancellation or
  affinity domain. `task_scope()` remains an isolated structured-cancellation
  boundary so group cancellation cannot poison its caller; its final user
  affinity still flows back. Foreign coroutine promises remain boundaries.
- Lazy task ownership no longer installs creator-thread virtual-stack state.
  Ancestry is bound when a task is actually awaited; independent scheduler
  spawn detaches construction-time ancestry.

## Cancellation And Structured Concurrency

- Every running Elio logical vthread has a shared execution context with
  cooperative cancellation authority. Direct lazy-task awaits reuse it between
  Elio promises. Independent runtime roots, foreign coroutine promises, and
  explicit token parameters stay separate unless an adapter deliberately
  bridges them.
- Low-level integrations that raw-resume a nested unstarted task must establish
  an independent execution context before inspecting promise policy. Normal
  task awaits and runtime handoffs do this automatically.
- `join_handle::request_cancel()` is a best-effort request, not forced frame
  destruction. Pass `this_coro::cancel_token()` into every wait that should
  react and handle cancellation callback exceptions where callbacks may throw.
- Built-in cancellable I/O and timer waits reserve their owner-worker abort
  handoff before submission. For registered operations on a live worker,
  cancellation no longer needs a freshly allocated abort executor or the
  ordinary task queue's allocating overflow path. Temporary backend admission
  rejection retains abort intent until admission or original-operation
  retirement; owner/context checks and permanent key retirement prevent stale
  retries from targeting reused operation storage. Epoll timer cancellation
  removes the timer in place without rebuilding an allocating queue (#1202).
  No signature change is required. Continue awaiting cleanup before releasing
  buffers, descriptors, or task frames. This does not make user callbacks
  non-throwing, guarantee progress under arbitrary allocation failure, or add
  standalone cross-thread cancellation or safe forced shutdown. Registration
  and setup may still allocate before submission.
- `mutex`, `shared_mutex`, `semaphore`, `event`, `condition_variable`, and
  `channel` have explicit cancellable wait overloads. Existing no-token
  overloads retain their prior result shapes.
- Use `coro::task_group` or `coro::task_scope()` when children must be cancelled
  and joined before captured state is released. Select fail-fast,
  collect-all, and bounded-concurrency policy explicitly.

## Combinator Behavior

- `when_all()`, `when_any()`, and `with_timeout()` now return move-only lazy
  tasks and structurally own every accepted branch.
- `when_any()` cancels and joins losers before returning. A token-ignoring loser
  can therefore delay the result.
- A launch-time callable-transfer failure in `when_any()` takes precedence over
  an already selected winner because the complete branch set was not accepted.
- `with_timeout()` treats the duration as the point at which cancellation is
  requested, not as a hard return-time bound. It waits for the wrapped work to
  reach a terminal state.
- Parent cancellation that prevents a required result throws
  `combinator_cancelled`. Review code that previously assumed every failed
  `with_timeout()` was a deadline expiry.
- Later loser/secondary exceptions are routed through the scheduler unhandled
  exception handler after the primary result is fixed.

## Scheduler And I/O Integration

- A scheduler is one-shot. Once shutdown begins, `start()` does not restart it;
  construct a new scheduler for another run.
- Pending scheduler-owned I/O pins a task to the exact worker/backend owner.
  Caller affinity changes take effect after the operation reaches a terminal
  state and cannot clear the internal I/O pin.
- Custom awaitables derived from `io_awaitable_base` must use
  `setup_op_state()` and `prepare_op_state()`. The old
  `bind_to_worker()`/`restore_affinity()` pattern and mutable raw-backend
  accessor are removed. Custom backend `notify()` implementations must be
  non-throwing.
- Elio does not serialize conflicting operations on the same stream or fd.
  Callers remain responsible for preventing overlapping reads, overlapping
  writes, close-versus-I/O races, and protocol ordering errors.

## RPC And Process Lifecycle

- RPC server sessions structurally own accepted request, pong, and overload
  response tasks. Session teardown requests cancellation and joins accepted
  work; a handler that ignores both runtime and RPC tokens can delay teardown.
- RPC admission remains explicit per-session policy. Configure bounded
  concurrency and overload handling instead of relying on clients to
  self-limit.
- Follow [[Fork Safety]]. A single-threaded process that forks before Elio
  runtime use may create independent parent/child runtimes. With an active
  runtime or any other thread, the child must immediately call an explicitly
  async-signal-safe exec function such as `execve()`, or `_exit()`, and must not
  use or destroy inherited Elio state. Do not assume every exec-family wrapper
  is async-signal-safe.

## Upgrade Checklist

### HTTP Response Receive Changes

- HTTP and SSE now share incremental response framing. SSE enforces a truthful
  Content-Length, decodes real HTTP chunks, and treats truncated framing as an
  error. Replace fixtures that set CL/TE but send unrelated raw event bytes;
  see [[WebSocket SSE]] for valid wire examples.
- SSE `connect()` completes on final headers, not event/body completion.
  Retrieve piggybacked events through `receive()`. Preceding interim responses
  are capped by `sse::client_config::max_informational_responses` (16 by default;
  zero disallows interims). With reconnect disabled, clean HTTP completion
  returns no event with `errno = 0`; framing truncation reports `EBADMSG`.
- Unsupported response transfer-coding stacks such as `gzip, chunked` are
  rejected. Elio does not expose a partially transfer-decoded stream as if all
  transfer codings were removed. Content-Encoding handling is separate.
- The accumulating `response_parser` now exposes partial chunk payload before
  trailing CRLF validation. Require successful completion before treating its
  body as a valid complete response. Its consumed count still includes retired
  bytes from earlier calls; `reset()` still retains unread input.
- New `response_decoder` instead reports consumption only from the supplied
  input and borrows body slices from it. New `response_reader` exposes slices
  until its next operation, move, or destruction; copy only if longer ownership
  is needed. Use `next_response()` to retain interim-tail bytes and `reset()`
  to discard receive state for a new connection. Reapply request method after
  either reset. See [[HTTP Streaming]] for migration and ownership details.
- Final response headers received during an Expect wait suppress the upload
  immediately, including when the rejection response body arrives later or
  uses close delimiting. EOF processing is shared with ordinary responses.

The outgoing producer/writer redesign in
[#1191](https://github.com/Coldwings/Elio/issues/1191) is a separate pending phase;
these receive changes do not change existing server serialization contracts.

### General Checklist

1. Replace task copies and implicit lvalue handoffs with explicit moves.
2. Remove direct `vthread_stack` allocator use while preserving any logical
   virtual-stack/debugging integrations you need.
3. Pass runtime or explicit cancellation tokens into every operation that must
   stop during task-group or shutdown cancellation.
4. Re-evaluate `when_any()` and `with_timeout()` latency assumptions now that
   losers are joined.
5. Update custom I/O awaitables and backend `notify()` implementations to the
   0.6 ownership contract.
6. Define RPC concurrency/overload policy and ensure handlers cooperate with
   session cancellation.
7. Audit process creation paths against the documented fork boundary.
8. Run normal, ASAN, and TSAN validation for application-specific adapters and
   cancellation callbacks.
9. Check HTTP/SSE fixtures for truthful framing and review borrowed response
   body lifetimes before adopting the pull reader.

See [[API Contracts]] for the authoritative guarantee/responsibility inventory
and the `0.6.0` section of `CHANGELOG.md` for the complete change list.
