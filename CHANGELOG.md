# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [0.4.0] - 2026-06-10

First tagged release. Includes 89 commits of bug fixes, hardening, and API
stabilization since the 0.3.0 internal milestone.

### Breaking Changes

- **`when_all` single-argument return type**: `co_await when_all(f)` now returns
  `std::tuple<T>` instead of bare `T`. Update call sites from
  `auto result = co_await when_all(f)` to `auto [result] = co_await when_all(f)`.
  (#122)
- **`channel` is now non-movable**: `channel(channel&&)` and
  `operator=(channel&&)` are deleted. The previous move constructor did not hold
  any mutex, creating a data race with concurrent `send`/`recv`. Channels should
  be heap-allocated or wrapped in `std::unique_ptr` when ownership transfer is
  needed. (#121)
- **`io_context` is now unconditionally non-movable**: Previously movable under
  the epoll backend but non-movable under io_uring. Both backends now
  consistently delete move operations. (#120)

### Added

- **`when_any` stabilized** — removed `ELIO_EXPERIMENTAL` gate. Callables may
  now optionally accept a `cancel_token` parameter for cooperative cancellation
  when another callable wins. Homogeneous return types are unwrapped
  automatically (`pair<size_t, T>` instead of `pair<size_t, variant<T,T,...>>`).
  (#129)
- **`with_timeout` combinator** — `co_await with_timeout(duration, callable)`
  returns `timeout_result<T>` with `timed_out` flag and optional value. Supports
  cancel_token propagation for cooperative cancellation on timeout. (#134)
- `elio::go_to(worker_id, f, args...)` free function and `ELIO_GO_TO` macro for
  fire-and-forget task spawning pinned to a specific worker. (#124)
- Unified `ELIO_ASYNC_MAIN(func)` macro that auto-detects all four async-main
  signatures (args/no-args x int/void). The three old variants
  (`ELIO_ASYNC_MAIN_VOID`, `ELIO_ASYNC_MAIN_NOARGS`,
  `ELIO_ASYNC_MAIN_VOID_NOARGS`) are deprecated. (#125)
- `when_all` cancels remaining tasks on first exception via cooperative
  `cancel_source`/`cancel_token`. (#117)
- Autoscaler for automatic worker thread pool scaling based on queue depth. (#29)
- Async generator (`generator<T>`) with symmetric transfer and `ELIO_CO_FOR`
  iteration. (#40)
- Batch I/O API and high-level file helpers (`file_helpers.hpp`). (#45)
- Sharded `object_cache` with coroutine-safe get-or-create. (#103)
- RDMA-Core abstraction layer (experimental, gated by `ELIO_ENABLE_RDMA`). (#102)
- CUDA GPUDirect RDMA helpers (experimental, gated by `ELIO_ENABLE_RDMA_CUDA`). (#105)
- Per-binary SIGALRM watchdog for hang diagnosis in tests. (#96)
- `coro::traits.hpp` with `task_value_t` and `is_task_v` type traits. (#108)
- `io_context::backend()` accessor. (#109)

### Fixed

- **semaphore::release() phantom permit leak**: `release(n)` woke `k` waiters
  but added the full `n` to the count, leaking `k` phantom permits. Now
  correctly adds `n - k`. (#123)
- **vthread_stack**: `assert()` in `pop()` replaced with `__builtin_trap()` so
  invariant checks remain active in release builds. (#120)
- **spinlock slow path**: removed `std::this_thread::sleep_for()` which could
  block worker threads. High-contention warning logged at DEBUG level after lock
  acquisition. Class documentation clarifies spinlock is thread-blocking and not
  coroutine-aware. (#131)
- Sync primitives: all types (`semaphore`, `event`, `channel`, `mutex`,
  `shared_mutex`, `condition_variable`, `spinlock`) now have explicit
  copy + move `= delete` for consistency. (#126, #121)
- Runtime: vstack leak on wrapper construction exception. (#116)
- Runtime: trampoline exception handle leak. (#110)
- Runtime: worker_thread false sharing via `alignas(64)`. (#115)
- Runtime: drain race, shutdown ordering, shrink IO leak. (#70)
- Runtime: `workers_` grow race and active_tasks shutdown. (#72)
- Runtime: `blocking_pool` shutdown refusal, arm64 hang fix. (#83, #104)
- Runtime: task tracking from spawn-time, not body-resume-time. (#98)
- I/O: io_uring default `queue_depth` reduced from `512 * nproc` to flat 256,
  clamped to [64, 4096]. Fixes `ENOMEM` on arm64 runners with low
  `RLIMIT_MEMLOCK`. (#133)
- I/O: io_uring CQE use-after-free and per-op syscall reduction. (#69)
- I/O: io_uring deadlock in `submit_wake_poll`. (#58)
- I/O: epoll switched from edge-triggered to level-triggered. (#61)
- I/O: `io_context::run` blocks on notify instead of polling. (#71)
- Network: async stream close to avoid fd-reuse races with io_uring. (#90)
- Network: errno preservation across DNS negative-cache, IPv6 zone-id. (#74)
- HTTP: request smuggling, header injection, chunk overflow rejection. (#67)
- HTTP: client-side CL+TE validation, response/timeout limits. (#93)
- HTTP/2: header name/value copy to avoid nghttp2 UAF. (#59)
- HTTP/2: no-throw across nghttp2 C callback on malformed headers. (#76)
- WebSocket: serialized writes, pipelined upgrade, mask direction. (#65)
- SSE: serialized sends, cancel token observance. (#94)
- TLS: CA path lifetime, ALPN re-registration, close_notify in dtor. (#79)
- TLS: skip SSL_shutdown when socket already shut, ignore SIGPIPE. (#88)
- RPC: DoS hardening, slow-loris protection, endianness fixes. (#85)
- RPC: frame watchdog pinning, concurrent dispatch determinism. (#91, #95)
- Cancel token: mutex deadlock in `add_callback`, resume scheduling. (#51, #63)
- Cancel token: guard against io-awaitable double-resume. (#77)
- Coro: generator rvalue UAF in `for_each`. (#78)
- Coro: `when_all` logs warning on discarded subsequent exceptions. (#111)
- Coro: double-resume prevention in `set_waiter`. (#64)
- Sync: lost wakeups in mutex/shared_mutex, TOCTOU in channel. (#68)
- Sync: TSAN warning elimination in synchronization primitives. (#50)
- Signal: use `pthread_sigmask` for thread-safe signal masking. (#73)
- Time: sleep awaitable migration to `op_state`, SQ-full fallback. (#81, #86, #92)

### Changed

- `channel(0)` documentation clarified: capacity 0 means unbounded (unlike Go's
  rendezvous semantics). (#125)
- Lock-free fast path for `shared_mutex` read lock. (#52)
- Autoscaler proportional scale-up and scale-down hysteresis. (#62)
- Log formatting moved outside global lock to reduce contention. (#75)
- Hash: hardware-accelerated SHA-1/256 and hardened CRC32. (#84)
- Chase-Lev deque: dropped unused `steal_batch`, reclaim old buffers. (#66)
- Worker thread: deleted unused move semantics. (#119)
- Coro: `when_any` resolve/resolve_void merged into single template. (#112)
- Sync: `shared_mutex::unlock_shared` correctness documented — `WRITER_WAITING`
  flag prevents reader sneak-in during the unlock window. (#132)
- Tools: gdb/lldb scripts updated for `std::array` workers and `op_state`. (#89)
- Build: improved portability, package export, CI consumer check. (#33)

### Known Limitations

- TSAN sync primitive tests currently run with single-threaded scheduler to work
  around coroutine-frame-reuse false positives. Multi-threaded TSAN coverage
  will be restored in a future release with targeted suppressions.

## [0.3.0] - 2026-02-03

Internal milestone. Core runtime, I/O backends, networking stack, HTTP/1.1,
HTTP/2, WebSocket, SSE, TLS, RPC, and synchronization primitives feature
complete.

### Added

- Work-stealing scheduler with Chase-Lev deque
- io_uring and epoll backends with automatic detection
- TCP and Unix domain socket support (IPv4/IPv6)
- HTTP/1.1 server and client
- HTTP/2 client via nghttp2
- WebSocket (RFC 6455) and Server-Sent Events
- TLS via OpenSSL with ALPN
- RPC framework with zero-copy serialization
- Synchronization primitives: mutex, shared_mutex, semaphore, event, channel,
  condition_variable, spinlock
- Virtual thread stack with coroutine backtrace support
- Cancel token for cooperative cancellation
- `when_all` and `when_any` combinators
- Signal handling via signalfd
- Timer wheel integration (`sleep_for`, `sleep_until`, `yield`)
- Debug tools: elio-pstack, GDB/LLDB scripts
- Hash utilities (CRC32, SHA-1, SHA-256)

## [0.2.0] - 2025-12

Internal milestone. Scheduler rewrite, per-worker I/O, performance tuning.

### Added

- Thread-local I/O backend per worker
- Worker affinity and task pinning
- `go()`, `spawn()`, and `join_handle` API
- Unified I/O backend notification and worker wakeup
- `serve` utility for graceful shutdown

## [0.1.0] - 2025-11

Initial internal release.

### Added

- Basic coroutine task type
- Single-threaded scheduler
- epoll-based I/O
- TCP networking
