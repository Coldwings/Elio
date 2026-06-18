# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [0.5.0] - 2026-06-18

Focus: **correctness infrastructure and internal quality**. TSAN coverage
restoration, exception observability, real I/O cancellation, modularization of
the sync subsystem, and comprehensive bug fixes (74 bugs across all subsystems).

### Breaking Changes

- **`channel(0)` semantics changed to rendezvous**: `channel<T>()` and
  `channel<T>(0)` now create a synchronous rendezvous channel (Go-style
  hand-off: `send` suspends until a matching `recv` is ready). Use
  `channel<T>::unbounded()` to create an unbounded channel with no
  back-pressure. The previous `channel(0) = unbounded` semantics was
  inconsistent with Go/Rust/Kotlin conventions and has been replaced. (#141)
- **Deprecated async main macros removed**: `ELIO_ASYNC_MAIN_VOID`,
  `ELIO_ASYNC_MAIN_NOARGS`, and `ELIO_ASYNC_MAIN_VOID_NOARGS` have been
  deleted. Use the unified `ELIO_ASYNC_MAIN(func)` macro which auto-detects
  all four signatures (args/no-args x int/void). (#143)

### Added

- **`sync/primitives.hpp` modularization**: Split into individual headers
  (`sync/mutex.hpp`, `sync/shared_mutex.hpp`, `sync/semaphore.hpp`,
  `sync/event.hpp`, `sync/channel.hpp`, `sync/condition_variable.hpp`,
  `sync/spinlock.hpp`). The umbrella `primitives.hpp` is retained for
  backward compatibility. (#140)
- **Per-scheduler exception handler**: `scheduler::set_unhandled_exception_handler()`
  allows routing unhandled exceptions from detached tasks (`go()`) and
  `when_any` losers through a custom callback. When no handler is set,
  exceptions are logged at ERROR level (previously silently dropped).
  (#136, #139)
- **I/O awaitable `cancel_token` support**: `recv()`, `send()`, and
  `connect()` now accept an optional `cancel_token` parameter. Cancellation
  issues `IORING_OP_ASYNC_CANCEL` on io_uring backend to abort pending
  kernel operations. Makes `when_any` cancellation promise real for I/O-bound
  tasks. (#135)
- **Channel benchmark harness**: `examples/bench_channel.cpp` provides
  performance baselines for SPSC/MPMC throughput, contention scalability,
  and bounded vs unbounded vs rendezvous comparisons. (#144)

### Changed

- **TSAN multi-threaded coverage restored**: 10 sync primitive tests in
  `test_sync.cpp` now run with multi-threaded schedulers instead of
  single-threaded. Coroutine frame reuse false positives resolved via
  `__tsan_acquire`/`__tsan_release` annotations at frame
  allocation/deallocation points. (#137)
- **`condition_variable::wait` design clarified**: Both `wait(mutex&)` and
  `wait(Lock&)` overloads intentionally use different patterns (double
  co_await vs single co_await). Documented as intentional design choice
  with rationale in class documentation. (#138)

### Fixed

#### Sync Primitives (10 bugs)

- **channel rendezvous deadlock**: `send()` slow path did not suspend when
  no receiver was waiting, violating rendezvous semantics. Fixed by adding
  proper suspension on `send_waiters_` queue. (#150)
- **channel bounded recv sender starvation**: When `try_push()` failed after
  popping from ring, sender remained in `send_waiters_` without wakeup.
  Fixed by waking sender on `try_push()` failure. (#150)
- **channel bounded send closed race**: Fast path did not re-check `closed_`
  under lock, allowing send after close. Fixed by adding lock-protected
  closed check. (#150)
- **channel close deadlock**: `items_available_.release()` called while
  holding mutex could resume coroutines via trampoline, causing same-thread
  deadlock. Fixed by moving release outside lock scope. (#146)
- **channel recv data loss**: `await_ready()` returning true skipped value
  extraction in `await_suspend()`, leaving `result` unset. Fixed by using
  retry loop with suspend-only awaitable. (#146)
- **channel send slow path deadlock**: `unique_lock<std::mutex>` held
  across `co_await` suspension point caused producer to hold mutex while
  suspended. Fixed by scoping lock to critical section before `co_await`.
  (#146)
- **LockfreeMPMCRing capacity=1 overflow**: Vyukov algorithm cannot
  distinguish "slot writable" from "slot written but unread" with single
  slot. Fixed by enforcing minimum capacity of 2. (#146)
- **condition_variable atomicity violation**: User mutex unlock occurred
  after enqueue in `await_suspend()`, creating race window. Fixed by moving
  unlock inside `internal_mutex_` critical section. (#150)
- **mutex unlock dangling pointer**: Waiter address transfer in unlock
  could reference freed coroutine frame. Fixed by using sentinel locked
  marker. (#150)
- **semaphore/shared_mutex count overflow**: Added overflow guards and
  assertions for non-negative counts. (#150)

#### Coroutine Infrastructure (8 bugs)

- **join_handle exception propagation data race**: `await_resume()` could
  destroy `join_state` while catch block accessed exception via `e.what()`.
  Fixed by holding `shared_ptr` copy of `join_state` during `get_value()`
  execution. (#157)
- **cancel_token concurrent callback semantics**: Documented that
  `on_cancel()` callbacks may execute concurrently with cancellation. (#149)
- **when_any winner selection race**: Removed unsafe `await_suspend` fast
  path; all resumptions now go through `schedule_handle()` for proper
  happens-before synchronization. (#149)
- **task promise destructor safety net**: Added `~promise_type()` that calls
  `join_state_->mark_destroyed()` to prevent `wait_destroyed()` deadlock on
  force-destroy. (#149)
- **with_timeout result access guards**: Added `assert(value.has_value())`
  to `timeout_result::operator*()` overloads. (#149)
- **task return_value exception routing**: Wrapped `join_state_->set_value()`
  in try-catch to properly route exceptions instead of losing moved-from
  values. (#149)
- **generator exception handling**: Removed redundant
  `promise_base::unhandled_exception()` call; generator manages its own
  exception storage. (#149)
- **promise_base frame chain clobbering**: Guarded `current_frame_ = parent_`
  restoration with `if (current_frame_ == this)` to prevent clobbering
  unrelated frame chains during detached coroutine destruction. (#149)

#### Runtime and Scheduler (9 bugs)

- **Chase-Lev TOCTOU race**: `get_next_task()` read `num_threads()` with
  relaxed ordering then passed `!single_worker` to `pop_local()`, creating
  window where newly-started worker could steal while owner used unfenced
  fast path. Fixed by always passing `allow_concurrent_steals=true` and
  using acquire ordering for steal decision. (#153)
- **Chase-Lev pop_local missing fence**: Unsafe fast path removed entirely;
  `pop_local()` now always delegates to `pop()` which uses required seq_cst
  fence. (#153)
- **autoscaler start/stop TOCTOU**: Non-atomic check-then-act replaced with
  `compare_exchange_strong` to atomically claim/release ownership. (#153)
- **autoscaler config data race**: Protected `config_` with mutex;
  `update_config()` locks before writing, `run()` takes snapshot under lock
  at each tick. (#153)
- **scheduler set_thread_count deadlock**: Added runtime guard checking
  `worker_thread::current()` at entry; returns early if called from worker
  thread (prevents re-entrant deadlock on `workers_mutex_`). (#153)
- **autoscaler success detection race**: Removed racy re-read of
  `num_threads()` after `set_thread_count()`; treat step > 0 as 'attempted'.
  (#153)
- **mpsc_queue documentation**: Corrected "wait-free" → "lock-free" comment.
  (#153)
- **work stealing test flakiness**: Increased task workload from 50k to 200k
  iterations and added 50ms delay after `set_thread_count(4)` to ensure
  tasks remain queued long enough for stealing. (#155)
- **worker_thread steal decision synchronization**: Added acquire ordering on
  steal decision to synchronize with `set_thread_count()`'s release store.
  (#153)

#### I/O Subsystem (10 bugs)

- **batch_read_awaitable UAF**: No orphan protocol for in-flight CQEs;
  forced cancel could dereference freed `batch_state`. Fixed by adding
  atomic phase CAS (pending/orphaned/completed) analogous to `op_state`.
  (#154)
- **batch I/O submit error handling**: `submit_batch_io_uring()` now checks
  `io_uring_submit` return and rolls back `pending_ops_` on failure; added
  `unregister_pending()`. (#154)
- **cancellable I/O early cancel success**: Set `result_ = {-ECANCELED, 0}`
  on all early-cancel paths in recv/send/connect awaitables to distinguish
  cancellation from legitimate zero-byte operations. (#154)
- **epoll multi-pending-op consumption**: Only consume one op per event
  direction per poll cycle to prevent EAGAIN from consuming all queued ops.
  (#154)
- **epoll EEXIST state**: Set `state.registered = true` on EEXIST so
  subsequent calls use `EPOLL_CTL_MOD`. (#154)
- **epoll timer cancel key mismatch**: Added `cancel_key` field to
  `timer_entry` matching tagged `op_state` pointer used by
  `cancellable_sleep`. (#154)
- **io_uring poll error logging**: Log `io_uring_submit` errors in poll()
  auto-submit path. (#154)
- **epoll fd_state reset**: Reset `fd_state` after close so reused fd
  numbers register correctly. (#154)
- **cancellable I/O post-registration cancel**: Re-check cancelled state
  after registration and submit `ASYNC_CANCEL` inline in all 3 cancellable
  awaitables. (#154)
- **timer submit_blocking rejection**: `submit_blocking` returns bool;
  destroy awaiter handle on rejection to prevent frame leak. (#154)

#### Network and Signal Handling (13 bugs)

- **TCP/UDS accept_awaitable UAF**: Did not inherit `io_awaitable_base`,
  missing op_state orphan protocol. Fixed by making accept_awaitable inherit
  `io_awaitable_base` with proper `bind_to_worker`/`setup_op_state`.
  (#151)
- **signal_wait_awaitable UAF**: Did not use op_state lifecycle protocol.
  Fixed by inheriting `io_awaitable_base` with proper setup. (#151)
- **signal masking incomplete**: Only blocked signals on current thread.
  Fixed by blocking SIGINT/SIGTERM at `worker_thread::run()` entry so all
  workers inherit blocked mask. (#151)
- **TCP connect_awaitable fd leak**: No destructor to close fd on coroutine
  destruction. Fixed by adding destructor that calls
  `close_fd_for_destructor`. (#151)
- **UDS abstract socket address**: Constructor trimmed trailing nulls
  incorrectly; added length-aware overload. (#151)
- **signal update() unblock**: `update()` now unblocks signals removed from
  the set. (#151)
- **UDS to_sockaddr truncation**: Throws `std::invalid_argument` instead of
  silent truncation. (#151)
- **signal ctx_ move semantics**: Changed from reference to pointer for
  correct move semantics. (#151)
- **hash HashDoS vulnerability**: Replaced polynomial hash with FNV-1a to
  prevent HashDoS attacks. (#151)
- **signal destructor SQE draining**: Destructor and `close()` now use
  `close_fd_for_destructor` for proper SQE draining. (#151)
- **UDS unlink_on_bind warning**: Logs warning on unexpected unlink failures
  instead of silent ignore. (#151)
- **TCP listener close race**: Added op_state support to prevent UAF on
  close during pending accept. (#151)
- **UDS close/accept coordination**: Resolved by accept_awaitable op_state
  orphan protocol. (#151)

#### HTTP and WebSocket (19 bugs)

- **HTTP server TLS capture**: Lambda captured `&tls_ctx` by reference into
  fire-and-forget `go()`; could outlive caller. Fixed by capturing
  `std::shared_ptr<tls_context>`. (#152)
- **HTTP server send_response error handling**: Silently swallowed write
  failures; caller continued keep-alive loop on broken stream. Fixed by
  returning bool and breaking loop on failure. (#152)
- **HTTP server active connection tracking**: Added atomic counter for
  graceful shutdown coordination. (#152)
- **HTTP server keep-alive logic**: Fixed edge cases in connection reuse
  decision. (#152)
- **HTTP server error handler safety**: Added null checks and exception
  guards. (#152)
- **HTTP client pool mutex-across-suspension**: `lock_guard<std::mutex>`
  held across `co_return` suspension point. Fixed by extracting connection
  under lock into local variable before suspension. (#152)
- **HTTP client absolute response deadline**: Added timeout enforcement for
  entire response, not just individual reads. (#152)
- **HTTP client HTTPS→HTTP redirect**: Reject downgrade redirects to
  prevent credential leakage. (#152)
- **HTTP parser chunk CRLF validation**: Added validation for both chunked
  parsers. (#152)
- **HTTP parser URI control character rejection**: Reject control characters
  in URIs. (#152)
- **HTTP common keep_alive parsing**: Proper token-based parsing instead of
  substring search. (#152)
- **WebSocket continuation frame validation**: Reject continuation frames
  without preceding initial frame (RFC 6455 §5.4). (#152)
- **WebSocket close code validation**: Validate close codes and reject
  reserved values. (#152)
- **WebSocket UTF-8 validation**: Validate text message payloads as UTF-8.
  (#152)
- **WebSocket CSPRNG**: Use cryptographically secure PRNG for mask keys
  instead of `std::mt19937`. (#152)
- **WebSocket handshake CSPRNG**: Use CSPRNG for Sec-WebSocket-Key. (#152)
- **WebSocket server TLS capture**: Same fix as HTTP server TLS capture.
  (#152)
- **WebSocket close response codes**: Map `no_status` (1005) to `normal`
  (1000) per RFC 6455 §7.4.1. (#152)
- **SSE server atomic state**: Changed `state_` to `std::atomic` for thread
  safety. (#152)
- **SSE client retry overflow**: Added overflow protection for retry delay
  calculation. (#152)
- **SSE client event dispatch**: Spec-compliant event dispatch with proper
  field handling. (#152)
- **SSE client persistent backoff**: Persistent exponential backoff across
  reconnection attempts. (#152)

#### RPC (5 bugs)

- **RPC protocol version field**: Added `version` byte to `frame_header`
  struct; validated in `is_valid()` to reject incompatible versions. (#148)
- **RPC client receive_loop leak**: Changed from `shared_ptr` to `weak_ptr`
  capture to prevent receive loop from running forever with no consumer.
  (#148)
- **RPC client ping timeout**: Added cancellation support to `ping()`
  timeout task using `cancel_source`. (#148)
- **RPC server cleanup callback leak**: Wrapped send path in try/catch so
  exceptions don't cause cleanup callbacks to be silently dropped. (#148)
- **RPC server send_response return**: Changed to return bool and check
  `write_frame`'s return value; cleanup callbacks only run on successful
  write. (#148)

### Documentation

- **Wiki and README synchronization**: Automated audit found and fixed 106
  documentation mismatches across 17 files. Updated API references, file
  paths, CMake options, behavioral descriptions, and code examples to match
  current implementation. (#156)

### Performance

- **Bounded channel lock-free fast path**: `channel::send()` and
  `channel::recv()` now use `LockfreeMPMCRing` (Vyukov's bounded MPMC queue)
  for bounded channels, eliminating mutex contention on hot path. Mutex
  only acquired for waiter queue management. (#146)

### Internal

- **`shared_mutex::unlock` optimization**: Eliminated vector allocation in
  writer path by using intrusive list for waiter handles. (#145)
- **TSAN cancel executor affinity**: Set affinity on timer/IO cancel
  executors to prevent cross-worker stealing and TSAN data races. (#147)

### Testing

- **Multi-threaded TSAN coverage**: Restored multi-threaded schedulers for
  sync primitive tests; added `__tsan_acquire`/`__tsan_release` annotations.
  (#137)
- **Work stealing test stabilization**: Fixed flakiness in Release builds by
  increasing task workload and adding delay after thread pool expansion.
  (#155)

### Verification

All quality gates passed:
- ✅ 475 unit tests, 2770 assertions
- ✅ ASAN clean (no memory errors)
- ✅ TSAN clean (no data races, including arm64-Debug)
- ✅ CI: 5/5 checks pass (x64/arm64 x Debug/Release + package-consumer)

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
