# HTTP Streaming

## Implemented Scope

Elio's HTTP/1 receive path provides a shared incremental response decoder and
a pull reader. Ordinary HTTP clients explicitly accumulate its body slices;
SSE clients incrementally parse those slices as events. This is the receive
phase of [#1191](https://github.com/Coldwings/Elio/issues/1191), tracked in
[#1192](https://github.com/Coldwings/Elio/issues/1192).

The sending path uses a shared response plan and server-managed producer/writer
lifecycle. This is HTTP/1 behavior, not an HTTP/2 streaming API or an end-to-end
zero-copy guarantee.

## Outgoing Response Ownership

`response` owns a complete body. Its constructors and `set_body()` change body
storage without generating Content-Length in the metadata. `response_head`
contains metadata only. A move-only `streaming_response` owns its producer and
declares either a known byte count or an unknown length. `reply` is the variant
of complete and streaming responses.

Router registrations accept synchronous or `task` results of all three reply
shapes through explicit adapters. Registered handler callables are copyable;
the producer returned by a handler may be move-only. A context remains alive
through producer execution and send cleanup. Once the handler selects its final
reply, `send_interim()` is sealed and returns false with `EALREADY`.

The server invokes a producer at most once. HEAD and body-forbidden statuses
skip it. Open files, authorize access, and do work that can change final status
before returning a streaming response. A failure after headers cannot be repaired
by sending another final response on that connection.

The producer receives `body_writer&` and a cooperative cancellation token. It
returns `task<send_result>`. Each `write`/`writev` is sequential and borrowed:
keep both descriptor storage and payload valid and immutable until the await
returns, including failure cleanup. Never detach a write, escape the writer,
or use it concurrently. Empty writes do not end the response. Only successful
producer return delegates final framing to the server; there is no public
`finish()` operation.

## Framing And Failure

| Response | Effective framing |
| --- | --- |
| Complete body or known-length stream | Derived/declared Content-Length, with exact byte-count enforcement |
| Unknown-length HTTP/1.1 stream | Chunked |
| Unknown-length HTTP/1.0 stream | Close-delimited, no connection reuse |
| Explicit close-delimited stream | No CL/TE, no connection reuse; only for unknown length |
| HEAD | No body or producer invocation; length describes the representation |
| 204 | No body or framing length |
| 205 | No body, Content-Length: 0 |
| 304 | No body; optional explicitly supplied representation length |

Manual Content-Length is a validated assertion, not an encoder switch. Invalid
or conflicting assertions and manually supplied Transfer-Encoding are rejected
before final output. Ordinary final planning rejects 1xx and successful CONNECT;
interims and WebSocket 101 handoff have separate paths.

Logical writes absorb short writes and EINTR; readiness-aware transports handle
EAGAIN without busy spinning. No caller remainder retry or whole-response replay
is required or supported. An unrecoverable error permanently terminates the
writer. Too-long production is rejected before sending the excess; too-short
production fails finalization. Failure prevents deliberate emission of a normal
chunk terminator and prevents HTTP connection reuse.

`confirmed_body_bytes` is cumulative diagnostic progress, not a replay offset.
It can increase when an underlying completion arrives after timeout wins.
The winning cancellation/timeout/error result remains terminal; already-sent
bytes cannot be rolled back and may already have reached the peer.

`server_config::write_timeout` defaults to zero (disabled). Positive values apply
to each logical write under one original deadline across partial progress, not
to the whole response or an SSE stream. Timeout requests abort; return still
waits for transport and watchdog cleanup. It is not a hard upper bound on return
time. Coroutine-frame allocation can throw before returning a task; the writer
still becomes terminal, even if the producer catches that exception.

## Sending Costs And Shutdown Boundaries

The HTTP writer uses bounded iovec cursors and small chunk metadata, not a
body-sized serialization buffer. TCP uses scatter/gather; TLS consumes borrowed
segments through its scalar encrypted-write path. TLS record/encryption buffers,
kernel copies, and coroutine/control allocations remain separate costs. This
contract does not claim allocation-free writes or one syscall/chunk/record per
logical write. No hidden queue retains body bytes between calls.

`stop()` cooperatively cancels HTTP sessions and producers; a producer ignoring
its token cannot be safely forcibly destroyed. Before destroying a server or its
TLS context, request stop, await all listener tasks, and then wait for active
connections to reach zero. A zero count observed before listener completion
does not exclude an accepted connection about to be registered.

WebSocket ordinary HTTP fallback uses the same sender but does not reuse the
connection. Once 101 has successfully handed ownership to WebSocket, its handler
retains its separate shutdown contract. HTTP producer cancellation is not a
promise to terminate upgraded handlers. TLS close-notify also retains its
existing bounded shutdown timeout rather than a new hard cancellation guarantee.

## Managed SSE

Return `sse::make_streaming_response(producer)` from a normal route. Its producer
receives a scoped sequential `sse::event_writer&` and cancellation token.
`send_event(event_view)`, `send_data(string_view)`, and `send_comment(string_view)`
borrow field slices through completion and use bounded descriptors for line
prefixes/delimiters. HTTP chunking is applied by the shared writer, not by the
SSE producer. Invalid id/type control characters fail before event output;
ignored sink failures still prevent successful response finalization.

The factory supplies `Content-Type: text/event-stream` and `Cache-Control:
no-cache`. It does not grant cross-origin access by default. Set an appropriate
CORS policy explicitly. The former `build_sse_response()` header-only helper is
removed. Legacy raw-stream `sse_connection` is not a managed HTTP body writer and
must not be used to bypass framing on a managed response.

## Data Path And Ownership

```text
transport → reusable reader buffer → HTTP body view → application / SSE parser
                                    metadata only → bounded decoder storage
```

`response_decoder::decode(input)` returns body views into `input`, never an
owned body copy. It can deliver the beginning of a chunk without receiving its
remaining payload or trailing CRLF. It owns parsed headers and fragmented
framing lines, not a whole-message or whole-chunk buffer.

`response_reader` owns one reusable transport buffer and a decoder. Its
`read(stream, token)` returns one event per await. The body view remains valid
until the next reader operation, move, or destruction. Consume it synchronously,
or await a borrowing downstream operation before calling `read()` again. Copy
explicitly if data must outlive that boundary or be processed independently.

The reader is move-only. Move it only without an active operation, and treat
the moved-from object as usable only for destruction or assignment. The stream
is supplied per read, not captured permanently, and must remain alive and
unmoved until that read completes. Do not switch streams midway through a
response. Serialize reads, configuration changes, resets, and moves.

The default receive buffer is 8192 bytes; a requested size of zero becomes one
byte. Parsed metadata is bounded separately by 100 field lines and 8192 bytes
per line by default. Status, chunk-size and trailer lines share the line-size
limit; trailers and duplicate headers consume the field-count budget. A
fragmented line may retain its trailing CR in addition to its allowed content.
Declared chunks retain the existing 1 GiB upper bound. There is no implicit
aggregate body limit in the decoder or reader: choose one at the application
layer when accumulating data. TLS and kernel transport buffering are outside
this HTTP-layer no-body-copy guarantee; SSE event parsing still owns its event
strings and applies its configured event-buffer limit.

## Events And Message Boundaries

| Event | Meaning and caller action |
|-------|---------------------------|
| `need_more` | Decoder accepted all current input and needs more bytes. The reader handles this internally. |
| `headers_complete` | Metadata and framing are available. Inspect status before consuming body. |
| `body` | A nonempty borrowed payload fragment; not an HTTP chunk or SSE event boundary. |
| `message_complete` | The framed message ended successfully; it may be an interim response. |
| `protocol_handoff` | A 101 or successful CONNECT header boundary; validate the upgrade/tunnel separately. |
| `error` | Stop normal message processing and inspect the decoder diagnostic or reader error code. |

Headers are reported before body even when both arrive in one transport read.
For direct decoder use, advance the current input by `result.consumed` and
decode again, including with empty input, until `need_more` or a terminal event.
Terminal events repeat with zero consumption. Unlike `response_parser`, this
consumed count never includes bytes from an earlier feed.

After a completed interim, `reader.next_response()` retains any unread bytes
and starts another message. It clears method context: set the original request
method again. It refuses pending, erroneous, and handed-off messages.
`reader.reset()` is different: it discards unread data and EOF state for a new
connection, retaining buffer capacity and configured limits. Neither closes or
reopens a connection. Both must run without an active read.

`remaining()` exposes unread *wire* data. Use it for a validated protocol
handoff, not as SSE/body data. Low-level code owns connection reuse decisions;
ordinary `http::client` refuses reuse for close-delimited responses or buffered
bytes after a final response.

HEAD has no delivered body even if Content-Length advertises representation
size. Informational/204/304 responses complete at their header boundary. Elio
also treats 205 as bodyless. Other responses use Content-Length, supported
chunked encoding, or connection close.
Only the single `chunked` transfer coding is implemented; stacks such as
`gzip, chunked` are rejected, not partially decoded and mislabeled as body.
This is separate from application handling of Content-Encoding.

## Completion, Errors And Cancellation

`response_read_result::success()` means `error == 0`, not that the whole response
is complete. Normal EOF completes a close-delimited response. EOF before a
declared length, chunk delimiter, or final trailer terminator is `EBADMSG`.
Configured metadata-limit failures are `EMSGSIZE`; transport errors remain
positive error codes. Payload may have been delivered before a later framing
failure. Do not infer a valid complete document from those earlier slices.

`read()` requires a readiness-aware stream and handles short reads and `EINTR`
internally. It does not busy-retry EAGAIN from a transport that violates that
precondition. `read_with(receive, token)` lets a caller supply a deadline-aware
read policy without duplicating HTTP parsing. The callback is invoked with
`(void* buffer, size_t capacity)` and must return an awaitable `io::io_result`,
write no more than capacity, and stop accessing the buffer before returning.
Capture/pass cancellation into that callback's transport operation explicitly;
the reader does not pass the token as a third callback argument.

Transport errors preserve parsing state, which allows the HTTP client's Expect
wait to cancel a pending read and continue when the transport permits it.
This is not permission to retry arbitrary failed transports. Framing errors
are terminal until reset. The reader does not impose a timeout, close a stream,
pool a connection, automatically reconnect, or replay application work.

Cancellation remains cooperative. Safe return relies on the underlying read
finishing its access to the borrowed buffer; do not destroy an in-flight reader
or coroutine frame to force timeout completion. A buffered event can be
returned without issuing a new transport read. Callers requiring cancellation
to suppress already-buffered delivery should check their token before pulling.

## Borrowing A Body Fragment Through A Downstream Await

This helper assumes the source connection already has a request awaiting its
response. It forwards decoded body to a **raw byte sink**, not to an HTTP
response writer. The caller owns connection cleanup and application policy.

```cpp
#include <elio/elio.hpp>
#include <elio/http/http_response_reader.hpp>

elio::coro::task<bool> forward_body(
    elio::net::stream& source, elio::net::stream& sink,
    elio::coro::cancel_token token) {
    using namespace elio::http;
    response_reader reader;
    reader.set_request_method(method::GET);
    size_t interims = 0;
    for (;;) {
        if (token.is_cancelled()) co_return false;
        const auto part = co_await reader.read(source, token);
        if (!part.success()) co_return false;
        if (part.event == response_event::body) {
            const auto sent = co_await sink.write_exactly(
                part.body.data(), part.body.size(), token);
            if (sent.result < 0 ||
                static_cast<size_t>(sent.result) != part.body.size()) {
                co_return false;
            }
        } else if (part.event == response_event::protocol_handoff) {
            co_return false;
        } else if (part.event == response_event::message_complete) {
            if (reader.decoder().status_code() >= 200) co_return true;
            if (++interims > 16 || !reader.next_response()) co_return false;
            reader.set_request_method(method::GET);
        }
    }
}
```

The next source read occurs only after the sink write completes, so the borrowed
body remains valid and the downstream await supplies backpressure. A later
source-framing or sink error cannot undo bytes already forwarded. This helper
does not promise application-level atomicity or infer that a 4xx/5xx status is
application success.

## Validation Map

Sending and receiving have separate evidence. A passed plain-loopback peer test
does not establish TLS, backend, sanitizer, or throughput coverage.

- `test_http_response_plan.cpp`: method/status/version framing, explicit length
  assertions, serializer parity, metadata preservation and move-only ownership.
- `test_http_body_writer.cpp`: controlled short writes, borrowed pointer lifetime,
  terminal arbitration, late completion, watchdog cleanup and allocation-failure
  checkpoints. Checkpoints are not a replacement for real allocator/sanitizer
  execution.
- `test_http_response_sender.cpp`: ordinary and owned producer execution, HEAD
  suppression, repeat dispatch, producer failure and length enforcement.
- `test_http_streaming_server.cpp` and `test_http_server.cpp`: route adapters,
  context lifetime/sealing, raw wire, connection reuse and cooperative stop.
- `test_http_streaming_transport.cpp`: real loopback TCP/TLS writes against a
  non-draining peer, cancellation/timeout after observed pending I/O, sticky
  failure, no finalizer write and buffer reuse after cleanup. Forced epoll and
  io_uring cases report unavailable backends explicitly.
- `test_sse_writer.cpp`: multiline/UTF-8 and metadata validation, fixed descriptor
  batches, borrowed source identity, sticky failure and factory ownership.
- `http_streaming_peer.py` with `http_streaming_peer_server.cpp`: pinned h11
  0.16.0 validates plain HTTP/1.1 sequential connection reuse, complete/known/
  chunked replies, HEAD producer suppression, 204/205/304 metadata, finite SSE
  and truncated failure.
  Enable `ELIO_BUILD_HTTP_INTEROP_TESTS` and install the exact peer version in
  the selected Python environment; CTest runs `http_streaming_interop`.
- `http_streaming_cost_probe.cpp`: complete/known/chunked sends of prepared
  64 KiB and 4 MiB bodies into a controlled, nonbuffering sink. Counts successful
  C++ allocation requests on the measuring thread and checks exact in-order
  borrowed source coverage and bounded iovecs. Excludes prepared source storage,
  malloc/custom allocator bypasses, other threads and transport/kernel buffers.
  Cumulative allocation bytes are not peak memory or throughput. Pointer
  coverage proves borrowed transport inputs, not absence of every possible
  intermediate copy; control/coroutine allocations remain observable.

The `HTTP streaming contracts` workflow also runs for relevant wiki edits and
compiles public-header and SSE examples. It publishes a per-scenario table and
retains peer logs. Its scope is conformance, not performance ranking; sanitizer
coverage belongs to the main Debug matrix, not this focused peer job. Runtime
results must still be checked for the current PR head before merging.

- `test_http_response_decoder.cpp`: pointer identity, partial large chunks,
  every split/bytewise framing, EOF, limits, HEAD, handoff and adapter semantics.
- `test_http_response_reader.cpp`: reusable-buffer pulls, error/EOF handling,
  interim boundaries, lifetime and state transitions.
- `test_sse.cpp`: genuine chunked and length-delimited wire fixtures, UTF-8 and
  event delimiters split across chunks, truncation and post-body bytes.
- `test_http_client.cpp`: ordinary and Expect response paths and connection
  behavior. Receiving final headers must suppress the pending upload without
  waiting for its response body.

See [[API Contracts]], [[API Reference]], [[WebSocket SSE]], and
[[Migrating to 0.6]] for related contracts and migration instructions.
