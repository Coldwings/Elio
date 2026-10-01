# Composable Byte Streams

`<elio/net/byte_stream.hpp>` defines the TLS-free C++20
`net::publishing_byte_stream` concept and its explicit semantic opt-in tag,
`net::publishing_byte_stream_contract`. It is an ownership and lifecycle
boundary for future TLS, buffered-channel, and HTTP Transport adapters, not an
arbitrary `read`/`write` duck type.

This foundation supplies a contract and syntax checks, not those adapters.
Existing `tcp_stream`, `tls_stream`, and the closed TCP/TLS `net::stream`
variant are not automatically opted in. Their current close, cancellation,
concurrency, and output-finish guarantees remain unchanged. See
[[API Contracts]] and [[Migrating to 0.6]].

## Required Interface

A stream is move-constructible, neither copy-constructible nor copy-assignable,
and explicitly declares:

```cpp
using byte_stream_contract = elio::net::publishing_byte_stream_contract;
```

For `Stream& stream`, `const Stream& const_stream`, a cancellation token, and
a `std::chrono::milliseconds` budget, the concept checks these exact types:

| Expression | Required result |
|---|---|
| `stream.read(void_buffer, size, token)` | `coro::task<io::io_result>` |
| `stream.write(const_void_buffer, size, token)` | `coro::task<io::io_result>` |
| `stream.finish_write(token, budget)` | `coro::task<net::write_finish_result>` |
| `const_stream.read_end_scope()` | `net::close_scope`, `noexcept` |
| `stream.abort_and_settle()` | `coro::task<void>` |

The tag is a semantic promise by the implementation. Passing a C++ concept
cannot prove correct buffering, cancellation arbitration, or wire behavior.
No native descriptor, polling API, runtime type erasure, TLS include, or
move-assignment operation is required. An accepted-only buffered stream cannot
opt in truthfully without a publishing adapter.

## Progress And Publication

Reads and writes preserve reliable byte ordering, support short positive
progress, and wait for readiness rather than exposing transient would-block
as a normal public result. `io_result::result` carries the actual count or a
negative error. Positive progress never exceeds the requested count.

Accepted bytes are bytes the current layer has taken responsibility for.
Published bytes have advanced through every owned lower layer's publishing
write boundary. A positive write result promises both for its returned prefix:
it cannot leave that prefix waiting for an unrelated future application write
to start an output pump. At a TCP root, publication proves kernel acceptance,
not peer receipt, application acknowledgement, or durable storage. Cancellation
and failures do not undo bytes already published or make a suffix replay-safe.

A wrapper may use an internal flush/watermark capability to establish that
boundary. Extra explicit drain APIs are optional, with separately documented
scope; they cannot weaken positive write completion. Blocking or SSL/state
mutexes are released before awaiting a lower operation. A one-sided ownership
lease or explicit state machine may remain active across an await.

For a nonempty read, zero is EOF. Only after such a result does
`read_end_scope()` identify whether the peer finished its write direction or
the current protocol session. The enum's `write_direction` names the peer's
output here; it does not finish the local writer. Zero-length requests and
negative results are not evidence of EOF or an authenticated peer alert.

## Operation And Lifetime Matrix

The following requirements apply to opted-in streams on both epoll and
available io_uring paths. A wrapper exclusively owns its lower stream's I/O;
callers cannot bypass it through a native handle or an alias to that lower
object.

| Operation | Library responsibility | Caller responsibility |
|---|---|---|
| One reader plus one writer | Permit their overlap; arbitrate each completion/cancellation exactly once. | Serialize multiple readers and multiple writers; retain object and borrowed buffers through completion. |
| `finish_write(token, budget)` | Occupy the writer slot; finish the current protocol output with explicit scope and status. One reader may overlap. | Serialize against other writers, handshake, moves, and destruction. Check the result; success is not peer delivery. |
| One `abort_and_settle()` with one reader and one writer | On starting abort, seal admission of new I/O, request cancellation of owned internal work, close the whole owned chain, and settle internal operations before completion. | Keep the stream and all borrowed buffers alive; normally join every public operation, including abort, before moving or destroying them. Serialize multiple abort calls. |
| Handshake, move, destruction, and legacy close | Only the specific type's documented guarantees apply. Destruction is not asynchronous normal settlement. | Serialize against active public operations; do not treat cancellation or abort completion as permission to destroy still-live public coroutine frames. |

Abort is a non-cancellable cleanup boundary, not output finish, peer delivery,
or a deadline guarantee. Its completion settles owned internal I/O and state;
public read/write frames may still need their normal continuation and teardown.
Those frames must be joined separately. Once abort starts, errors cannot bypass
internal settlement, including when an exception propagates. If allocating
the lazy abort task fails before it starts, no settlement has occurred; the
caller still owns the previous lifetime obligations.

Cancellation is cooperative. A cancellation winner cannot release borrowed
storage while the backend still uses it, and late completion cannot resume an
operation a second time. Already-selected positive completion is not rewritten
as cancellation. Retain pending lower-operation state and buffers until actual
completion/cleanup, not merely until cancellation was requested. Forced runtime
shutdown remains non-graceful and does not authorize destroying active frames.

## Protocol And Closure Matrix

These rows describe the required behavior of future conforming adapters, using
the existing TCP/TLS finish behavior as the compatibility baseline:

| Current layer | Positive write proves | `finish_write` / nonempty read EOF | Whole-chain abort |
|---|---|---|---|
| TCP root | Kernel acceptance of the returned prefix. | Directional output finish / peer write-direction EOF. Healthy reverse traffic remains usable. | Close the root and settle owned I/O. |
| TLS 1.3 | Ciphertext for the returned plaintext prefix published through the owned lower chain. | Current TLS output finish / authenticated peer `close_notify`, directionally. Raw lower truncation is an error. | Close and settle this TLS layer and every owned lower layer. |
| TLS 1.2 | The same recursive ciphertext publication boundary. | Coordinated whole-current-session closure; reverse plaintext may be discarded while closing. Raw lower truncation is an error. | Close and settle the whole owned chain. |
| Transparent buffered channel | The returned prefix published through its lower layer, not merely stored locally. | Delegate protocol scope to the lower layer. Return owned read-ahead in order, exactly once, before reading lower input or exposing lower EOF. | Discard unpublished/read-ahead storage only after outstanding users have settled; abort the lower chain. |
| TLS over a buffered or TLS channel | Publish inner ciphertext through all lower publishing writes to the root. | Finish the inner TLS layer, not an implicit lower TLS finish or raw socket half-close. Report the inner negotiated scope. | Abort and settle every owned layer, including outer TLS. |

`write_finish_result` retains its existing `scope`, positive-errno `error`,
`local_end_flushed`, and `peer_end_observed` meanings. A local-end flush proves
publication of the current layer's end marker, not successful peer receipt.
Nested protocol finish must not bypass lower I/O ownership or close a lower
write direction just because an inner layer finished. The existing TCP/TLS
timeout and TLS 1.2 whole-session rules remain the baseline; adapters document
how a supplied budget applies to their own protocol closure.

## Buffering And Implementation Verification

Each adapter declares a finite bound for its owned read-ahead and pending output
payload. A composed chain accounts for the sum of those per-layer bounds,
including retained submitted storage through cancellation/abort cleanup. Do
not report only the top layer's queue as a whole-chain bound. Caller-owned
borrowed payload, protocol/control state, and OpenSSL's unrelated allocations
are outside a payload-only limit and must not be advertised as bounded by it.

Every adapter must add scripted short-progress, ordered read-ahead, EOF-scope,
publication, completion/cancellation-race, and overlapping-abort regressions.
Use barriers or test hooks where feasible rather than sleeps. Concept syntax
checks alone do not validate these runtime requirements. Generic TLS and HTTP
Transport implementations remain separate work following this foundation.
