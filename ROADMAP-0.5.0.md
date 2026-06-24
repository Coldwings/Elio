# Elio v0.5.0 Roadmap

Status: Draft
Date: 2026-06-10
Method: Five-faction code debate (Architecture Guardian, Performance Engineer,
API Advocate, Reliability Engineer, Simplicity Advocate) — 4 rounds, full
consensus reached.

---

## Overview

v0.5.0 focuses on **correctness infrastructure and internal quality**: TSAN
coverage restoration, exception observability, real I/O cancellation, and
modularization of the sync subsystem. No new user-facing features; no breaking
API changes.

Guiding principle: make the existing primitives provably correct before adding
new ones.

---

## Milestone: v0.5.0

### P0 — Must Ship

#### ~~1. Split `sync/primitives.hpp` into individual headers~~ — Done (PR #140)

**Scope:** Refactor only — no behavioral changes.

`primitives.hpp` (1425 lines) bundles 7 unrelated synchronization types. This
violates the project's own pattern (`coro/` has task.hpp, generator.hpp,
cancel_token.hpp each standalone) and means touching spinlock recompiles every
TU that uses mutex.

Deliverables:
- `sync/mutex.hpp`
- `sync/shared_mutex.hpp`
- `sync/semaphore.hpp`
- `sync/event.hpp`
- `sync/channel.hpp`
- `sync/condition_variable.hpp`
- `sync/spinlock.hpp`
- `sync/primitives.hpp` — retained as umbrella `#include`, backward compatible

Acceptance: all existing tests pass unchanged; no public API delta.

---

#### ~~2. `when_any` loser exception reporting~~ — Done (PR #136)

**Problem:** When the winner returns normally and a loser throws, the exception
is silently swallowed (`when_any.hpp:123`, `catch(...)` in the `go()` lambda).
In production, a database corruption error racing against a timeout becomes
invisible.

Deliverables:
- Default: loser exceptions logged at ERROR via `ELIO_LOG_ERROR`
- Per-scheduler handler callback:
  `scheduler::set_unhandled_exception_handler(handler)` — when registered,
  called for loser exceptions instead of default log. Handler stored per
  scheduler instance (not global); `promise_base` retrieves it via
  `get_current_scheduler()`. Thread-safe: handler pointer is
  `std::atomic<std::function<...>*>`.
- when_any tests: verify loser exception triggers handler when set, logs
  ERROR when not set

Performance note (from perf engineer): while touching when_any internals,
evaluate replacing `shared_ptr<state>` with intrusive refcount or frame-embedded
state to eliminate 1 heap allocation per call (~40-80 ns). Deferred to v0.6.0
pending profiling evidence — not a blocker for v0.5.0.

---

#### ~~3. I/O awaitable `cancel_token` support~~ — Done (PR #135)

**Problem:** `cancel_token` currently only works with `time::sleep_for`. Core
I/O awaitables (recv, send, connect) have no cancel_token overload. When
`when_any` cancels a loser that's blocked on `co_await recv()`, the coroutine
hangs until the peer closes the connection or `shutdown_force` fires. This makes
when_any's cancellation promise hollow.

Deliverables:
- `recv(fd, buf, len, cancel_token)` — on cancellation, issues
  `IORING_OP_ASYNC_CANCEL` to abort the pending kernel op
- `send(fd, buf, len, cancel_token)` — same
- `connect(fd, addr, cancel_token)` — same
- Cancellation path tests: verify token cancellation unblocks I/O awaitable
- epoll backend: best-effort via `close(fd)` or document unsupported

---

#### ~~4. Restore multi-threaded TSAN test coverage~~ — Done (PR #137)

**Problem:** 10 sync primitive test cases in `test_sync.cpp` are forced to
single-threaded scheduler with comment "Use single-threaded scheduler to avoid
TSAN memory reuse false positives". This means the concurrent synchronization
primitives have zero concurrent TSAN verification — the lock-free mutex LIFO
stack, shared_mutex reader-writer state machine, and channel waiter wake-up
paths are all untested under TSAN.

Root cause: coroutine frame reuse (allocator recycles frames) looks like a
data race to TSAN because it doesn't understand the happens-before relationship.

Deliverables:
- Add `__tsan_acquire` / `__tsan_release` annotations at coroutine frame
  allocation/deallocation points so TSAN understands frame recycling
- Restore multi-threaded scheduler (2+ workers) for at least: mutex, channel,
  semaphore, shared_mutex tests
- CI gate: `test_tsan` target must pass with multi-threaded sync tests

---

#### ~~5. Unify `condition_variable::wait` return type~~ — Done (PR #138, WONTFIX)

**Problem:** `wait(mutex&)` returns `task<void>` (double co_await pattern),
while `wait(Lock&)` returns a raw awaitable (single co_await). Switching lock
types silently changes the call pattern — a trap for users. The compiler does
not flag the error: `co_await cv.wait(mtx)` with single co_await compiles but
yields an unfinished awaitable object instead of void.

Deliverables:
- Unify both overloads to the same raw awaitable pattern (single co_await)
- Update tests and documentation
- Consider if double co_await can be eliminated entirely

---

### P1 — Should Ship

#### ~~6. Mark epoll backend as experimental~~ — Removed

**Rationale for removal:** epoll is not experimental — it has 20 years of
production validation. Marking it `@experimental` would be misleading and
unnecessarily discourage users on kernel < 5.1. The correct documentation
framing is "io_uring recommended for kernel 5.1+; epoll is the reliable
fallback for older kernels" — not a stability warning.

---

#### 7. Deprecated macro deletion

Three deprecated macros in `async_main.hpp:321-333`:
- `ELIO_ASYNC_MAIN_VOID`
- `ELIO_ASYNC_MAIN_NOARGS`
- `ELIO_ASYNC_MAIN_VOID_NOARGS`

All emit `#pragma GCC warning` but lack a removal version. Grep confirms
zero downstream usage in tests/ and examples/.

Deliverables:
- Delete all three macros directly (no deprecation cycle — pre-1.0 library)
- Audit for any other `[[deprecated]]` markers missing removal timeline
  (e.g., `cancel_token::on_cancel_resume`)

---

#### ~~8. `go()` exception handler integration~~ — Done (PR #139)

**Design note:** `go()` is fire-and-forget by design — exception swallowing is
the expected semantic, not a deficiency. Users who need exception observability
should use `spawn()` → `join_handle<T>` or `go_joinable()`. The three-tier API
(`go` / `go_joinable` / `spawn`) already covers all use cases.

However, the per-scheduler handler introduced by item 2 provides an opportunity:
when a handler is registered, `go()` tasks route unhandled exceptions to it for
diagnostic purposes (logging, metrics), without changing the fire-and-forget
contract. When no handler is registered, the default behavior changes from silent
drop to `ELIO_LOG_ERROR`.

Deliverables:
- `promise_base` destructor: if `exception_` is set and task is detached:
  1. Query `get_current_scheduler()->unhandled_exception_handler()`
  2. If handler registered → invoke handler
  3. If no handler → `ELIO_LOG_ERROR` with exception type info
- Test: go() task throws with handler → handler called; without → log ERROR

---

#### 9. Channel benchmark harness

**Rationale for P1 promotion:** With #9 (rendezvous semantics) merged (PR #141),
the channel internal state machine now has three modes (rendezvous/bounded/unbounded).
A benchmark baseline is needed to:
1. Measure the performance impact of the rendezvous changes
2. Detect regressions in future modifications
3. Provide data for any future optimization proposals (lock-free or otherwise)

The original P2 rationale ("lock-free is indefinitely deferred, so benchmark
has no immediate decision to drive") no longer holds — the benchmark has
standalone value independent of lock-free plans.

Deliverables:
- Catch2 `BENCHMARK` section in a new `tests/bench/bench_channel.cpp`
- Scenarios: SPSC throughput, MPMC (4 producer / 4 consumer), contention
  scalability (1-16 threads), bounded vs unbounded vs rendezvous
- Baseline numbers recorded in wiki/Performance-Tuning.md

---

### P2 — Record, Not Urgent

| # | Item | Notes |
|---|------|-------|
| ~~9~~ | ~~`channel(0)` semantics~~ | **Done.** `channel(0)` changed to rendezvous; `channel<T>::unbounded()` added. |
| 10 | `shared_mutex::unlock` writer path | Replace `std::vector<handle>` temp allocation with intrusive list. |

---

### Deferred to v0.6.0

| Item | Precondition |
|------|-------------|
| `task_group` / structured concurrency | <300 lines hard cap; reuse `when_all` internals; API RFC first |
| `when_all`/`when_any` `shared_ptr` elimination | Profiling evidence that heap allocation is the bottleneck; depends on task_group lifecycle model |

### Deferred Indefinitely (Data-Driven)

| Item | Gate |
|------|------|
| Channel lock-free rewrite | Benchmark (P1 #9) must prove mutex is the bottleneck + TSAN infra (item 4) must be complete |
| io_uring registered buffer/fd | Benchmark must prove syscall overhead is the bottleneck |

### Maintain As-Is

| Item | Rationale |
|------|-----------|
| RDMA modules (3718 lines, 4 sub-modules) | Differentiation path vs PhotonLibOS. Not expanding in v0.5.0 but keeping compile + test green. Pre-1.0 "no external users" applies to entire library, not RDMA specifically. |
| `cluster::` module | Not building. Application-layer concern. |
| HTTP/2 server | Not building. High complexity, small user base. |

---

## Dependency Graph

```
                    ┌─────────────────────┐
                    │  1. primitives.hpp   │
                    │     split            │
                    └─────────┬───────────┘
                              │ enables cleaner test files
                              ▼
                    ┌─────────────────────┐
                    │  4. TSAN multi-      │
                    │     thread restore   │◄─── independent of split
                    └─────────────────────┘     but benefits from it
                              │
          ┌───────────────────┼───────────────────┐
          ▼                   ▼                   ▼
  ┌───────────────┐ ┌─────────────────┐ ┌─────────────────┐
  │ 2. when_any   │ │ 3. I/O cancel   │ │ 5. condvar      │
  │    exception  │ │    token        │ │    unify        │
  └───────┬───────┘ └────────┬────────┘ └─────────────────┘
          │                  │
          ▼                  │
  ┌───────────────┐          │
  │ 8. go() handler│◄────────┘
  │   (uses #2's  │
  │    handler)    │
  └───────────────┘
          │
          ▼
  ┌───────────────────────────────┐
  │  0.6.0: task_group            │
  │  (depends on exception infra  │
  │   + I/O cancel being stable)  │
  └───────────────────────────────┘
```

Items 1-5 (P0) have no hard dependencies on each other and can be developed
in parallel as independent PRs. Recommended order: #3 → #2 → #4 → #5 → #1.
Item 8 shares the per-scheduler exception handler mechanism with item 2 and
should be implemented together or immediately after.

---

## Verification

Each PR must pass:

```bash
cmake -B build && cmake --build build
ctest --test-dir build --output-on-failure
cmake --build build --target test_asan
cmake --build build --target test_tsan    # especially for items 1, 4
```

Tag-filtered validation:
- Items 1, 4, 5: `./build/tests/elio_tests "[sync]"`
- Item 2: `./build/tests/elio_tests "[combinators]"`
- Item 3: `./build/tests/elio_tests "[io]"`
- P1 #9: `./build/tests/elio_tests "[benchmark]"` (new tag)

---

## Decision Log

| Decision | Rationale | Decided by |
|----------|-----------|------------|
| ~~`channel(0)` is P2, not P0~~ | **Reversed.** Rendezvous semantics implemented; `channel<T>::unbounded()` provides escape hatch. Go-style default is safer; unbounded-by-default hides back-pressure bugs. | Owner override |
| RDMA: maintain, don't freeze | Pre-1.0 "no users" applies to all modules; 3764 lines of mock-based tests run in CI; differentiation path | Owner override |
| TSAN is P0, not maintenance | 10 single-thread downgrades = structural testing gap, not debt | Reliability convinced Architecture |
| task_group deferred to 0.6.0 | Need stable exception + cancel infra first; <300 line hard cap | Architecture + Simplicity |
| Channel lock-free deferred | No benchmark baseline; "who proposes, who proves" | Simplicity principle |
| Breaking changes batched to 0.6.0 | Header-only users need migration window across minor versions | Architecture + Simplicity |
| `go()` exception swallowing is by design | Three-tier API (`go`/`go_joinable`/`spawn`) covers all use cases; fire-and-forget means exceptions are not observable by contract; per-scheduler handler is opt-in diagnostic, not a fix | Owner override |
| condvar unify promoted to P0 | Double co_await is a silent API trap: switching lock types compiles but yields wrong semantics; compiler cannot flag the error | API Advocate + Architecture |
| Deprecated macros: delete directly | Pre-1.0 library; grep confirms zero downstream usage; no deprecation cycle needed | Simplicity |
| Channel benchmark promoted to P1 | #9 (rendezvous) merged; benchmark needed as baseline for future optimization and regression detection; lock-free still deferred but benchmark has standalone value | Performance + Simplicity |
| shared_ptr optimization deferred | No profiling evidence that heap allocation is the bottleneck; v0.5.0 focuses on correctness, not micro-optimization | Architecture + Simplicity |
| epoll cancel: warn + no-op | close(fd) causes fd reuse race (catastrophic in high-frequency scenarios); epoll cannot truly cancel in-flight ops | Reliability |
| when_any loser exception: log ERROR | Loser exceptions may indicate systemic issues (resource exhaustion, protocol violation); WARNING insufficient for production diagnostics | Reliability |
| Exception handler: per-scheduler | Avoids global mutable state; handler lifecycle follows scheduler instance; multi-scheduler tests can inject mock handlers | Architecture (Owner approved) |
| I/O cancel reuses existing io_backend::cancel | io_backend already has `virtual bool cancel(void*)` pure virtual; no new abstraction layer needed | Architecture (self-corrected) |
| epoll not marked experimental | epoll has 20 years of production validation; marking it experimental is misleading and unnecessarily discourages users on kernel < 5.1 | Owner override |
| `channel(0)` = rendezvous | Go-style default is safer; unbounded-by-default hides back-pressure bugs; only 1 internal usage so migration cost near zero | 5-faction debate consensus |
