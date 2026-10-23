# Worker Lifetime Leases

`<elio/runtime/worker_lease.hpp>` provides `elio::runtime::lease_current_worker()`
and the move-only `external_worker_lease` type. They are also available through
`<elio/elio.hpp>`.

A lease represents a long-lived external obligation tied to one worker OS
thread. Examples include an external completion queue or a thread-affine device
runtime. Unlike best-effort affinity, an active lease delays normal worker
retirement even after a pool shrink removes that worker from `num_threads()`.
The acquiring logical task, including directly awaited Elio child frames,
continues to execute on that exact owner. Independent spawned roots do not
inherit the lease's execution constraint.

Foreign-thread event notifications and cancellation wakeups return the leased
task to its retained scheduler/worker, not the notifier's scheduler. An isolated
`task_scope()` keeps its own cancellation and placement context and may migrate;
its return restores a still-leased parent to the parent's owner. Do not access
the parent's thread-affine resources from a scope body that migrates elsewhere.
Async generators likewise keep an independent producer context, but yield and
exhaustion restore a leased consumer to its owner. `for_each()`'s internal loop
shares its Elio caller's context so callbacks and loop return honor that lease.
An application-provided awaitable that directly resumes on a foreign executor
must arrange its own owner handoff before thread-affine code runs.

```cpp
auto owner = co_await elio::runtime::lease_current_worker();
// Initialize an external owner on this thread.
// Drive it in bounded turns, awaiting events between turns as appropriate.
// Drain all external commands and destroy thread-affine resources here.
owner.release();
```

Acquisition does not suspend on success. It requires an Elio task currently
executing on a running scheduler worker. Outside a worker it throws
`std::logic_error`; after retirement admission or shutdown has closed it throws
`std::runtime_error`. Allocation or execution-context setup may also throw.
Acquisition is serialized with retirement admission: either the obligation is
counted before drain, or acquisition is rejected.

## Ownership And Release

The runnable `examples/worker_lease.cpp` demonstrates retaining an event waiter
on worker 1 while a controller shrinks the visible pool to one worker.

- `active()` reports whether this token retains its obligation. A default or
  moved-from token is inactive. Move assignment releases the destination's old
  obligation. Destruction releases any remaining obligation.
- `worker_id()` and `context_generation()` identify the acquired context. These
  are diagnostic snapshots, not reusable worker handles. After release they do
  not authorize scheduling or access to a later worker incarnation.
- `release()` is idempotent and may run on a foreign thread. Serialize access
  to the token itself; concurrent release/move/inspection of one token is not
  supported. Release removes the acquiring task's ownership constraint and
  wakes the owner so retirement can progress.
- Moving a token does not transfer the original logical task's execution
  constraint to a different task. Keep the owner continuation in the acquiring
  logical task, or arrange an explicit owner-service path before release.
- An inactive token and the metadata of a token retained after forced teardown
  are safe to destroy after the scheduler. That does not make an external ring,
  queue, borrowed buffer, or suspended task safe to abandon.

## Progress And Shutdown

The lease contributes to pending/active accounting even when the acquiring task
is waiting on a semaphore/event and has no pending native I/O. Draining workers
continue bounded owner-task service and native backend polling. A continuously
yielding owner cannot trap retirement in unbounded task redistribution.

The application still owns the external protocol, completion progress,
cancellation, fairness, and resource teardown. A lease is not a polling hook,
does not expose the backend ring, and does not automatically drive another ring
through an eventfd. Never block the worker on an external blocking wait.

Graceful `shutdown()` waits for the obligation within its existing timeout
policy. A timed-out shutdown and `shutdown_force()` retain Elio's existing
non-graceful behavior and may stop workers with active leases. Release external
owners before scheduler teardown for correctness; a lease does not make forced
frame destruction safe or turn cooperative cancellation into completion.

Native protocol integrations can account for their obligations internally;
their users need not manually acquire a lease unless their own external
ownership requires one. See [[API Contracts]] for the general responsibility
boundary.
