# Migrating To 0.6

## Finishing Stream Output

Use `co_await stream.finish_write(token)` to finish application output without
branching on TCP versus negotiated TLS version. TCP/TLS 1.3 preserve reading;
TLS 1.2 closes the session normally, not as an unsupported half-close error.
The result reports scope, error and observed closure, not peer receipt or
lossless relay completion. One reader may overlap; serialize other writers
and lifetime changes. TLS 1.2 peer closure also freezes new plaintext writes
and drains the already BIO-accepted ciphertext prefix and alert within
`tls_stream_options::session_close_timeout` (five seconds by default). Legacy
`shutdown()`/`close()` remain separate serialized whole-close APIs.

## HTTP Complete And Streaming Replies

`http::reply` now has a third alternative, `tunnel_response`. Update exhaustive
visitors and code that assumes every non-complete reply is streaming. Ordinary
`send_response()` rejects tunnel replies and ordinary 2xx CONNECT responses;
use `router.connect()` plus a `tunnel_response` for server-owned acceptance.
Return an ordinary non-2xx response for rejection. Registrations accept
`response`, `streaming_response`, `tunnel_response` or `reply`, including their
`task` forms; a registered handler remains copyable while its returned session
callable may be move-only.

Replace manual socket extraction, acceptance-header writes and post-CONNECT
parser loops with the scoped `tunnel_stream` callback. The server writes the
head first and supplies captured binary read-ahead exactly once. Keep payloads,
descriptors and the view alive through joined operations; full tunnel writes
report confirmed bytes plus uncertainty on failure, not a replayable suffix.
Use the optional two-buffer `relay()` only after applying application-specific
authorization and upstream connection policy. See
[CONNECT handoff](HTTP-Streaming.md#connect-tunnel-handoff) and
[the proxy example](https://github.com/Coldwings/Elio/blob/main/examples/http_connect_proxy.cpp).

For direct `prepare_response` callers, `body_description::length` defaults to
`std::nullopt`, not zero. `{.kind = response_body_kind::streaming}` therefore
describes an unknown-length stream. Supply `.length = uint64_t{0}` explicitly
for a known-empty stream or complete body. A default complete description
without a length is invalid and returns `EINVAL` without generated headers.
Ordinary `response` sending already supplies its actual body length.

CONNECT request parsing now requires a valid authority with an explicit
destination port: use `CONNECT example.com:443`, not `CONNECT /`. Invalid
authorities, Transfer-Encoding and positive Content-Length are rejected before
request-body accumulation. CL:0 remains accepted. Read the preserved authority
from `path()` and retain post-header bytes via `take_remaining()` when owning a
low-level handoff. Syntax acceptance does not resolve or authorize the target,
and low-level parsing alone does not perform a tunnel handoff. The server-owned
API above supplies that separate lifecycle; HTTP/2 CONNECT is not included.

`response` now owns a complete body; its constructors and `set_body()` no longer
generate Content-Length in metadata. The shared outgoing plan derives that
length when sending or serializing. A manually supplied Content-Length remains
a checked assertion: changing the body does not silently replace it.

`streaming_response` owns a producer and an optional declared byte count.
Unknown HTTP/1.1 streams default to chunked; explicit close delimiting belongs
to this streaming transfer policy. `reply` is the move-only variant. Router
callbacks accept either concrete response or `reply`, synchronously or through
`task`. Explicitly named `handler_func` now returns `task<reply>`; callbacks
returning `task<response>` still work through registration adapters.

The complete-response `set_close_delimited()`/`close_delimited()` and
`sse::build_sse_response()` are removed. Do not substitute a Transfer-Encoding
header or raw writes after an ordinary response. Return
`sse::make_streaming_response(producer)` from the route; the producer takes a
scoped `sse::event_writer&` and cancellation token and returns `task<send_result>`.
See `examples/sse_server.cpp` for a compilable migration. CORS permission must
now be set explicitly. The new event writer rejects CR/LF/NUL in id/type instead
of silently omitting invalid fields as the legacy serializer did.

`response::serialize()` remains an explicit string-materialization operation;
invalid CL/TE framing throws `std::invalid_argument`. Server sending avoids
that body-sized string. Received responses preserve original headers: explicitly
remove Transfer-Encoding before reserializing decoded chunked content, along
with any obsolete Content-Length assertion. No implicit normalization hides
this decision. HEAD/304 representation metadata can be supplied through
`set_representation_length()`; an explicit nonzero CL on 205 is rejected rather
than overwritten. Interim/101/tunnel serialization is a separate headers-only
path, not an ordinary final reply.

Moving a HEAD/304 length from a manual header to
`set_representation_length()` does not establish its accuracy. Keep that value
consistent with the corresponding selected GET representation. Elio checks
framing consistency but does not generate the hypothetical GET body to verify
it. Ordinary HEAD with unknown body length requires explicit representation
metadata if CL is to be advertised; unlike 304, manual CL alone is rejected.

Never detach or concurrently use a writer. Borrowed buffers and descriptors
remain alive through each write and its cleanup. Context survives producer
execution, but `send_interim()` returns `EALREADY` after final selection.
Recoverable short-write progress is internal; terminal errors do not authorize
replay. Per-write timeout defaults to disabled and is independent of producer
lifetime. On shutdown, request `stop()`, await listeners, then drain sessions
before destroying server/TLS state. Cancellation cannot forcibly destroy a
noncooperative producer. See [[HTTP Streaming]] for the complete boundaries.

### Framing Setters And Checked Lengths

The examples below use `using namespace elio;` and `using namespace
elio::http;`. Current-side counterparts, including the necessary headers,
live in `tests/compile/http_streaming_migration.cpp`. Historical snippets
explain the migration only; snippets using removed APIs are explicitly not
compiled against the current library.

Before: response body setters generated or replaced the length header. Code
could accidentally depend on a body change repairing an old assertion:

```cpp
// Historical behavior; the calls still compile, but the assumption is obsolete.
auto value = response::ok("old");
value.set_header("Content-Length", "3");
value.set_body(std::string_view("longer"));
// Previously: the setter replaced Content-Length with 6.
```

After: remove an obsolete assertion deliberately and let preparation derive
the new length. Alternatively, set a new truthful assertion yourself. With no
manual assertion, `response::ok()` is an empty complete response and sends
`Content-Length: 0`; it does not reserve a future streaming body.

```cpp
response changed_body() {
    auto value = response::ok("old");
    value.set_header("Content-Length", "3");
    value.set_body(std::string_view("longer"));
    value.get_headers().remove("Content-Length");
    return value; // sending derives Content-Length: 6 without changing metadata
}
```

Leaving the old `3` assertion in place makes ordinary `serialize()` throw
`std::invalid_argument`. The same invalid reply fails server send preflight
before final bytes; it is not silently repaired or sent as a second response.
For explicit serialization, distinguish invalid input from allocation failure:

```cpp
bool stale_length_is_rejected() {
    auto value = response::ok("old");
    value.set_header("Content-Length", "3");
    value.set_body(std::string_view("longer"));
    try {
        (void)value.serialize();
    } catch (const std::invalid_argument&) {
        return true;
    }
    return false;
}
```

Before, a manual `Transfer-Encoding: chunked` could be mistaken for an encoder
selection. It was not safe to attach that header to an ordinary body and then
send unencoded payload. After, ordinary final preflight rejects manual TE;
choose unknown-length production instead:

```cpp
streaming_response chunked_reply() {
    return streaming_response(status::ok,
        [data = std::string("hello")](body_writer& writer,
            coro::cancel_token token) -> coro::task<send_result> {
            co_return co_await writer.write(data, token);
        }); // HTTP/1.1 selects chunked; HTTP/1.0 selects close delimiting
}
```

### Received Headers Are Not Outgoing Encoder Instructions

Before, directly reserializing an accumulated chunked response could preserve
TE while writing its already-decoded payload without chunk framing. That was
an unsafe forwarding idiom, not a wire-format guarantee:

```cpp
// Historical unsafe idiom; not a valid current chunked forwarding operation.
auto received = response::from_parser(parser);
auto wire = received.serialize();
```

After, require successful receive completion first. For a complete ordinary
GET response whose body has already been transfer-decoded, explicitly remove
received TE and the length assertion when choosing fresh complete-body framing:

```cpp
std::string serialize_decoded_body(response received) {
    received.get_headers().remove("Transfer-Encoding");
    received.get_headers().remove("Content-Length");
    return received.serialize(); // derives CL from decoded complete body
}
```

The by-value parameter leaves the original received response unchanged.
Neither `from_parser()` nor `from_decoder()` performs this normalization
implicitly, and `from_decoder()` expects an already-decoded body. Do not use
this example for HEAD/304 representation metadata or protocol handoff. It is
not a general proxy implementation: other hop-by-hop headers, content
encoding, application policy and trust boundaries remain caller duties.

### Replace Close Markers And Header-Only SSE

Before (removed API; intentionally non-compiling with the current library):

```text
response headers(status::ok);
headers.set_close_delimited();
headers.set_header("Connection", "close");
// Application separately sends headers and raw body bytes.
```

After, close delimiting belongs to an unknown-length streaming response. Do
not supply CL or TE; the shared plan emits `Connection: close` and prevents
reuse. Use automatic framing unless close delimiting is actually required:

```cpp
streaming_response close_delimited_reply() {
    return streaming_response(status::ok,
        [data = std::string("hello")](body_writer& writer,
            coro::cancel_token token) -> coro::task<send_result> {
            co_return co_await writer.write(data, token);
        }, std::nullopt, response_transfer::close_delimited);
}
```

Before (removed helper; intentionally non-compiling with the current library):

```text
auto headers = sse::build_sse_response();
// Application separately drives a raw sse_connection after sending headers.
```

After, register an owned SSE producer as a normal HTTP route. Ordinary
`task<response>` handlers still work through router adapters; only an explicitly
named `handler_func` must now return `task<reply>`. There is no task covariance.

```cpp
void register_migrated_routes(router& routes) {
    routes.get("/ordinary", [](context&) -> coro::task<response> {
        co_return response::ok("ordinary");
    });
    handler_func selected = [](context&) -> coro::task<reply> {
        co_return chunked_reply();
    };
    routes.get("/stream", std::move(selected));
    routes.get("/events", [](context&) {
        return sse::make_streaming_response(
            [](sse::event_writer& writer, coro::cancel_token token) -> coro::task<send_result> {
                const auto sent = co_await writer.send_data("hello", token);
                if (!sent.success()) co_return sent;
                co_return send_result{};
            });
    });
}
```

The factory adds no CORS permission. Set any application-authorized CORS
headers on the returned response before selection; never add raw socket
writes alongside the managed SSE writer.

Managed SSE `event_view::id` uses `std::optional<std::string_view>` to
distinguish omission from an explicit Last-Event-ID reset. `{}` and
`std::nullopt` omit id; an empty string/view emits `id:\n` and resets it.
Use `std::nullopt` instead of an empty-string sentinel when omission is intended.
For a possibly empty ID that should retain the legacy omission behavior, use
`id.empty() ? std::nullopt : std::optional<std::string_view>{id}`.
Nonempty IDs remain borrowed, and `send_data()` omits id. Data is always
emitted: this API does not introduce an id-only event operation.

### Borrowed Buffers And Failure Propagation

Before, code directly managing a transport had to advance partial `iovec`
progress itself; attempting that same remainder/replay loop around the new
HTTP writer would mix two different completion contracts. Likewise, returning
an ordinary error response after starting raw output cannot repair that output.

After, a logical HTTP body write completes all submitted slices or fails.
Keep both the buffers and descriptor storage alive until its await returns,
including cancellation cleanup. Reuse them only after that boundary:

```cpp
coro::task<send_result> borrowed_write(body_writer& writer, coro::cancel_token token) {
    std::string first = "hello ";
    std::string second = "world";
    const std::array<body_buffer, 2> parts{{
        {first.data(), first.size()}, {second.data(), second.size()}}};
    const auto sent = co_await writer.writev(std::span<const body_buffer>(parts), token);
    if (!sent.success()) co_return sent;
    first.assign("safe to reuse after the awaited operation and cleanup");
    co_return sent;
}
```

Pass the supplied token into source waits as well as writes. There is no
public `finish()`: successful producer return delegates finalization to the
server, which still checks the writer and declared length. A failed write
stays terminal even if its result is ignored or a producer catches an
allocation exception. A source error after headers must also be propagated:

```cpp
streaming_response failing_source() {
    return streaming_response(status::ok,
        [](body_writer& writer, coro::cancel_token token) -> coro::task<send_result> {
            const auto sent = co_await writer.write("prefix", token);
            if (!sent.success()) co_return sent;
            // Simulates a source error discovered after final headers.
            co_return send_result{send_errc::producer_error, EIO};
        });
}
```

This is an intentionally failing response, not a retry recipe: the prefix may
already be visible, the normal chunk terminator is not deliberately emitted,
and the connection is not reusable. Open/authorize sources before returning
a streaming reply when failure should instead select an ordinary error status.
See [[HTTP Streaming]] for the pre-/post-header recovery state table.

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

## TLS Duplex Output And Close Budgets

TLS low-level `write()` may now return positive short progress of at most
16 KiB, even for a larger input. TLS `writev()` likewise returns progress from
the first nonempty slice. Use `co_await stream.write_exactly(data, size, token)`
when a complete logical write is required. HTTP `body_writer` already handles
partial progress; no new application retry loop is required there.

The optional third `tls_stream` constructor argument accepts
`tls_stream_options{.ciphertext_budget = 1024 * 1024}`; 1 MiB is the default.
This on-demand budget covers retained custom-BIO ciphertext payload, including
consumed block prefixes and outstanding drain leases until the full block is
freed. It excludes queue metadata, OpenSSL and kernel memory. Direct output
avoids the queue when possible; no caller plaintext is retained by a background
output pump. `ENOBUFS` or `ENOMEM` is terminal, not a request to retry with a
larger budget on the same connection. The first transport failure stays sticky,
and operations ending in transport failure await owned output-I/O cleanup
before returning.

TLS cancellation now has an explicit connection boundary: cancellation detected
by the initial cancellation check is operation-local; cancellation observed
after that check is terminal for the connection and its overlapping reader/writer,
even when the sibling uses another token. Await both, then establish a new
stream. Completed slices are not rolled back. Exact helpers' cancellation checks
between completed slices remain local because no SSL operation is unfinished.

Read success need not wait for unrelated ciphertext output; handshake success
establishes local TLS state but final control records may still be draining.
Neither result proves peer receipt. Keep the stream and borrowed plaintext
alive through public operations even though internal transport ownership now
retains the physical descriptor through pending internal I/O cleanup.

Legacy `shutdown()` still returns `task<void>`. Its default is now one 5-second
whole-session close budget, not a new budget for each retry. Expiry starts abort
and cleanup; it does not promise return within exactly 5 seconds or lossless
delivery. Serialize it with reads/writes. Destruction aborts owned output, does
not asynchronously finish normal shutdown, and never makes forced destruction
of an active caller coroutine safe. This API is not directional half-close or
CONNECT tunnel support.
After whole-session shutdown the transport is retired; establish a new stream
for another TLS session rather than calling `handshake()` on the closed one.

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

Receive-side migration and the implemented outgoing producer/writer are
separate parts of [#1191](https://github.com/Coldwings/Elio/issues/1191). Apply
the HTTP complete/streaming migration above as well: server serialization now
uses shared framing preflight, and the old close marker/SSE header helper are
removed.

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
