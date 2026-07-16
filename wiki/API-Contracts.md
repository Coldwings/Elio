# API Contracts

This page defines Elio's public API responsibility boundaries. Use it together
with the feature guides and header comments when triaging issues or reviewing
security-sensitive behavior.

Specific API documentation can narrow or extend these rules. When a feature
page or header gives a more specific contract, the more specific contract wins.
If no page says an object is safe for concurrent use, callers must serialize
mutable access to that object.

## How To Use This Page

- Treat "Elio guarantees" as library-side behavior. A regression against those
  guarantees is a bug or contract issue.
- Treat "Caller must guarantee" as an application precondition. Violating those
  preconditions is caller misuse unless Elio separately promises fail-closed
  behavior for that path.
- For ambiguous cases, clarify the contract first, then decide whether code
  changes are still required.
- Protocol input from peers is a library boundary. Application payload meaning
  after protocol parsing is an application boundary.

## Cross-Cutting Defaults

| Area | Elio guarantees | Caller must guarantee |
|------|-----------------|-----------------------|
| Object lifetime | Public APIs preserve memory safety when documented lifetimes are met. Awaiters clean up registered waiters when their owning coroutine frame is destroyed according to the API contract. | Keep borrowed buffers, spans, callbacks, streams, schedulers, TLS contexts, RDMA objects, and other externally owned resources alive for the documented duration. |
| Thread safety | APIs documented as concurrent-safe protect their own internal state. Protocol send helpers that document frame serialization prevent interleaved wire frames. | Unless documented otherwise, serialize access to mutable objects from multiple coroutines or threads. Do not issue overlapping operations on non-concurrent streams or clients. |
| Cancellation and timeouts | APIs with `cancel_token` or timeout parameters honor the documented cancellation point or deadline and return the documented cancellation or timeout result. | Pass tokens into operations that support cancellation. A wrapper timeout does not forcibly destroy a child operation that has no cancellation-aware wait point. |
| Results and terminal states | Operations report errors through their documented result type, optional return, `errno`, or exception path. Protocol objects transition to closed/error states as documented. | Check operation results before using returned values. Do not continue using an object after a documented terminal state unless the API documents reuse. |
| Resource limits | Configured size limits, session limits, queue limits, and deadlines are enforced at the documented layer. | Choose limits and timeouts that fit the application's threat model. Unlimited or zero-disabled limits are an application policy choice. |
| Low-level fast paths | High-level helpers validate protocol and transport boundaries before dispatching to application handlers. | Raw parser, RDMA, SGE, buffer-view, and verbs-adjacent helpers may require valid caller inputs for performance. Use them only with documented preconditions satisfied. |

## Runtime, Coroutines, And Timers

| Interface family | Elio guarantees | Caller must guarantee |
|------------------|-----------------|-----------------------|
| `coro::task<T>` and direct `co_await` | Task awaiters resume the awaiting coroutine and deliver returned values or rethrow stored exceptions through the documented await path. Destroying an uncompleted task destroys its coroutine frame. | Do not copy or move task objects except where a type explicitly supports it. Keep referenced objects alive across suspension points. |
| `coro::generator<T>` | Generator awaiters deliver yielded values in producer order and surface completion through the documented `next()` result. | Consume the generator according to its single-consumer contract unless a specific generator API documents sharing. Keep yielded references or views alive only for their documented lifetime. |
| `coro::cancel_source` and `coro::cancel_token` | Cancellation state is shared safely across registered callbacks and cancellation-aware operations observe cancellation according to their own API contract. | Cancellation is cooperative. Pass tokens into operations that support them, keep callback captures valid, and do not assume an operation without token support will stop. |
| `elio::when_all`, `elio::when_any`, `elio::with_timeout` | Coroutine composition helpers launch the supplied task-producing callables, collect documented results, propagate exceptions through the await path, and request cooperative cancellation of losing or timed-out branches where supported. | Supply callables that return Elio tasks and keep captured state valid until all launched branches finish or observe cancellation. Treat `with_timeout` as cooperative; it cannot forcibly destroy a child task that ignores cancellation. |
| `runtime::scheduler`, `run()`, `async_main` | Scheduler workers run submitted coroutine tasks and perform orderly shutdown according to the scheduler API. Per-worker I/O contexts route operations through the active worker backend. | Start the scheduler before submitting work that requires it, shut it down intentionally, and keep scheduler-owned objects alive while tasks use them. |
| `runtime::autoscaler` | The autoscaler samples scheduler state, applies configured triggers/actions, and serializes its own config updates according to the runtime autoscaler API. | Keep the scheduler alive while the autoscaler is running. Choose min/max worker bounds and trigger thresholds that fit the workload. Ensure custom actions and callbacks are safe under scheduler resizing. |
| `elio::go()`, `elio::spawn()`, `join_handle` | Spawn helpers store callable arguments in the coroutine frame and return results or exceptions through the documented await path. | Keep referenced external objects alive for detached work. Prefer explicit value captures for background tasks that outlive the calling scope. |
| `spawn_blocking()` and blocking pool | Blocking work submitted through the helper is run outside scheduler worker execution and resumes the awaiting coroutine with the callable result or exception. | Use it only for work that may block or consume CPU outside the event loop. Keep callable captures alive according to normal C++ value/reference rules. |
| `ELIO_GO`, `ELIO_GO_TO`, `ELIO_SPAWN` | Macros provide inline spawn syntax. | Because the macros use reference captures, every referenced object must outlive the spawned coroutine. |
| `go_to()` and affinity helpers | Affinity requests are respected according to the scheduler's documented placement behavior. | Use valid worker IDs when deterministic pinning matters and handle resizing or fallback behavior documented by the scheduler. |
| `time::sleep_for`, `sleep_until`, `yield` | Timer awaiters wake at the requested time or cancellation point supported by the operation. | Pass cancellation tokens into timer operations that support cancellation. Do not assume an unrelated wait primitive is cancellable unless it accepts a token. |

## Signal Handling

| Interface family | Elio guarantees | Caller must guarantee |
|------------------|-----------------|-----------------------|
| `signal::signal_set` and `signalfd` helpers | Signal helpers integrate Linux `signalfd` delivery with coroutine waits and return observed signals through the documented result path. | Block the relevant signals in threads that should route them through `signalfd`. Coordinate process-wide signal masks with other application code and libraries. |
| `serve()` shutdown helpers | Serve helpers use configured shutdown signals to stop listeners and allow orderly shutdown according to the server API. | Choose the signal set and shutdown deadline appropriate for the process. Ensure application tasks observe shutdown and release resources. |

## Networking And Streams

| Interface family | Elio guarantees | Caller must guarantee |
|------------------|-----------------|-----------------------|
| TCP and UDS listeners | Bind helpers configure sockets according to supplied options and accept connections asynchronously. Listener close/shutdown stops future accepts as documented. | Choose bind addresses, filesystem socket paths, permissions, backlog, and socket options appropriate for the deployment. |
| `tcp_stream`, `uds_stream` base I/O | `read()`, `write()`, and `writev()` perform one readiness-aware operation and report the actual byte count or error. | Treat positive short reads/writes as normal stream behavior. Loop or use exact-length helpers when a full protocol message must be transferred. |
| Exact-length stream helpers | `read_exactly()` and `write_exactly()` continue until the requested byte count, EOF, cancellation, or error according to the helper contract. | Use these helpers for fixed-size protocol fields or complete message writes. Keep buffers valid until the operation finishes. |
| DNS resolution | Resolver helpers return resolved addresses or an empty/error result according to the resolver contract. | Decide address ordering, retries, and application policy when multiple addresses are available. |
| `io_context` and backends | io_uring and epoll backends submit supported operations and resume awaiters with completion results. | Do not bypass backend ownership rules. Keep operation buffers and file descriptors valid until completion unless an API documents transfer or cancellation behavior. |

## TLS

| Interface family | Elio guarantees | Caller must guarantee |
|------------------|-----------------|-----------------------|
| `tls_context` | Context helpers configure OpenSSL state, certificate paths, ALPN, and verification settings according to their return values. | Keep the context alive while TLS streams or listeners use it. Load trusted CA paths and certificates for production deployments. |
| `tls_stream` | TLS streams perform handshake, encrypted reads/writes, and bounded shutdown according to the TLS stream contract. | Do not issue concurrent operations on one TLS stream unless the API documents that pattern as safe. Keep plaintext buffers valid until operations complete. |
| Certificate verification | High-level client defaults keep verification enabled where practical. | Disable verification only for controlled tests. Hostname, trust-store, private-key, and certificate-file policy belongs to the application deployment. |

## HTTP/1.1

| Interface family | Elio guarantees | Caller must guarantee |
|------------------|-----------------|-----------------------|
| HTTP parser and server request intake | Elio parses request lines, headers, bodies, and protocol framing before dispatching a request handler. Malformed HTTP is rejected at the parser/server boundary. | Validate application routes, authorization, allowed methods, content schema, and business rules in handlers. |
| HTTP server limits | Configured request, header, body, keep-alive, and session limits are enforced at their documented layer. | Pick limits appropriate for exposed services. A disabled limit or unbounded timeout is an application policy choice. |
| HTTP responses | Response serialization preserves status, headers, and body framing generated through Elio response APIs. | Do not construct invalid application headers or bodies. Avoid exposing secrets or unsafe redirects from handler logic. |
| HTTP client | Client helpers perform connection setup, optional TLS, request serialization, response parsing, and timeout handling according to config. | Check response status and errors. Decide retry, redirect, authentication, and application payload validation policy. |
| Raw HTTP helpers | Raw parser/building helpers expose low-level protocol pieces without full server/client policy. | Use raw helpers only with valid inputs and application-level validation around them. |

## HTTP/2

| Interface family | Elio guarantees | Caller must guarantee |
|------------------|-----------------|-----------------------|
| `h2_client` | The client negotiates ALPN/TLS, initializes the HTTP/2 session, validates frames through nghttp2, and reports response or error results. | Avoid concurrent use of one high-level `h2_client` unless a future API documents shared-session multiplexing. Use separate clients for parallel application work. |
| HTTP/2 settings and limits | Configured stream limits, read timeouts, and connection setup timeouts are applied as documented. | Choose settings that match the peer and workload. Validate response status, headers, and body semantics at the application layer. |
| nghttp2 dependency | Elio maps nghttp2 protocol validation into the client/session result path. | Provide the dependency through the documented build or package mode and handle provider-specific deployment constraints. |

## WebSocket

| Interface family | Elio guarantees | Caller must guarantee |
|------------------|-----------------|-----------------------|
| WebSocket handshake | Server and client helpers validate HTTP upgrade requirements, selected subprotocols, masking rules, and configured handshake deadlines before opening the connection. | Register only intended routes and subprotocols. Validate origin, authorization, session identity, and application policy in the application layer. |
| WebSocket frames | Elio validates opcodes, reserved bits, masking direction, control-frame rules, fragmentation state, message size, and close behavior before exposing messages to handlers. | Validate text encoding, JSON/schema, authorization, and business meaning of message payloads after `receive()`. |
| `ws_connection` send helpers | Server-side send helpers serialize concurrent sends so frames produced by multiple senders do not interleave on the wire. | Keep the connection object alive while operations run and stop sending after close/error results. |
| `ws_client` | Client connect/receive/send/close operations maintain client protocol state and fail closed on invalid peer frames. Parser state is reset for new connection attempts as documented by client behavior. | Serialize use of one client unless a method documents concurrent safety. Reconnect policy, backoff, idempotency, and replay of application messages belong to the caller. |
| Raw frame parser | Parser helpers identify WebSocket protocol errors and incomplete input. | Feed bytes in order and respect parser terminal error states. Raw parser use does not replace application payload validation. |

## Server-Sent Events

| Interface family | Elio guarantees | Caller must guarantee |
|------------------|-----------------|-----------------------|
| SSE server | Elio serializes events into SSE wire format and prevents concurrent senders from interleaving frames where documented. | Decide event names, ids, retry values, authorization, and payload schema. Stop producing events when the connection is inactive. |
| SSE client | The client parses SSE response headers and event stream syntax and applies configured connection/read deadlines. | Browser-style reconnection policy, last-event-id persistence, duplicate handling, and application event validation belong to the caller unless a specific API documents otherwise. |

## RPC

| Interface family | Elio guarantees | Caller must guarantee |
|------------------|-----------------|-----------------------|
| RPC frame parser | Elio validates frame type, flags, payload length, checksum presence, and configured maximum message size before dispatching frames. | Treat CRC32 as accidental corruption detection, not authentication or tamper resistance. Use TLS, message authentication, or an application security layer for hostile peers. |
| `rpc_client` | Concurrent calls are correlated by request ID, timeouts are reported through `rpc_result`, and stale timed-out responses are not matched to unrelated future calls. | Keep the client alive for outstanding calls. Check `rpc_result::ok()` before reading values. Bound total in-flight calls according to application resource policy. |
| `rpc_server` | Registered handlers are dispatched for valid method frames and session limits/timeouts are applied as configured. | Assign stable unique method IDs, version schemas, validate authorization and business invariants inside handlers, and keep handler-captured objects alive. |
| Serialization buffers | Serialization and deserialization follow declared `ELIO_RPC_FIELDS` order and supported type rules. | Use compatible schemas on both sides. Do not share mutable serializer/buffer objects across threads unless documented safe. Keep `buffer_ref` backing storage alive through the documented send/cleanup point. |

## Synchronization Primitives

| Interface family | Elio guarantees | Caller must guarantee |
|------------------|-----------------|-----------------------|
| `sync::mutex`, `shared_mutex`, `semaphore`, `event`, `condition_variable` | Awaiters are resumed according to the primitive's documented ordering, cleanup, and cancellation support. | Use the primitive that matches the concurrency invariant. Do not destroy synchronization objects while waiters can still reference them unless the API documents safe teardown. |
| `sync::channel` | Channel send/receive/close operations follow the documented capacity and close semantics. | Handle closed-channel results. Keep message values and channel objects alive according to move/lifetime rules. |
| `sync::object_cache` | Cache lookups coalesce construction per key, return move-only borrows with reference-counted lifetime tracking, and apply explicit eviction, TTL, and reclaim behavior according to the cache configuration. | Keep constructor callables and captured state valid while construction runs. Release borrows promptly, choose TTL/reclaim settings for the workload, and treat cached object contents and eviction policy as application-level state. |
| `sync::spinlock` | The spinlock provides a thread-blocking atomic lock for very short critical sections. | Do not hold a spinlock across `co_await` or long-running work. Prefer coroutine-aware primitives for code that may suspend or contend. |
| Low-level lock-free primitives | Lock-free queues/rings provide the documented producer/consumer shape and memory-ordering guarantees. | Respect the declared producer/consumer cardinality. Do not use internal runtime primitives as general-purpose synchronization unless documented public. |

## File Helpers And Batch I/O

| Interface family | Elio guarantees | Caller must guarantee |
|------------------|-----------------|-----------------------|
| File read/write helpers | Helpers perform chunked or batch I/O according to the documented result semantics and avoid single unbounded kernel requests where documented. | Validate paths, permissions, expected file size, overwrite policy, and trust of file contents. Keep buffers and paths valid until the operation completes. |
| Batch operations | Batch helpers submit supported operations and return per-operation results. | Interpret partial success and retry policy at the application layer. Do not assume all operations are atomic as a group unless a helper documents that behavior. |

## Hash Functions

| Interface family | Elio guarantees | Caller must guarantee |
|------------------|-----------------|-----------------------|
| CRC32 | CRC32 helpers compute the documented checksum over supplied bytes and iovecs. | Use CRC32 only for accidental error detection or protocol compatibility. Do not use it as a security boundary. |
| SHA-1 and SHA-256 | Hash helpers compute the documented digest over supplied bytes and incremental updates. | Choose a hash that matches the security goal. SHA-1 is not collision-resistant for new security designs. Keep input spans valid for the call. |

## RDMA, RDMA-CM, And CUDA RDMA

| Interface family | Elio guarantees | Caller must guarantee |
|------------------|-----------------|-----------------------|
| RDMA backend traits | Awaiters normalize backend post failures into documented completion results without throwing. | Backend `post_*` methods must be `noexcept`, follow the required return convention, and keep QP/CQ/backend objects alive while operations can complete. |
| RDMA `connection` operations | High-level single-buffer operations fail before posting when a 32-bit SGE length precondition is not met. Started operations preserve synchronous post failures until awaited. | Keep SGE arrays and payload buffers alive for the hardware operation. Split oversized local buffers into valid SGEs. Ensure remote keys, addresses, and access rights are valid. |
| Memory regions and remote buffers | RAII wrappers deregister memory according to their ownership contract. | Keep registered memory alive while any local or remote operation can access it. Bounds for fast-path subviews and remote offsets are caller-owned where documented. |
| RDMA-CM helpers | CM wrappers expose lifecycle and event handling in coroutine-friendly form. | Follow RDMA-CM state-machine requirements, close/destroy IDs in valid states, and keep event channels/listeners alive while operations reference them. |
| CUDA RDMA helpers | CUDA-specific wrappers expose GPU buffer and memory-region helpers when the feature is enabled. | Ensure CUDA context, device pointer validity, registration support, peer access, and driver/runtime compatibility. |

## Logging And Debugging

| Interface family | Elio guarantees | Caller must guarantee |
|------------------|-----------------|-----------------------|
| Logging macros and logger | Logger internals serialize writes according to the logging implementation. | Avoid logging secrets. Configure log level and sinks according to deployment requirements. |
| Virtual stack/debug helpers | Debug helpers expose Elio coroutine-frame metadata best effort. | Treat debug output as diagnostic data, not a stable application protocol or security boundary. |
