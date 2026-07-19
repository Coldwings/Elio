# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

## [Unreleased]

### Added

- **Runtime task cancellation contexts**: Every task execution context now owns
  cooperative cancellation authority. `join_handle::request_cancel()` remains
  valid across task completion and frame destruction, while
  `this_coro::cancel_token()` exposes the active task token with a
  never-cancelled fallback outside Elio execution. Cancellation propagates
  one-way through direct lazy-task awaits between Elio tasks; foreign coroutine
  types, independent spawns, and explicit token parameters remain separate
  cancellation boundaries. Constructing a task execution context and linking a
  lazy child can now report allocation failure; the context constructor and
  `task::await_suspend` are therefore no longer `noexcept`. Requests are
  best-effort and do not force frame destruction, roll back side effects, or
  replace a result that already completed.
- **Structured task groups**: Added scheduler-bound `coro::task_group` and
  callback-shaped `coro::task_scope()` APIs. Groups propagate parent and group
  cancellation to children, join every registered child frame, default to
  fail-fast sibling cancellation, optionally collect all failures without
  cancelling siblings through `task_group_error`, and can bound concurrently
  executing child bodies. `task_scope()` runs its body in the group cancellation
  context, so child fail-fast cancellation can wake a token-aware body, and
  always cancels and joins children when its body fails. Collect-all scopes
  aggregate simultaneous body and child failures. Completion and cancellation
  dispatch preserve the selected scheduler domain; requests from another
  scheduler are posted asynchronously to avoid reciprocal worker deadlock. A bare
  group requires an explicit `co_await group.join()`; `join()` returns a direct
  single-use awaitable instead of creating a nested task and therefore has no
  child-link allocation window. The group destructor can only request
  cancellation as a non-joining fallback. Task scopes retain their body
  callable and captures through child joining, while automatic locals in the
  returned body coroutine retain normal coroutine lifetime and must not be
  referenced by children after the body returns. Scheduler-domain operations
  require an actual worker; the thread that called `scheduler::start()` remains
  an external caller for cancellation dispatch.
- **Cancellation-aware basic synchronization waits**: Added explicit
  `cancel_token` overloads for `mutex::lock()`, `semaphore::acquire()`, and
  `event::wait()`. Each returns `cancel_result` and uses one terminal wake state
  so cancellation and normal notification cannot both resume a waiter. A
  cancellation winner does not acquire a lock or consume a permit; an already
  completed acquisition remains caller-owned. The corresponding no-token
  awaiters retain their existing `void` results and lightweight fast paths.
- **Cancellation-aware shared lock and condition waits**: Added explicit
  `cancel_token` overloads for shared and exclusive `shared_mutex` acquisition,
  plus all three `condition_variable` wait modes. Reader/writer counts, writer
  preference, and notification-versus-cancellation races use the same terminal
  winner model. A condition wait that released an associated lock re-acquires
  it before returning either `completed` or `cancelled`; a pre-cancelled wait
  leaves the already-held lock untouched.
- **Cancellation-aware channel operations**: Added `cancel_token` overloads for
  `channel::send()` and `channel::recv()`. Their result objects distinguish
  cancellation from a closed channel while preserving the existing boolean and
  optional results on no-token overloads. Cancellation winners do not transfer
  a pending send value into the channel or consume a received value; send,
  receive, and close paths skip cancelled waiters without consuming another
  waiter's wakeup.
- **Process fork boundary**: Documented the unsupported inherited-runtime
  continuation path and regression-tested supported single-threaded
  fork-before-runtime use plus active-runtime parent continuation with child
  `execve()`/`_exit()`. Elio does not install `pthread_atfork` repair hooks or
  provide child-side runtime reinitialization. Added a 0.5.x-to-0.6 migration
  guide covering ownership, cancellation, combinators, I/O integration, RPC,
  and process lifecycle changes.

### Changed

- **Structured coroutine combinators**: `when_all()`, `when_any()`, and
  `with_timeout()` now return move-only lazy `coro::task` objects and own every
  accepted branch through a scheduler-bound task group instead of detached child
  tasks or dedicated combinator awaitables. `when_all()` remains fail-fast but
  drains cancelled siblings before rethrowing the first child failure. Later
  child failures remain observable through the scheduler unhandled-exception
  handler. A callable-transfer failure while launching takes precedence because
  the complete branch set was not accepted. `when_any()`
  still selects the first successful or exceptional completion, requests
  cancellation of the remaining branches, and now waits for every loser to
  reach a terminal state before returning or throwing. Late loser exceptions
  remain observable through the scheduler unhandled-exception handler, but no
  loser outlives the combinator implicitly. `with_timeout()` now waits for the
  wrapped operation after deadline cancellation, so token-ignoring work can
  delay return even though the result remains timed out. Parent cancellation
  that prevents a required result is reported through the new
  `combinator_cancelled` exception instead of being mistaken for timeout.
  Concurrent deadline expiry and parent cancellation follow the underlying
  best-effort timer outcome and may report either timeout or cancellation. These
  return-type, lifetime, and timing changes are intentional 0.6 breaking changes.
- **Structured RPC session teardown**: Server sessions now own accepted request,
  pong, and overload-response tasks through a scheduler-bound task scope. Session
  close requests both the explicit `rpc_context::cancel_token` and the runtime
  `this_coro::cancel_token()`, cancels pending frame writes and response-lock
  waits, closes pending frame reads when the session owner is cancelled, and
  joins every accepted child before releasing the session slot. Session
  bookkeeping remains valid if the server facade is released after its stopped
  `serve()` call returns while accepted handlers are still draining
  cooperatively.
  Cancellation remains cooperative, so a handler that stops observing both
  tokens can delay session teardown. Existing per-session admission and overload
  policies are unchanged.
- **One-shot scheduler lifecycle**: Once scheduler shutdown begins, later
  `start()` calls no longer restart workers or scheduler-owned resources, and
  pool resize no longer introduces workers during teardown. Concurrent shutdown
  callers now share one teardown, final worker joins are serialized, and
  external destruction waits for worker-initiated teardown.
- **Worker-local I/O ownership**: Scheduler-owned `io_context` instances now
  carry stable worker/generation identity. Submitted operations hold context
  drain accounting until completion, cancellation completion, prepare rollback,
  or orphan cleanup; Elio task promises also receive an operation-scoped
  effective-affinity pin. Pool shrink keeps retiring workers alive to poll
  pinned I/O and run owner-local cancellation work before a slot can restart.
  Mutating `io_context` operations enforce the exact owner thread, while
  `notify()` remains cross-thread safe and standalone contexts remain
  caller-driven. `io_context::notify()` and the `io_backend::notify()` override
  contract are now `noexcept`, so custom backend implementations must provide a
  non-throwing wakeup override. The mutable raw-backend accessor has been
  removed. These ownership checks do not serialize or diagnose conflicting
  operations on the same stream or fd; that concurrency remains the caller's
  responsibility. Downstream awaitables derived from `io_awaitable_base` must
  replace the removed `bind_to_worker()` / `restore_affinity()` pattern with
  `setup_op_state(awaiter, ctx)` and submit through `prepare_op_state()` so
  worker/context pin accounting and prepare-exception rollback are preserved.
- **Shared task execution context**: Every `promise_base` now references a
  shared `task_execution_context` control block. User affinity and the internal
  worker-local flag moved into this block, while scheduler placement reads its
  distinct effective-affinity boundary. Scheduler-created `join_handle` state
  shares the same context as the spawned wrapper promise, so runtime policy can
  outlive frame destruction without storing a raw promise pointer or keeping
  the frame alive. Lazy `task<T>` objects still own only their coroutine handle;
  operation completion state remains local to awaitables. Debugger layout
  discovery and frame helpers have been updated for the new promise layout.
- **Move-only lazy tasks**: `coro::task<T>` and `coro::task<void>` now support
  move construction and move assignment while remaining non-copyable. Moving a
  task transfers ownership of its unstarted coroutine frame and leaves the
  source empty. Replacing an owned task destroys the destination's previous
  lazy frame. Runtime handoff paths now require an explicit rvalue ownership
  transfer. Lazy ownership no longer installs a frame in creator-thread
  virtual-stack state; ancestry is bound when the task is actually awaited and
  preserved when a suspended task is scheduled for resumption, including
  targeted affinity migration between workers. Resume scheduling borrows the
  coroutine handle, reports stopped-worker rejection without consuming it, and
  uses a per-worker overflow queue for rare full-inbox bursts so worker-bound
  tasks are not resumed on submitting threads. Overflow batches remain visible
  to scheduler idle accounting while they move into a worker's local deque;
  non-blocking serialized queue snapshots prevent observers from crossing an
  entire transfer and mistaking accepted work for an idle runtime. Generator and
  cancellation-executor creation now restore the creator's virtual-stack TLS
  before independent handoff. Generator yield and completion transfers restore
  the active consumer context even after internal asynchronous suspension.
  Scheduler-bound
  `spawn_blocking()` calls now report an unavailable blocking pool on the
  current worker instead of falling back to a detached thread.
- **Coroutine frame allocation**: `task<T>` frames now use the standard heap
  allocation path in every build, without allocator-owned LIFO lifetime rules.
  Logical coroutine ancestry remains available through the `promise_base`
  frame chain, and frames may be destroyed on a different worker from the one
  that created them when ownership is transferred safely. The vthread
  abstraction, virtual stack tracking, and virtual-stack debugging APIs remain
  unchanged.
- **Scheduler accounting hooks**: The promise-level spawn-completion callback
  is now an internal scheduler detail rather than a public `promise_base`
  field. It was never a supported application extension point.
- **Timer fallback rejection**: If a timer cannot be prepared by the I/O
  backend and the scheduler blocking pool is unavailable, `sleep_for()` and
  `sleep_until()` now continue on the current worker and throw
  `std::runtime_error`. They no longer destroy the borrowed awaiting coroutine
  frame. This applies to both regular and cancellation-aware sleeps.
- **0.6.0 development metadata**: The in-tree development version is now 0.6.0,
  with README, public header, and CMake release metadata aligned after v0.5.3.

### Removed

- **Vthread frame allocator**: The public `elio/coro/vthread_stack.hpp` header,
  segmented `vthread_stack` bump allocator, and its ownership/deferred-deletion
  protocol have been removed. Direct allocator users must migrate to an
  allocator whose lifetime and destruction-order contract fits their objects;
  normal `task<T>` users do not need a replacement.
- **Allocator-bound helper APIs**: The `promise_base` vstack ownership accessors
  and `io_uring_backend::resume_with_vstack()` have been removed with the
  allocator. Code that used them for logical coroutine ancestry should use the
  virtual-stack inspection APIs instead; custom allocation is now independent
  of Elio runtime state.

### Fixed

- **Scheduler exception-handler publication**: Concurrent handler replacement,
  reporting, and getter calls now exchange shared handler snapshots under an
  explicit mutex instead of relying on `atomic<shared_ptr>`. Reporters and
  getter callers keep their selected handler alive, while reports invoke it
  outside the lock. This preserves concurrent replacement and self-replacement
  without the GCC 12 ThreadSanitizer race.
- **Cancellation callback teardown**: Destroying or unregistering a callback
  registration now synchronizes with cancellation that has already selected or
  started the callback, preventing callback captures from being released while
  dispatch still uses them. Self-unregistration and same-dispatch removal of a
  later callback remain deadlock-free. Cross-dispatch teardown requested from
  inside another cancellation callback is deferred to avoid mutual wait cycles;
  the callback payload remains alive through dispatch, while external captures
  still require caller synchronization. Cancellation rethrows the first
  callback exception after dispatching the remaining callbacks.

## [0.5.3] - 2026-07-17

### Added

- **Exact-length stream I/O helpers**: `read_exactly()` / `write_exactly()` on
  `net::tcp_stream`, `net::uds_stream`, `tls::tls_stream`, and the unified
  `net::stream`. They loop over partial `read`/`write` results until exactly
  `length` bytes are transferred, transparently retrying on transient
  `EAGAIN`/`EWOULDBLOCK` (waiting for socket readiness) and `EINTR` (never
  surfaced to callers). `read_exactly()` returns `-ENODATA` if the peer closes
  before `length` bytes arrive, and both helpers return `-EOVERFLOW` when asked
  to transfer more than `INT32_MAX` bytes. `net::stream::write_all()` is retained
  as a compatibility alias for `write_exactly()`. (#249)
- **TCP loopback benchmarks**: Added Elio, libuv, and standalone Asio benchmark
  targets plus workflow support for ping-pong and streaming TCP loopback
  measurements. (#241)
- **RDMA ibverbs receive and CUDA package surface**: Added shared receive queue
  support for the ibverbs backend and exported the optional
  `Elio::elio_rdma_cuda` package target for GPUDirect RDMA consumers. (#289)
- **RDMA started operations**: RDMA awaiters now support explicit `.start()`
  submission for receive preposting and real send/read/write pipelining while
  preserving lazy `co_await` semantics. The ibverbs request/response and
  performance examples now use started handles where ordering or queue depth
  requires work requests to be posted before awaiting them. (#766)
- **RPC fast-response regression coverage**: Added a deterministic client test
  that publishes a response before `call()` reaches its wait path, preserving
  coverage for the pending-response release/acquire contract. (#850)
- **RPC per-session request concurrency policy**: Added
  `max_in_flight_requests_per_session` and `request_overload_policy` to
  `rpc_server_config`, plus `rpc_error::resource_exhausted`, so servers can
  reject excess requests on a bounded best-effort path or close a session when
  one client exhausts its configured active-request budget. (#941)
- **UDS cancellable API parity**: Added cancellation-aware
  `uds_stream::read()`/`read_exactly()` overloads and `uds_connect()` overloads
  so Unix-domain sockets expose the same cancellation boundary as TCP streams
  and connects. (#943)

### Changed

- **RPC framing reuses the stream exact-length helpers**: `read_frame_bounded()`
  now calls the stream's `read_exactly()` instead of a private `read_exact()`
  loop, and the existing `rpc::rpc_stream` concept now also requires
  `read_exactly()`/`write_exactly()` alongside its scatter-gather write,
  readiness-poll, and validity requirements. A partial read/write of a frame
  means the message is corrupt regardless, so there is no behavioral reason for
  RPC to carry its own duplicate loop. The unused `rpc::read_exact()`/
  `rpc::write_exact()` helpers were removed.
- **0.5.3 development metadata**: The in-tree development version is now 0.5.3,
  with README, public header, and CMake release metadata aligned after v0.5.2.
- **Documentation and examples**: README/wiki/API snippets now reflect the
  current OpenSSL and default-build requirements, CMake options, installed
  package consumption flow, HTTP/2 and HTTP/TLS APIs, UDS networking, RDMA
  targets, RPC buffer semantics, scheduler APIs, signal handling, and
  generator-neutral example build commands.
- **Timeout cancellation documentation**: Clarified that `with_timeout()`
  cooperatively cancels token-aware work and does not forcibly destroy a losing
  child suspended in `event::wait()`. (#773)
- **WebSocket public-header example**: Qualified the HTTP route context and
  response types, aligned the async entry-point signature with its forwarded
  arguments, and added compile-only coverage for both contracts. (#772, #781)
- **CI and package validation coverage**: Pull-request CI now keeps required
  checks green for docs-only changes, runs registered CTest/package checks,
  verifies HTTP/2 option dependencies and optional installed targets, compiles
  TCP benchmark targets, bounds release benchmark runtime, and retries apt
  dependency installs on arm64 runners. (#895)
- **API responsibility boundaries**: Added wiki/API-reference guidance that
  separates Elio library guarantees from caller responsibilities across
  runtime, networking, TLS, HTTP, WebSocket, SSE, RPC, synchronization, file,
  hash, RDMA, logging, and debug APIs. Security guidance now points audits at
  those boundaries before expanding library responsibility. (#870, #889, #921)
- **Documented HTTP/WebSocket/RPC package contracts**: Clarified
  `server_config::max_request_size` as an aggregate HTTP request-byte cap,
  documented WebSocket reconnects as caller-managed, defined `net::stream`
  concurrency and RPC stream/client lifecycle contracts, described RPC CRC32 as
  non-cryptographic corruption detection rather than a security boundary, and
  distinguished HTTP/2 nghttp2 FetchContent source builds from installed-package
  dependency discovery. (#888, #902, #903, #922, #923, #924, #926)

### Deprecated

- **`signal_fd::restore_mask()`**: `signal_fd` blocking is now explicitly
  acquire-only. The non-composable whole-thread snapshot API is deprecated and
  disabled; it returns `false` without changing the mask. Callers release
  signals explicitly after all mask users are finished. (#784)

### Fixed

- **WebSocket server heartbeat enforcement**: `ws_server` now starts the
  configured route heartbeat after a successful upgrade, sends server pings,
  records pongs observed by the route handler receive loop, and closes stale
  connections that cannot send the heartbeat ping or observe a pong within
  `ping_timeout`. After the WSS upgrade completes, TLS streams now serialize
  direct OpenSSL state access so heartbeat writes can coexist with a handler
  receive loop waiting on the same connection. (#937)
- **HTTP parser consumed-byte accounting**: Request and response parsers now
  report consumed input bytes on `need_more` paths, preserving correct buffered
  data accounting for pipelined keep-alive requests and partial response-header
  reads. (#908, #911)
- **WebSocket upgrade and client-handshake hardening**: WebSocket upgrade
  request size accounting now counts the HTTP upgrade request but excludes
  post-upgrade frame bytes, client upgrade responses validate mandatory
  `Upgrade`/`Connection` semantics and enforce configured header count/line
  limits, and invalid pipelined server frames are rejected during handshake
  instead of leaking parser state into a later connection attempt. (#890, #908,
  #909, #919)
- **SSE and HTTP/1.1 boundary validation**: SSE response parsing now enforces
  configured response-header limits, HTTP/1.1 requests reject missing or
  duplicate `Host` headers where required, and chunked transfer parsing rejects
  malformed chunk metadata and trailers at the parser boundary. (#904, #891,
  #892)
- **RPC client timeout and control-frame routing**: RPC calls now cover the
  send and frame-read paths with configured timeouts, and pong completions route
  to the pending ping operation instead of being confused with call responses.
  (#893, #896, #897)
- **RDMA single-buffer length bounds**: High-level RDMA single-buffer
  send/receive/read/write operations now fail before posting when the local
  buffer length cannot fit the 32-bit SGE length field. (#905)
- **SSE Last-Event-ID contract handling**: SSE parsing now applies valid `id`
  fields to the Last-Event-ID buffer even when no event is dispatched, and
  honors empty `id:` fields as cursor resets for reconnects. (#860)
- **WebSocket mask-direction validation**: WebSocket frame parsing now rejects
  role-invalid masking as soon as the complete frame header is available instead
  of waiting for the payload body. (#861)
- **WebSocket control-frame ordering**: WebSocket receive loops now handle
  already-parsed control frames before delivering later data messages from the
  same read batch, preserving wire-order semantics for ping/pong/close. (#862)
- **WebSocket payload length canonicalization**: WebSocket frame parsing now
  rejects non-minimal extended payload length encodings at the frame-header
  boundary, matching the canonical encoding emitted by the frame encoder. (#863)
- **HTTP/2 response status validation**: HTTP/2 response parsing now rejects
  missing, malformed, duplicate, or out-of-range `:status` pseudo-headers
  instead of exposing them to callers as `200 OK`. (#872)
- **RPC frame contract validation**: Inbound RPC frames now reject unknown
  message types, reserved `compressed`/`streaming` flags, and unknown flag bits
  at the frame-header validation boundary instead of dispatching them as normal
  uncompressed requests or responses. (#854)
- **RPC typed payload canonicalization**: Typed RPC request, response, and
  error payload parsing now rejects trailing bytes after the declared typed
  fields instead of silently accepting undocumented extension data. (#873)
- **RPC frame type/flag validation**: RPC frame headers now reject request-only
  flags on non-request frames, nonzero method IDs on non-request frames, control
  frame payloads, and timeout-flagged requests that are too short to contain
  the timeout prefix. (#874)
- **SSE parser chunk-boundary CRLF handling**: The SSE client parser now treats
  `\r` at the end of one input chunk plus `\n` at the start of the next chunk as
  a single CRLF line ending, preserving pending event metadata across transport
  splits. (#857)
- **SSE event serialization safety**: Server-side SSE serialization now treats
  LF, CRLF, and standalone CR as data line separators, suppresses unsafe
  line-break-bearing `id`/`event` fields, and serializes multi-line comments as
  comments on every line to prevent control-field injection. (#855)
- **Linux platform validation**: CMake now rejects every non-Linux target at
  configure time instead of allowing unsupported Unix systems to fail later in
  Linux-specific headers. (#770)
- **CRC32C dispatch and capability reporting**: CRC32C entry points now keep
  ISA-dependent dispatch translation-unit local, avoiding conflicting inline
  definitions in mixed-flag builds. `crc32c_hw_available()` reports true only
  when that translation unit compiled a hardware implementation and the current
  CPU supports it. (#771)
- **Network stream readiness and socket setup**: TCP and UDS stream operations
  now wait for readiness on transient `EAGAIN`/`EWOULDBLOCK`, preserve readable
  data before HUP/EOF, apply requested `tcp_connect` socket options, and restore
  the active coroutine frame around I/O resumes. (#247)
- **Scheduler and runtime shutdown**: Fixed scheduler shrink/draining behavior,
  stopped-worker scheduling, blocking-pool joinability, `spawn_blocking`
  handoff/direct-resume deadlocks, exception-handler lifetime, `elio::run`
  shutdown on exceptions, rejected `go_joinable` handles, worker self-join,
  task-completion waiter lifetime races, and stale current-scheduler TLS after
  cross-thread shutdown, and blocking-pool worker self-shutdown. (#391, #660,
  #665)
- **Synchronization and object-cache lifetime**: Fixed mutex waiter lock
  transfer lifetime, sync handoff cancellation leaks, object-cache release
  handoff and canceled-construction cleanup, bounded-channel close state, and
  combinator waiter resume ordering and launch-time callable move failures.
  `when_any()` and `with_timeout()` also no longer require result types to be
  default-constructible. (#290, #760, #820)
- **Sync waiter wake ownership**: `event`, `condition_variable`, `mutex`,
  `semaphore`, `shared_mutex`, and `channel` wake paths now keep a cancelable
  wake token after dequeue, preventing a racing waiter-frame destruction from
  leaving a stale coroutine handle to schedule. (#762)
- **vthread stack fallback ownership**: Coroutine frames allocated outside a
  current `vthread_stack` now release their global fallback allocation on
  destruction and avoid popping memory from an unrelated current stack. (#848)
- **Cancellation callback exceptions**: Cancellation now invokes and releases
  every registered callback before rethrowing the first callback exception,
  including callbacks registered after cancellation. (#774)
- **`when_any` winner finalization**: Result-transfer and loser-cancellation
  exceptions can no longer leave `when_any()` or `with_timeout()` permanently
  suspended after a winner has been claimed. (#764)
- **`when_any` launch publication**: Launch completion and winner resolution now
  use one atomic wake state, preventing their concurrent publication from
  losing the only waiter resumption. (#775)
- **Signal masks during descriptor updates**: `signal_fd::update()` no longer
  unblocks signals removed from its descriptor, preserving caller-owned masks
  and avoiding premature delivery of pending signals. Newly blocked signals
  are rolled back when the descriptor update fails. (#769)
- **Signal mask error reporting**: `signal_set` now exposes direct
  `pthread_sigmask()` error-code helpers, and `signal_fd` reports automatic
  block failures from that direct error code instead of stale `errno`. (#791)
- **io_uring submit retry accounting**: Public `io_uring_backend::submit()`
  now keeps staged SQEs accounted for when `io_uring_submit()` returns a
  negative error, allowing later submit/poll attempts to retry them instead of
  making `run_until_complete()` believe suspended awaiters have no pending
  work. (#829)
- **RPC fast-response completion race**: `rpc_client::call()` now waits for a
  pending request's response/error fields to be published before parsing the
  result, avoiding intermittent default-valued successful responses when a peer
  replies before the caller reaches the normal completion wait. (#844)
- **RPC request ID reservation bound**: `rpc_client` now limits request ID
  collision probing to the current pending-request count plus one, avoiding a
  possible `UINT32_MAX`-probe CPU spin before reporting local reservation
  failure. (#846)
- **Batch io_uring submit retry accounting**: `batch_read()` and
  `batch_write()` now keep staged batch SQEs accounted and suspended after a
  negative direct submit result, so later poll retries cannot submit stale
  `batch_completion` user data after the awaitable has inline-completed. (#842)
- **io_uring prepare rejection contract**: `io_uring_backend::prepare()` now
  validates unsupported operations and malformed timeout requests before
  consuming an SQE, keeping rejected requests side-effect free. (#849)
- **Batch I/O fallback errors**: Synchronous `batch_read()` and `batch_write()`
  fallback paths now report syscall failures as `-errno`, matching io_uring and
  the public I/O result contract. (#763)
- **Batch I/O current-position offsets**: `batch_read()` and `batch_write()`
  now honor negative segment offsets as current file position operations
  instead of mapping them to absolute offset 0. (#830)
- **RDMA completion-channel routing**: `make_cq_drain()` now polls the CQ
  returned by each channel event, so shared completion channels cannot strand
  completions on a different CQ. (#765)
- **RDMA lazy-operation lifetime**: Destroying an RDMA operation awaitable
  before it is awaited now frees its unstarted completion state instead of
  orphaning it for a CQE that will never arrive. (#786)
- **RDMA operation awaitable move assignment**: Operation awaitables are no
  longer move-assignable, preventing replacement of a posted operation from
  freeing completion state while the backend still holds its `wr_id`. Move
  construction remains supported. (#787)
- **RDMA Release warnings-as-errors builds**: `op_awaiter_base::arm_()` no
  longer fails Release RDMA builds because its debug-only phase check stores an
  assertion-only state variable. (#793)
- **RPC cancellation and framing**: Fixed complete protocol writes, transient
  RPC `writev` retries, `rpc_client` timeout-watcher cleanup, member-coroutine
  self lifetime, end-to-end cancellation propagation, server accept wakeups on
  stop, RPC frame-header documentation, client request-id collision handling
  after wraparound, close-safe duplicate active server request-id rejection,
  and `rpc_client::ping()` success reporting when a connection closes before
  pong, and one-way RPC request frames now suppress replies from servers that
  understand the `no_response` flag so delayed replies cannot be matched to
  unrelated future requests after request-id wrap. Clients created from an
  existing stream now start their receive loop when a scheduler is current, and
  expose `start()` for off-scheduler construction. Closing an RPC client now
  completes pending calls without waking waiters while holding pending-map
  locks. (#246, #253, #294, #814, #815, #818, #819, #828, #833)
- **HTTP/1 client and server robustness**: Fixed response-body framing, skipped
  informational responses, URL authority parsing, client cancellation
  propagation, connect/handshake/read timeout enforcement, failed handshake
  cleanup, outbound header-injection rejection, and forbidden response-body
  suppression, empty request-target rejection, and HTTP request/response
  version validation. (#321, #711, #713, #746)
- **HTTP URL authority validation**: The URL parser now rejects unbracketed IPv6
  literals and authorities with multiple port separators instead of treating the
  last colon as a valid port delimiter. (#655)
- **HTTP URL port validation**: Explicit `:0` ports are rejected instead of
  being confused with the parser's default-port sentinel. (#656)
- **HTTP and SSE URL scheme validation**: Client entry points now reject
  unsupported URL schemes instead of treating them as plaintext HTTP. (#657)
- **WebSocket and SSE validation**: Fixed oversized WebSocket frame
  preallocation, malformed close payloads, close encode state preservation,
  subprotocol selection validation, non-terminal route wildcards, IPv6 client
  URLs, upgrade read timeouts, non-SSE content-type rejection, SSE parser
  buffering limits, and invalid URL errno reporting. (#320, #658)
- **HTTP/2 client behavior**: Fixed connect/read timeout enforcement, session
  config application, buffered response-body caps, HTTP/2 option dependency
  validation, duplicate response header preservation, nghttp2 FetchContent
  cache scoping, installed nghttp2 target restoration, and public HTTP/2 header
  documentation for the current high-level sequential pooled reuse contract.
  (#270, #752, #832)
- **RDMA correctness and optional build support**: Fixed RDMA CM event routing
  and status signs, cancellable CM polling, awaited operation completion
  requests, inline completion awaiter lifetimes, SGE and remote-buffer length
  validation, CQ pump cancellation, GPU memory-region move/default construction,
  and package/export metadata for optional RDMA targets. (#286, #291, #297)
- **Installed package and consumer metadata**: Installed packages now restore
  exported RDMA, liburing, nghttp2, C++20, and optional target requirements, and
  consumer checks validate those package contracts. (#269, #270)
- **Header and hash portability**: Public headers gained missing includes, SHA
  intrinsic includes no longer leak into the `elio` namespace, and CRC32C
  documentation now covers hardware and software implementations.
- **Pre-1.0 package version compatibility**: Installed package metadata now
  treats each `0.<minor>` release line as a separate compatibility boundary,
  so Elio 0.5.x no longer satisfies incompatible 0.4 or 0.6 requests. (#380)
- **HTTP header-line limits**: Request and response parsers now reject an
  oversized header line as soon as the buffered unterminated line exceeds
  `max_header_size`, instead of waiting for a trailing CRLF. (#377)
- **HTTP keep-alive pipelining**: The server now consumes request bytes buffered
  behind a completed keep-alive request before waiting for another socket read,
  preventing pipelined clients from stalling when multiple requests arrive in
  one TCP read. (#342)
- **Cancellable epoll I/O**: `async_recv()`, `async_send()`, and
  `async_connect()` now cancel pending epoll operations promptly when their
  `cancel_token` fires, matching the existing `async_poll_read()` behavior.
  (#339)
- **HTTP/WebSocket server stop**: `stop()` now cancels the active listener
  accept operation so idle listen loops exit promptly without needing a new
  client connection to wake them. (#341)
- **TLS shutdown timeout enforcement**: `tls::tls_stream::shutdown()` now bounds
  readiness waits with cancellable poll operations, so a peer that never sends
  `close_notify` cannot keep shutdown suspended beyond the configured timeout.
  (#338)
- **RDMA CM backlog wakeups**: `event_channel` now wakes filtered waiters when
  another waiter consumes and stashes their CM event, preventing
  `next_event_for()` / `accept_connect()` from sleeping indefinitely until an
  unrelated future CM event arrives. (#607)
- **Asio TCP ping-pong benchmark watchdog**: The Asio backend now uses the same
  grace window as the Elio backend before reporting `TIMEOUT`, so normal phase
  completion near the measurement boundary no longer fails the benchmark. (#609)
- **TCP benchmark workflow timeout budget**: Manual benchmark dispatch now
  accounts for ping-pong watchdog grace when sizing the outer client timeout,
  preventing accepted inputs from being killed before in-program timeout
  reporting can finish. (#611)
- **TCP benchmark compile CI timeout**: The PR compile-only TCP benchmark job
  now has a repository-controlled job timeout so configure/build stalls fail
  promptly instead of waiting for the GitHub Actions platform default. (#612)
- **RDMA CUDA bandwidth example buildability**: The GPUDirect RDMA bandwidth
  example is now gated on the CM helper it uses and has been updated from stale
  endpoint/receive helper calls to the current `event_channel`, `acceptor`,
  free `connect`, and `recv(buffer_view)` APIs. (#621)
- **Installed package thread dependency**: The exported `elio` target now links
  `Threads::Threads` instead of raw `pthread`, and installed package config
  restores the Threads dependency before loading exported targets. (#622)
- **WebSocket route parameters**: Named `:param` captures in WebSocket route
  patterns are now exposed through `ws_connection::param()` and `params()`, so
  `/chat/:room` handlers can read the captured room value. (#623)
- **Async readv API reference**: Added the documented public `async_readv()`
  awaitable and compile/runtime coverage for scatter-gather reads. (#624)
- **TCP benchmark mode parsing**: Invalid `-m` values now fail fast instead of
  silently running both ping-pong and streaming benchmarks. (#625)
- **TCP benchmark workflow scope**: The main-branch TCP loopback benchmark
  workflow now triggers only for benchmark-affecting source and workflow paths,
  instead of every example change. (#626)
- **Cancellable I/O completion ordering**: Completed TCP connects and lower-level
  cancellable I/O operations now preserve the backend completion result when a
  token is cancelled before `await_resume()`, instead of rewriting successful
  completions to `ECANCELED`. (#627)
- **TCP benchmark workflow build surface**: The main-branch TCP loopback
  benchmark workflow now builds only the TCP benchmark targets, keeping its
  build surface aligned with its narrowed trigger paths. (#635)
- **Elio TCP streaming benchmark pipeline depth**: The Elio backend now applies
  the documented `-q` streaming pipeline depth by writing message bursts, and all
  TCP benchmark backends share the same 1..64 normalized depth. (#649)
- **HTTP URL port validation**: URL parsing now rejects empty ports, overflowing
  ports, IPv6 authority suffixes, and trailing garbage after numeric ports instead
  of silently accepting the numeric prefix. (#647)
- **HTTP/1 TLS timeout cleanup**: Client read/write timeout paths now mark
  TLS-backed streams as externally shut down after watchdog-driven `shutdown(2)`,
  preventing destructor-time `SSL_shutdown()` on an unusable socket. (#648)
- **TLS stream zero-length I/O**: Base TLS `read()` and `write()` overloads now
  return success for zero-length operations without starting a handshake or
  OpenSSL I/O. (#747)

## [0.5.2] - 2026-07-01

Focus: **stability and security hardening**. 21 critical bug fixes addressing
use-after-free vulnerabilities, deadlocks, race conditions, and security issues
across sync primitives, I/O subsystem, network stack, and runtime.

### Security

- **HTTP DoS protection**: Added header count and size limits to prevent
  denial-of-service attacks via header flooding (#209)
- **WebSocket hardening**: Enforced `max_request_size`, added entropy validation,
  and improved UTF-8 checking to prevent malformed frame attacks (#219)

### Fixed

#### Critical Use-After-Free Vulnerabilities (5 bugs)

- **spawn_blocking UAF**: Coroutine frame destroyed while blocking operation
  in-flight caused use-after-free when worker thread attempted to resume. Fixed
  by replacing raw `blocking_state*` with `shared_ptr` and adding three-state
  claim protocol (kAlive → kResuming → kDone / kDead) to eliminate TOCTOU race.
  (#236)
- **channel send success flag**: `send()` returned `false` after successful
  direct-steal `recv()`, causing senders to incorrectly believe delivery failed.
  Fixed by setting `sender->success_ = true` in all 4 direct-steal paths. (#238)
- **condition_variable waiter UAF**: Waiter lifetime not properly tracked,
  leading to use-after-free when coroutine destroyed during notification. Fixed
  using intrusive list and proper synchronization. (#227)
- **chase_lev_deque destructor UAF**: Coroutine leak when scheduler shutdown
  before deque destruction. Fixed by ensuring proper cleanup order. (#201)
- **connect awaitables UAF**: Connection attempt completed after awaitable
  destroyed. Fixed by using `op_state` for lifetime tracking. (#213)

#### Deadlocks and Double-Resume (4 bugs)

- **mutex/object_cache double-resume**: Same waiter could be resumed multiple
  times, causing undefined behavior. Fixed by ensuring each waiter is resumed
  exactly once. (#229)
- **notify_waiter deadlock**: Moving notification outside mutex caused deadlock
  when waiter destroyed before notification delivered. Fixed by holding mutex
  during notification. (#223)
- **condition_variable deadlock**: Notification ordering could cause waiter to
  miss wake-up. Fixed by proper synchronization. (#227)
- **when_all await_suspend sync**: Race in await_suspend could cause deadlock
  when multiple tasks completed simultaneously. Fixed by using atomic
  synchronization. (#207)

#### TOCTOU and Race Conditions (3 bugs)

- **timer await_suspend and cancel race**: Cancel could race with await_suspend,
  causing timer to fire after cancellation. Fixed by proper atomic ordering. (#226)
- **I/O batch TOCTOU**: Race between batch completion and awaitable destruction.
  Fixed by adding `orphaned` flag. (#233)
- **file helpers close race**: File descriptor could be closed while operations
  still in-flight. Fixed by using reference counting. (#233)

#### Data Integrity (4 bugs)

- **file descriptor leak**: File descriptors not properly closed in error paths.
  Fixed by ensuring cleanup in all exit paths. (#233)
- **append_file write ordering**: Concurrent appends could interleave. Fixed by
  using atomic file operations. (#217)
- **TLS password lifetime**: Password buffer freed before SSL handshake
  completed. Fixed by extending lifetime with shared_ptr. (#215)
- **HTTP client write corruption**: Write buffer could be corrupted during
  concurrent writes. Fixed by proper synchronization. (#217)

#### Network Stack (3 bugs)

- **TCP/UDS listener accept issues**: Listener could miss connections or accept
  on closed socket. Fixed by proper socket state tracking. (#224)
- **HTTP2 dead buffer**: Pool held references to destroyed connections. Fixed
  by removing dead buffers from pool. (#211)
- **RPC session leak**: Sessions not properly cleaned up on connection close.
  Fixed by ensuring cleanup in all paths. (#221)

#### Runtime (2 bugs)

- **trampoline coroutine leak**: Trampoline coroutines not destroyed when
  scheduler shutdown. Fixed by ensuring proper cleanup. (#201)
- **WebSocket/SSE validation**: Missing validation of port, close code, and
  line endings. Fixed by adding comprehensive validation. (#195)

### Changed

- **CI workflow permissions**: Added explicit `permissions: contents: read` to
  follow principle of least privilege (#240)

### Testing

- **spawn_blocking UAF tests**: 252 lines of tests covering coroutine destruction
  scenarios, shared_ptr lifetime, and three-state claim protocol
- **channel success flag tests**: 222 lines of tests verifying send() returns
  correct success status in all recv() paths

---

## [0.5.1] - 2026-06-23

Focus: **concurrency safety hardening**. Comprehensive audit and fixes for
use-after-free vulnerabilities, race conditions, and lifetime management issues
across all synchronization primitives. Added intrusive list infrastructure for
O(1) safe waiter unlinking.

### Added

- **Intrusive list for waiter lifetime tracking**: New `detail::intrusive_list<T>`
  infrastructure enabling O(1) safe unlinking of waiters on coroutine destruction.
  Each waitable primitive now uses `intrusive_list_node` inheritance, allowing
  waiters to remove themselves from queues in destructors without use-after-free.
  Includes comprehensive cancellation safety tests (246 lines, 10 test cases).
  (#193)

### Fixed

#### Critical Concurrency Bugs (4 bugs, #176)

- **channel use-after-free**: `try_recv()` could schedule dangling coroutine handle
  after waiter destruction. Fixed by checking `is_linked()` before scheduling.
- **epoll iterator invalidation**: `submit()` could invalidate `fd_states_` iterators
  when processing multiple file descriptors. Fixed by using index-based iteration.
- **io_uring batch TOCTOU**: Race between batch completion and awaitable destruction.
  Fixed by adding `orphaned` flag and checking before scheduling.
- **scheduler draining deadline**: `draining_deadline_` could be read with relaxed
  ordering after being written with release ordering. Fixed by using acquire ordering.

#### Synchronization Primitives (8 bugs, #181, #187, #193)

- **event reset race**: `reset()` could race with `set()` due to missing mutex.
  Fixed by holding mutex during reset.
- **shared_mutex ordering**: Reader/writer wake ordering could violate fairness.
  Fixed by ensuring writers are woken before readers.
- **condition_variable timing**: `notify_one()` could miss waiters due to timing.
  Fixed by checking waiter count before and after notification.
- **channel capacity enforcement**: Bounded channel could exceed capacity due to
  race between `size()` check and `try_push()`. Fixed by re-checking capacity
  under lock. (#187)
- **channel try_push moved-from**: `try_push()` taking rvalue reference could leave
  value in moved-from state on failure. Fixed by taking lvalue reference and
  adding rvalue overload that preserves original value.
- **channel send fast path**: Bounded channel could exceed capacity when concurrent
  senders both pass unlocked size check. Fixed by re-checking capacity under lock.
- **All sync primitives use-after-free**: Comprehensive fix across all primitives
  (mutex, shared_mutex, semaphore, event, channel, condition_variable) using
  intrusive list for safe waiter lifetime tracking. (#193)
- **API compatibility**: Added rvalue overload for `try_push()` to preserve
  existing API compatibility while fixing moved-from bug.

#### Coroutine Combinators (2 bugs, #186)

- **when_all await_resume ordering**: Exception could be thrown before value
  extraction, causing value loss. Fixed by extracting values before rethrowing.
- **generator null guard**: Missing null check in generator destructor. Fixed by
  adding null guard.

#### Runtime and Scheduler (2 bugs, #188)

- **autoscaler task counting**: `task_count()` could race with task spawn/destroy.
  Fixed by using atomic operations.
- **autoscaler config race**: Config update could race with config read. Fixed by
  using mutex protection.

#### I/O Subsystem (2 bugs, #189)

- **epoll timer truncation**: Timer deadline could be truncated incorrectly.
  Fixed by using proper rounding.
- **epoll close ordering**: `close()` could race with pending operations.
  Fixed by ensuring operations complete before closing file descriptor.

### Changed

- **CI dependencies**: Bumped `actions/checkout` from 4 to 7 (#174),
  `dorny/paths-filter` from 3 to 4 (#173)
- **Dependabot configuration**: Added GitHub Actions updates to dependabot.yml

### Documentation

- **Cancellation safety**: Added comprehensive documentation for cancellation safety
  guarantees across all synchronization primitives in wiki and API reference
- **GitHub community files**: Added CONTRIBUTING.md, CODE_OF_CONDUCT.md,
  and SECURITY.md for better community governance

### Testing

- **Intrusive list unit tests**: 171 lines of comprehensive tests covering all
  intrusive list operations (push, pop, remove, splice, etc.)
- **Cancellation safety tests**: 246 lines of tests verifying safe waiter
  unlinking on coroutine destruction for all sync primitives

### Verification

All quality gates passed:
- ✅ 495 unit tests, 2820 assertions (up from 475 tests, 2771 assertions in 0.5.0)
- ✅ ASAN clean
- ✅ TSAN clean
- ✅ CI: 5/5 checks pass (x64/arm64 x Debug/Release + package-consumer)

## [0.5.0] - 2026-06-18

Focus: **correctness infrastructure and internal quality**. TSAN coverage
restoration, exception observability, real I/O cancellation, modularization of
the sync subsystem, and comprehensive bug fixes (74 bugs across all subsystems).

### Breaking Changes

- **`channel(0)` semantics changed to rendezvous**: `channel<T>()` and
  `channel<T>(0)` now create a synchronous rendezvous channel (Go-style
  hand-off: `send` suspends until a matching `recv` is ready). Use
  `channel<T>::unbounded()` to create an unbounded channel with no
  back-pressure. (#141)
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
  exceptions are logged at ERROR level. (#136, #139)
- **I/O awaitable `cancel_token` support**: `recv()`, `send()`, and
  `connect()` now accept an optional `cancel_token` parameter. Cancellation
  issues `IORING_OP_ASYNC_CANCEL` on io_uring backend. (#135)
- **Channel benchmark harness**: `examples/bench_channel.cpp` provides
  performance baselines for SPSC/MPMC throughput, contention scalability,
  and bounded vs unbounded vs rendezvous comparisons. (#144)

### Changed

- **TSAN multi-threaded coverage restored**: 10 sync primitive tests in
  `test_sync.cpp` now run with multi-threaded schedulers instead of
  single-threaded. Coroutine frame reuse false positives resolved via
  `__tsan_acquire`/`__tsan_release` annotations. (#137)
- **`condition_variable::wait` design clarified**: Both `wait(mutex&)` and
  `wait(Lock&)` overloads intentionally use different patterns. Documented
  as intentional design choice with rationale. (#138)

### Fixed

#### Sync Primitives (10 bugs, #150, #161)

- **channel rendezvous deadlock**: `send()` slow path did not suspend when
  no receiver was waiting.
- **channel bounded recv sender starvation**: When `try_push()` failed after
  popping from ring, sender remained in `send_waiters_` without wakeup.
- **channel bounded send closed race**: Fast path did not re-check `closed_`
  under lock.
- **channel close deadlock**: `items_available_.release()` called while
  holding mutex.
- **channel recv data loss**: `await_ready()` returning true skipped value
  extraction.
- **channel send slow path deadlock**: `unique_lock<std::mutex>` held
  across `co_await`.
- **LockfreeMPMCRing capacity=1 overflow**: Vyukov algorithm cannot handle
  single slot. Fixed by enforcing minimum capacity of 2.
- **condition_variable atomicity violation**: User mutex unlock occurred
  after enqueue.
- **mutex unlock dangling pointer**: Waiter address transfer could reference
  freed coroutine frame.
- **semaphore/shared_mutex count overflow**: Added overflow guards.
- **channel sender wakeup data loss**: When receiver pops and tries to wake
  a blocked sender, if `try_push` fails due to concurrent fill, the sender's
  value was lost. Fixed by only popping sender when transfer succeeds.

#### Coroutine Infrastructure (9 bugs, #149, #157, #162)

- **join_handle exception propagation data race**: `await_resume()` could
  destroy `join_state` while catch block accessed exception. Fixed by
  holding `shared_ptr` copy during `get_value()`.
- **cancel_token concurrent callback semantics**: Documented that callbacks
  may execute concurrently.
- **when_any winner selection race**: Removed unsafe fast path.
- **task promise destructor safety net**: Added `~promise_type()` that calls
  `mark_destroyed()`.
- **with_timeout result access guards**: Added assertions.
- **task return_value exception routing**: Wrapped in try-catch.
- **generator exception handling**: Removed redundant call.
- **promise_base frame chain clobbering**: Guarded restoration.
- **join_handle<T>::await_resume() TSAN race**: Same fix as void variant.

#### Runtime and Scheduler (9 bugs, #153, #155)

- **Chase-Lev TOCTOU race**: `get_next_task()` read `num_threads()` with
  relaxed ordering.
- **Chase-Lev pop_local missing fence**: Unsafe fast path removed.
- **autoscaler start/stop TOCTOU**: Non-atomic check-then-act replaced with
  CAS.
- **autoscaler config data race**: Protected with mutex.
- **scheduler set_thread_count deadlock**: Added runtime guard.
- **autoscaler success detection race**: Removed racy re-read.
- **mpsc_queue documentation**: Corrected "wait-free" → "lock-free".
- **work stealing test flakiness**: Increased task workload and added delay.
- **worker_thread steal decision synchronization**: Added acquire ordering.

#### I/O Subsystem (10 bugs, #154)

- **batch_read_awaitable UAF**: Added orphan protocol with atomic phase CAS.
- **batch I/O submit error handling**: Added rollback on failure.
- **cancellable I/O early cancel success**: Set `result_ = {-ECANCELED, 0}`.
- **epoll multi-pending-op consumption**: Only consume one op per event.
- **epoll EEXIST state**: Set `registered = true` on EEXIST.
- **epoll timer cancel key mismatch**: Added `cancel_key` field.
- **io_uring poll error logging**: Log submit errors.
- **epoll fd_state reset**: Reset after close.
- **cancellable I/O post-registration cancel**: Re-check and submit cancel.
- **timer submit_blocking rejection**: Destroy handle on rejection.

#### Network and Signal Handling (13 bugs, #151)

- **TCP/UDS accept_awaitable UAF**: Made inherit `io_awaitable_base`.
- **signal_wait_awaitable UAF**: Inherit `io_awaitable_base`.
- **signal masking incomplete**: Block at `worker_thread::run()` entry.
- **TCP connect_awaitable fd leak**: Added destructor.
- **UDS abstract socket address**: Added length-aware overload.
- **signal update() unblock**: Unblock removed signals.
- **UDS to_sockaddr truncation**: Throw on truncation.
- **signal ctx_ move semantics**: Changed to pointer.
- **hash HashDoS vulnerability**: Replaced with FNV-1a.
- **signal destructor SQE draining**: Use `close_fd_for_destructor`.
- **UDS unlink_on_bind warning**: Log on unexpected failures.
- **TCP listener close race**: Added op_state support.
- **UDS close/accept coordination**: Resolved by orphan protocol.

#### HTTP and WebSocket (19 bugs, #152)

- **HTTP server TLS capture**: Capture `shared_ptr<tls_context>`.
- **HTTP server send_response error handling**: Return bool.
- **HTTP server active connection tracking**: Added atomic counter.
- **HTTP server keep-alive logic**: Fixed edge cases.
- **HTTP server error handler safety**: Added null checks.
- **HTTP client pool mutex-across-suspension**: Extract before suspension.
- **HTTP client absolute response deadline**: Added enforcement.
- **HTTP client HTTPS→HTTP redirect**: Reject downgrades.
- **HTTP parser chunk CRLF validation**: Added for both parsers.
- **HTTP parser URI control character rejection**: Reject control chars.
- **HTTP common keep_alive parsing**: Token-based parsing.
- **WebSocket continuation frame validation**: Reject without initial frame.
- **WebSocket close code validation**: Reject reserved values.
- **WebSocket UTF-8 validation**: Validate text payloads.
- **WebSocket CSPRNG**: Use CSPRNG for mask keys.
- **WebSocket handshake CSPRNG**: Use CSPRNG for Sec-WebSocket-Key.
- **WebSocket server TLS capture**: Same as HTTP server.
- **WebSocket close response codes**: Map no_status to normal.
- **SSE server atomic state**: Changed to `std::atomic`.
- **SSE client retry overflow**: Added protection.
- **SSE client event dispatch**: Spec-compliant.
- **SSE client persistent backoff**: Persistent across reconnections.

#### RPC (5 bugs, #148)

- **RPC protocol version field**: Added and validated.
- **RPC client receive_loop leak**: Changed to `weak_ptr`.
- **RPC client ping timeout**: Added cancellation support.
- **RPC server cleanup callback leak**: Wrapped in try-catch.
- **RPC server send_response return**: Check write_frame return.

### Documentation

- **Wiki and README synchronization**: Automated audit found and fixed 106
  documentation mismatches across 17 files. (#156)

### Performance

- **Bounded channel lock-free fast path**: `channel::send()` and
  `channel::recv()` now use `LockfreeMPMCRing` for bounded channels,
  eliminating mutex contention on hot path. (#146)

### Internal

- **`shared_mutex::unlock` optimization**: Eliminated vector allocation in
  writer path. (#145)
- **TSAN cancel executor affinity**: Set affinity on cancel executors to
  prevent cross-worker stealing. (#147)

### Testing

- **Multi-threaded TSAN coverage**: Restored for sync primitive tests. (#137)
- **Work stealing test stabilization**: Fixed flakiness in Release builds. (#155)

### Verification

All quality gates passed:
- ✅ 475 unit tests, 2771 assertions
- ✅ ASAN clean
- ✅ TSAN clean (including arm64-Debug)
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
- RPC framework with zero-copy deserialization and buffer_ref support
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
