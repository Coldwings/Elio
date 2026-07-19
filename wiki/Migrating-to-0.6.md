# Migrating To 0.6

Elio 0.6 changes coroutine ownership, cancellation, structured concurrency,
worker-local I/O enforcement, and several runtime contracts. Review the items
below when upgrading from 0.5.x.

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
- Lazy task ownership no longer installs creator-thread virtual-stack state.
  Ancestry is bound when a task is actually awaited; independent scheduler
  spawn detaches construction-time ancestry.

## Cancellation And Structured Concurrency

- Every Elio task has a shared execution context with cooperative cancellation
  authority. Direct lazy-task awaits propagate runtime cancellation between
  Elio promises. Foreign coroutine promises and explicit token parameters stay
  separate unless an adapter deliberately bridges them.
- `join_handle::request_cancel()` is a best-effort request, not forced frame
  destruction. Pass `this_coro::cancel_token()` into every wait that should
  react and handle cancellation callback exceptions where callbacks may throw.
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

See [[API Contracts]] for the authoritative guarantee/responsibility inventory
and the Unreleased section of `CHANGELOG.md` for the complete change list.
