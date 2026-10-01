#pragma once

#include <elio/coro/task.hpp>
#include <elio/io/io_backend.hpp>
#include <elio/net/stream_close.hpp>

#include <chrono>
#include <concepts>
#include <cstddef>
#include <type_traits>
#include <utility>

namespace elio::net {

/// Explicit semantic opt-in, not a compile-time proof of correct I/O behavior.
struct publishing_byte_stream_contract final {};

/// An exclusively owned, ordered byte stream with one reader and one writer.
/// Positive write progress is published recursively through the owned lower
/// chain before return; at a TCP root this proves kernel acceptance, not peer
/// receipt. Accepted-only buffering requires a publishing adapter.
///
/// Cancellation must settle the operation's owned I/O before releasing borrowed
/// buffers. It never permits asynchronous public-frame destruction. Protocol
/// finish occupies the writer slot and may overlap one reader. TCP/TLS 1.3
/// finish directionally; TLS 1.2 finishes the current session. Buffered channels
/// preserve their lower protocol's scope; nested TLS finishes its own TLS layer.
/// Nonempty read EOF reports the peer's output scope via read_end_scope(). Raw
/// TLS truncation is an error, not authenticated EOF.
///
/// One abort_and_settle may overlap one reader and one writer. Starting abort
/// seals new I/O, cancels owned internal work, closes the whole owned chain, and
/// settles that work before completion, including exceptional completion.
/// Abort is not cancellable and does not promise a deadline or flush delivery.
/// Callers retain this object and borrowed buffers, and normally join all public
/// operations (including abort), before moving or destroying them. An abort
/// task allocation failure before it starts does not establish settlement.
///
/// Each wrapper exclusively owns lower I/O and retains submitted buffers until
/// actual completion. Owned buffering is finite and accounted per layer; this
/// does not bound all protocol-library allocations. Blocking/SSL/state mutexes
/// cannot span a lower await; a single-side operation lease or state machine may
/// retain ownership. Native handles are optional. See wiki/Byte-Streams.md for
/// the lifecycle matrix and the distinction from legacy stream close APIs.
template<typename Stream>
concept publishing_byte_stream =
    std::move_constructible<Stream> && !std::copy_constructible<Stream> &&
    !std::is_copy_assignable_v<Stream> &&
    requires(Stream& stream, const Stream& const_stream, void* input,
             const void* output, size_t size, coro::cancel_token token,
             std::chrono::milliseconds timeout) {
        typename Stream::byte_stream_contract;
        requires std::same_as<typename Stream::byte_stream_contract,
                              publishing_byte_stream_contract>;
        { stream.read(input, size, std::move(token)) }
            -> std::same_as<coro::task<io::io_result>>;
        { stream.write(output, size, std::move(token)) }
            -> std::same_as<coro::task<io::io_result>>;
        { stream.finish_write(std::move(token), timeout) }
            -> std::same_as<coro::task<write_finish_result>>;
        { const_stream.read_end_scope() } noexcept -> std::same_as<close_scope>;
        { stream.abort_and_settle() } -> std::same_as<coro::task<void>>;
    };

} // namespace elio::net
