# HTTP Streaming

## Implemented Scope

Elio's HTTP/1 receive path provides a shared incremental response decoder and
a pull reader. Ordinary HTTP clients explicitly accumulate its body slices;
SSE clients incrementally parse those slices as events. This is the receive
phase of [#1191](https://github.com/Coldwings/Elio/issues/1191), tracked in
[#1192](https://github.com/Coldwings/Elio/issues/1192).

The proposed server-managed producer/writer and unified outgoing framing plan
are **not implemented by this phase**. Existing response serialization and SSE
server APIs retain their current contracts; this page does not promise an
outgoing zero-copy writer, new cancellation semantics, or HTTP/2 streaming.

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
