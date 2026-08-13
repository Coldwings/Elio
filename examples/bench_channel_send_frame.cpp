#include "channel_send_frame_bench_factory.hpp"

#include <elio/coro/frame.hpp>
#include <elio/coro/task.hpp>
#include <elio/log/macros.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <memory>
#include <new>
#include <string_view>

#if defined(__GLIBC__)
#include <malloc.h>
#endif

using namespace elio;
using namespace std::chrono;

namespace {

struct allocation_record {
    void* address = nullptr;
    size_t requested = 0;
};

thread_local bool record_allocations = false;
thread_local std::array<allocation_record, 64> allocation_records{};
thread_local size_t stored_allocation_count = 0;
thread_local size_t total_allocation_count = 0;
thread_local size_t total_requested_bytes = 0;

void record_allocation(void* address, size_t requested) noexcept {
    if (!record_allocations) return;
    const size_t index = total_allocation_count++;
    total_requested_bytes += requested;
    if (index < allocation_records.size()) {
        allocation_records[index] = {address, requested};
        ++stored_allocation_count;
    }
}

class allocation_recording_scope {
public:
    allocation_recording_scope() noexcept {
        stored_allocation_count = 0;
        total_allocation_count = 0;
        total_requested_bytes = 0;
        record_allocations = true;
    }

    ~allocation_recording_scope() { stop(); }

    void stop() noexcept { record_allocations = false; }
};

allocation_record find_frame_allocation(void* frame_address) {
    const auto frame = reinterpret_cast<std::uintptr_t>(frame_address);
    allocation_record match;
    size_t matches = 0;
    for (size_t i = 0; i < stored_allocation_count; ++i) {
        const auto base = reinterpret_cast<std::uintptr_t>(
            allocation_records[i].address);
        const size_t size = allocation_records[i].requested;
        if (frame >= base && frame - base < size) {
            match = allocation_records[i];
            ++matches;
        }
    }
    if (matches != 1) {
        std::abort();
    }
    return match;
}

size_t allocator_usable_bytes(void* allocation) noexcept {
#if defined(__GLIBC__)
    return malloc_usable_size(allocation);
#else
    (void)allocation;
    return 0;
#endif
}

struct send_frame_measurement {
    size_t frame_bytes = 0;
    size_t usable_bytes = 0;
    size_t allocations = 0;
    size_t allocated_bytes = 0;
};

template<size_t InlineBytes>
send_frame_measurement measure_send_frame(bool with_token) {
    sync::channel<bench::inline_payload<InlineBytes>> channel(1);
    coro::cancel_source source;
    auto token = source.get_token();

    allocation_recording_scope recording;
    auto frame = with_token
        ? bench::make_cancellable_send_frame(channel, token)
        : bench::make_send_frame(channel);
    recording.stop();

    const auto frame_allocation = find_frame_allocation(frame.address());
    return {
        frame_allocation.requested,
        allocator_usable_bytes(frame_allocation.address),
        total_allocation_count,
        total_requested_bytes};
}

template<size_t InlineBytes>
double measure_send_task_construction_ns(bool with_token, size_t operations) {
    sync::channel<bench::inline_payload<InlineBytes>> channel(1);
    coro::cancel_source source;
    auto token = source.get_token();

    const auto start = steady_clock::now();
    for (size_t i = 0; i < operations; ++i) {
        if (with_token) {
            auto frame = bench::make_cancellable_send_frame(channel, token);
            std::atomic_signal_fence(std::memory_order_seq_cst);
            (void)frame;
        } else {
            auto frame = bench::make_send_frame(channel);
            std::atomic_signal_fence(std::memory_order_seq_cst);
            (void)frame;
        }
    }
    const auto end = steady_clock::now();
    return duration<double, std::nano>(end - start).count() / operations;
}

template<size_t InlineBytes>
coro::task<void> ready_bounded_send_loop(
        sync::channel<bench::inline_payload<InlineBytes>>& channel,
        size_t operations) {
    for (size_t i = 0; i < operations; ++i) {
        if (!co_await channel.send(bench::inline_payload<InlineBytes>{})) {
            std::abort();
        }
        if (!channel.try_recv().has_value()) {
            std::abort();
        }
    }
}

template<size_t InlineBytes>
coro::task<void> ready_unbounded_send_loop(
        sync::channel<bench::inline_payload<InlineBytes>>& channel,
        size_t operations) {
    for (size_t i = 0; i < operations; ++i) {
        if (!co_await channel.send(bench::inline_payload<InlineBytes>{})) {
            std::abort();
        }
        if (!channel.try_recv().has_value()) {
            std::abort();
        }
    }
}

template<size_t InlineBytes>
coro::task<void> ready_token_send_loop(
        sync::channel<bench::inline_payload<InlineBytes>>& channel,
        coro::cancel_token token, size_t operations) {
    for (size_t i = 0; i < operations; ++i) {
        const auto result = co_await channel.send(
            bench::inline_payload<InlineBytes>{}, token);
        if (!result.success() || !channel.try_recv().has_value()) {
            std::abort();
        }
    }
}

template<typename Factory>
double measure_ready_send_ns(Factory&& factory, size_t operations) {
    auto operation = factory();
    auto handle = coro::detail::task_access::handle(operation);
    const auto start = steady_clock::now();
    {
        coro::detail::frame_context_scope frame_scope(
            std::addressof(handle.promise()));
        handle.resume();
    }
    const auto end = steady_clock::now();
    if (!handle.done()) {
        std::abort();
    }
    return duration<double, std::nano>(end - start).count() / operations;
}

template<size_t InlineBytes, bool WithToken>
coro::task<void> forced_sender(
        sync::channel<bench::inline_payload<InlineBytes>>& channel,
        coro::cancel_token token, size_t operations) {
    for (size_t i = 0; i < operations; ++i) {
        if constexpr (WithToken) {
            const auto result = co_await channel.send(
                bench::inline_payload<InlineBytes>{}, token);
            if (!result.success()) {
                std::abort();
            }
        } else if (!co_await channel.send(
                       bench::inline_payload<InlineBytes>{})) {
            std::abort();
        }
    }
}

template<size_t InlineBytes>
coro::task<void> forced_receiver(
        sync::channel<bench::inline_payload<InlineBytes>>& channel,
        size_t operations) {
    for (size_t i = 0; i < operations; ++i) {
        const auto result = co_await channel.recv();
        if (!result.has_value()) {
            std::abort();
        }
    }
}

template<bool WithToken>
double measure_forced_handoff_ns(size_t capacity, size_t operations) {
    using payload = bench::inline_payload<256>;
    sync::channel<payload> channel(capacity);
    const bool bounded_full = capacity == 1;
    if (bounded_full && !channel.try_send(payload{})) {
        std::abort();
    }

    coro::cancel_source source;
    auto sender = forced_sender<256, WithToken>(
        channel, source.get_token(), operations);
    auto receiver = forced_receiver(
        channel, operations + static_cast<size_t>(bounded_full));
    auto sender_handle = coro::detail::task_access::handle(sender);
    auto receiver_handle = coro::detail::task_access::handle(receiver);

    {
        coro::detail::frame_context_scope frame_scope(
            std::addressof(sender_handle.promise()));
        sender_handle.resume();
    }
    if (sender_handle.done()) {
        std::abort();
    }

    const auto start = steady_clock::now();
    {
        coro::detail::frame_context_scope frame_scope(
            std::addressof(receiver_handle.promise()));
        receiver_handle.resume();
    }
    const auto end = steady_clock::now();

    if (!sender_handle.done() || !receiver_handle.done() || !channel.empty()) {
        std::abort();
    }
    return duration<double, std::nano>(end - start).count() / operations;
}

void print_rate(const char* name, double ns_per_send) {
    std::cout << std::setw(42) << std::left << name
              << std::setw(12) << std::right << std::fixed
              << std::setprecision(2) << ns_per_send << " ns/send  "
              << (1e9 / ns_per_send) << " sends/s\n";
}

}  // namespace

#if defined(__GNUC__) || defined(__clang__)
#define ELIO_BENCH_NOINLINE __attribute__((noinline))
#else
#define ELIO_BENCH_NOINLINE
#endif

ELIO_BENCH_NOINLINE void* operator new(std::size_t size) {
    void* address = std::malloc(size == 0 ? 1 : size);
    if (!address) throw std::bad_alloc();
    record_allocation(address, size);
    return address;
}

ELIO_BENCH_NOINLINE void* operator new[](std::size_t size) {
    return ::operator new(size);
}

ELIO_BENCH_NOINLINE void operator delete(void* address) noexcept {
    std::free(address);
}
ELIO_BENCH_NOINLINE void operator delete(
        void* address, std::size_t) noexcept {
    std::free(address);
}
ELIO_BENCH_NOINLINE void operator delete[](void* address) noexcept {
    std::free(address);
}
ELIO_BENCH_NOINLINE void operator delete[](
        void* address, std::size_t) noexcept {
    std::free(address);
}

#undef ELIO_BENCH_NOINLINE

int main(int argc, char** argv) {
    bool smoke = false;
    if (argc == 2 && std::string_view(argv[1]) == "--smoke") {
        smoke = true;
    } else if (argc != 1) {
        std::cerr << "usage: bench_channel_send_frame [--smoke]\n";
        return 2;
    }

    log::logger::instance().set_level(log::level::error);

    std::cout << "=== Channel Send Frame Benchmark ===\n";
    if (smoke) {
        std::cout << "Smoke mode: reduced iteration counts for Debug "
                     "validation.\n";
    }
    std::cout << "Naturally aligned inline payloads; usable bytes are zero "
                 "when the allocator does not expose them.\n";
    std::cout << "Payload  Token  Frame bytes  Usable bytes  Allocations  "
                 "Allocated bytes  Construct ns\n";

    auto print_frame = [](size_t payload_bytes, bool with_token,
                          send_frame_measurement measured,
                          double construction_ns) {
        std::cout << std::setw(7) << payload_bytes << "  "
                  << std::setw(5) << (with_token ? "yes" : "no") << "  "
                  << std::setw(11) << measured.frame_bytes << "  "
                  << std::setw(12) << measured.usable_bytes << "  "
                  << std::setw(11) << measured.allocations << "  "
                  << std::setw(15) << measured.allocated_bytes << "  "
                  << std::fixed << std::setprecision(2) << construction_ns
                  << '\n';
    };

    const size_t construction_operations = smoke ? 100 : 50000;
    auto measure_payload = [&]<size_t Bytes>() {
        print_frame(Bytes, false, measure_send_frame<Bytes>(false),
                    measure_send_task_construction_ns<Bytes>(
                        false, construction_operations));
        print_frame(Bytes, true, measure_send_frame<Bytes>(true),
                    measure_send_task_construction_ns<Bytes>(
                        true, construction_operations));
    };

    measure_payload.template operator()<8>();
    measure_payload.template operator()<64>();
    measure_payload.template operator()<256>();
    measure_payload.template operator()<1024>();

    const size_t ready_operations = smoke ? 100 : 100000;
    sync::channel<bench::inline_payload<256>> bounded(1);
    auto unbounded = sync::channel<bench::inline_payload<256>>::unbounded();
    coro::cancel_source source;
    auto token = source.get_token();

    print_rate("ready bounded send (256 bytes)", measure_ready_send_ns(
        [&] { return ready_bounded_send_loop(bounded, ready_operations); },
        ready_operations));
    print_rate("ready unbounded send (256 bytes)", measure_ready_send_ns(
        [&] { return ready_unbounded_send_loop(unbounded, ready_operations); },
        ready_operations));
    print_rate("ready active-token send (256 bytes)", measure_ready_send_ns(
        [&] { return ready_token_send_loop(
            bounded, token, ready_operations); }, ready_operations));

    const size_t handoff_operations = smoke ? 100 : 100000;
    print_rate("forced bounded-full handoff (256 bytes)",
        measure_forced_handoff_ns<false>(1, handoff_operations));
    print_rate("forced bounded-full token handoff (256 bytes)",
        measure_forced_handoff_ns<true>(1, handoff_operations));
    print_rate("forced rendezvous handoff (256 bytes)",
        measure_forced_handoff_ns<false>(0, handoff_operations));
    print_rate("forced rendezvous token handoff (256 bytes)",
        measure_forced_handoff_ns<true>(0, handoff_operations));

    return 0;
}
