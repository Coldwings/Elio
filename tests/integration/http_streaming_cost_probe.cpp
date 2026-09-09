// Diagnostic HTTP-layer allocation/borrowing probe, not a transport benchmark.
#include <elio/http/http_response_sender.hpp>

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <new>
#include <optional>
#include <span>
#include <string>
#include <string_view>

namespace allocation_probe {
struct counters {
    size_t calls = 0;
    size_t requested_bytes = 0;
    size_t largest_request = 0;
    size_t payload_sized_calls = 0;
    size_t payload_threshold = 0;
};
// The controlled coroutine chain completes inline on this thread. No scheduler,
// timers, background workers, or transport library allocations are measured.
thread_local counters* active = nullptr;

void record(size_t size) noexcept {
    if (!active) return;
    ++active->calls;
    active->requested_bytes += size;
    active->largest_request = std::max(active->largest_request, size);
    if (size >= active->payload_threshold) ++active->payload_sized_calls;
}

void* allocate(size_t size, size_t alignment = 0) {
    void* pointer = nullptr;
    if (alignment == 0) pointer = std::malloc(size ? size : 1);
    else if (::posix_memalign(&pointer, alignment, size ? size : 1) != 0) pointer = nullptr;
    if (!pointer) throw std::bad_alloc();
    record(size);
    return pointer;
}

struct scope {
    explicit scope(counters& value) noexcept { active = &value; }
    ~scope() { active = nullptr; }
    scope(const scope&) = delete;
    scope& operator=(const scope&) = delete;
};
} // namespace allocation_probe

// Cover scalar/array, aligned, and nothrow C++ allocation entry points. These
// replacements are confined to this standalone executable, never library code.
void* operator new(size_t size) { return allocation_probe::allocate(size); }
void* operator new[](size_t size) { return allocation_probe::allocate(size); }
void* operator new(size_t size, std::align_val_t alignment) {
    return allocation_probe::allocate(size, static_cast<size_t>(alignment));
}
void* operator new[](size_t size, std::align_val_t alignment) {
    return allocation_probe::allocate(size, static_cast<size_t>(alignment));
}
void* operator new(size_t size, const std::nothrow_t&) noexcept {
    try { return allocation_probe::allocate(size); } catch (...) { return nullptr; }
}
void* operator new[](size_t size, const std::nothrow_t&) noexcept {
    try { return allocation_probe::allocate(size); } catch (...) { return nullptr; }
}
void* operator new(size_t size, std::align_val_t alignment, const std::nothrow_t&) noexcept {
    try { return allocation_probe::allocate(size, static_cast<size_t>(alignment)); }
    catch (...) { return nullptr; }
}
void* operator new[](size_t size, std::align_val_t alignment, const std::nothrow_t&) noexcept {
    try { return allocation_probe::allocate(size, static_cast<size_t>(alignment)); }
    catch (...) { return nullptr; }
}
void operator delete(void* pointer) noexcept { std::free(pointer); }
void operator delete[](void* pointer) noexcept { std::free(pointer); }
void operator delete(void* pointer, size_t) noexcept { std::free(pointer); }
void operator delete[](void* pointer, size_t) noexcept { std::free(pointer); }
void operator delete(void* pointer, std::align_val_t) noexcept { std::free(pointer); }
void operator delete[](void* pointer, std::align_val_t) noexcept { std::free(pointer); }
void operator delete(void* pointer, size_t, std::align_val_t) noexcept { std::free(pointer); }
void operator delete[](void* pointer, size_t, std::align_val_t) noexcept { std::free(pointer); }
void operator delete(void* pointer, const std::nothrow_t&) noexcept { std::free(pointer); }
void operator delete[](void* pointer, const std::nothrow_t&) noexcept { std::free(pointer); }
void operator delete(void* pointer, std::align_val_t, const std::nothrow_t&) noexcept { std::free(pointer); }
void operator delete[](void* pointer, std::align_val_t, const std::nothrow_t&) noexcept { std::free(pointer); }

namespace {
using namespace elio;
using namespace elio::http;

struct discard_sink {
    std::string_view source;
    size_t calls = 0;
    size_t max_iovecs = 0;
    size_t borrowed_bytes = 0;
    size_t metadata_bytes = 0;
    bool ordered = true;

    coro::task<io::io_result> writev(iovec* parts, size_t count, coro::cancel_token token) {
        if (token.is_cancelled()) co_return io::io_result{-ECANCELED, 0};
        ++calls;
        max_iovecs = std::max(max_iovecs, count);
        size_t accepted = 0;
        const auto base = reinterpret_cast<uintptr_t>(source.data());
        for (size_t i = 0; i < count; ++i) {
            const auto address = reinterpret_cast<uintptr_t>(parts[i].iov_base);
            const auto size = parts[i].iov_len;
            if (address >= base && address - base < source.size()) {
                const auto offset = static_cast<size_t>(address - base);
                if (size > source.size() - offset || offset != borrowed_bytes) ordered = false;
                borrowed_bytes += size;
            } else {
                metadata_bytes += size;
            }
            accepted += size;
        }
        if (accepted > static_cast<size_t>(std::numeric_limits<int32_t>::max())) {
            co_return io::io_result{-EOVERFLOW, 0};
        }
        // No concatenation, payload copy, retained descriptors, queue, or I/O.
        co_return io::io_result{static_cast<int32_t>(accepted), 0};
    }
};

enum class mode { complete, known_stream, chunked };
const char* name(mode value) {
    switch (value) {
        case mode::complete: return "complete";
        case mode::known_stream: return "known_stream";
        case mode::chunked: return "chunked";
    }
    return "invalid";
}

struct row {
    mode kind{};
    size_t body_bytes = 0;
    allocation_probe::counters allocations;
    size_t transport_calls = 0;
    size_t max_iovecs = 0;
    size_t borrowed_bytes = 0;
    size_t metadata_bytes = 0;
    bool passed = false;
};

row measure(mode kind, size_t size) {
    // Source, complete-response storage, metadata, producer type erasure, and
    // descriptor preparation intentionally precede the allocation window.
    std::string source(size, 'x');
    std::array<body_buffer, 257> parts{};
    for (size_t i = 0; i < parts.size(); ++i) {
        const size_t begin = size * i / parts.size();
        const size_t end = size * (i + 1) / parts.size();
        parts[i] = {source.data() + begin, end - begin};
    }
    auto producer = [buffers = std::span<const body_buffer>(parts)](
        body_writer& writer, coro::cancel_token token) -> coro::task<send_result> {
        co_return co_await writer.writev(buffers, token);
    };
    reply selected = kind == mode::complete
        ? reply(response::ok(source))
        : reply(streaming_response(status::ok, producer,
            kind == mode::known_stream ? std::optional<uint64_t>(size) : std::nullopt));
    discard_sink sink{kind == mode::complete ? std::get<response>(selected).body()
                                             : std::string_view(source)};
    row result;
    result.kind = kind;
    result.body_bytes = size;
    result.allocations.payload_threshold = size;
    response_send_result sent;
    {
        allocation_probe::scope measured(result.allocations);
        auto operation = send_response(sink, selected, method::GET, "HTTP/1.1", true);
        const auto handle = coro::detail::task_access::handle(operation);
        handle.resume();
        // Any asynchronous suspension invalidates this single-thread probe.
        // Do not destroy a potentially active coroutine frame on that path.
        if (!handle.done()) {
            std::fputs("cost probe unexpectedly suspended\n", stderr);
            std::abort();
        }
        sent = operation.await_resume();
    }
    result.transport_calls = sink.calls;
    result.max_iovecs = sink.max_iovecs;
    result.borrowed_bytes = sink.borrowed_bytes;
    result.metadata_bytes = sink.metadata_bytes;
    result.passed = sent.success() && sent.reusable &&
        sent.result.confirmed_body_bytes == size && sink.ordered &&
        sink.borrowed_bytes == size && sink.max_iovecs <= 64 &&
        sink.metadata_bytes < 4096 && result.allocations.payload_sized_calls == 0;
    return result;
}
} // namespace

int main() {
    try {
        allocation_probe::counters calibration;
        calibration.payload_threshold = 128;
        {
            allocation_probe::scope measured(calibration);
            // Explicit function invocation checks the counter without a new
            // expression whose allocation the compiler may legally elide.
            void* (*volatile allocate)(size_t) = &::operator new;
            void* pointer = allocate(128);
            ::operator delete(pointer);
        }
        if (calibration.calls != 1 || calibration.requested_bytes != 128 ||
            calibration.largest_request != 128 || calibration.payload_sized_calls != 1) {
            std::fputs("cost probe allocation counter calibration failed\n", stderr);
            return 2;
        }
        std::array<row, 6> rows{};
        size_t index = 0;
        bool passed = true;
        for (const size_t size : {size_t{64} * 1024, size_t{4} * 1024 * 1024}) {
            for (const auto kind : {mode::complete, mode::known_stream, mode::chunked}) {
                rows[index] = measure(kind, size);
                passed = passed && rows[index].passed;
                ++index;
            }
        }
        std::printf("{\n  \"scope\": \"HTTP/1 send_response inline controlled discard sink; no network\",\n"
            "  \"performance_eligible\": false,\n"
            "  \"allocation_metric\": \"successful C++ new/new[] requests on measuring thread, including aligned/nothrow and sink coroutine frames\",\n"
            "  \"excluded\": \"prepared source/reply/producer/descriptors, reporting, malloc-family bypasses, custom allocators, other threads, TCP/TLS/kernel buffering\",\n"
            "  \"limitations\": \"requested bytes are cumulative, not peak/live memory; compiler allocation elision remains possible; pointer coverage proves borrowed transport inputs, not absence of every intermediate copy; no throughput or end-to-end zero-copy claim\",\n"
            "  \"stream_descriptors\": 257,\n  \"timeout_enabled\": false,\n"
            "  \"invariants\": \"successful reusable send; exact in-order borrowed source coverage; at most 64 iovecs per call; metadata under 4096 bytes; no observed allocation request as large as this row's payload\",\n"
            "  \"passed\": %s,\n  \"rows\": [\n", passed ? "true" : "false");
        for (size_t i = 0; i < rows.size(); ++i) {
            const auto& value = rows[i];
            std::printf("    {\"mode\":\"%s\",\"body_bytes\":%zu,\"allocation_calls\":%zu,"
                "\"requested_allocation_bytes\":%zu,\"largest_allocation_bytes\":%zu,"
                "\"subpayload_sized_allocations\":%zu,\"payload_sized_allocations\":%zu,"
                "\"transport_calls\":%zu,\"max_iovecs\":%zu,"
                "\"borrowed_source_bytes\":%zu,\"metadata_bytes\":%zu,\"passed\":%s}%s\n",
                name(value.kind), value.body_bytes, value.allocations.calls,
                value.allocations.requested_bytes, value.allocations.largest_request,
                value.allocations.calls - value.allocations.payload_sized_calls,
                value.allocations.payload_sized_calls, value.transport_calls, value.max_iovecs,
                value.borrowed_bytes, value.metadata_bytes, value.passed ? "true" : "false",
                i + 1 == rows.size() ? "" : ",");
        }
        std::puts("  ]\n}");
        return passed ? 0 : 1;
    } catch (const std::exception& error) {
        std::fprintf(stderr, "cost probe failed: %s\n", error.what());
        return 2;
    }
}
