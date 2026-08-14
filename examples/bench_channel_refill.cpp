#include <elio/coro/task.hpp>
#include <elio/log/macros.hpp>
#include <elio/runtime/scheduler.hpp>
#include <elio/sync/channel.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

using namespace elio;
using namespace std::chrono;

namespace {

coro::task<std::uint64_t> receive_ready(sync::channel<int>& channel,
                                        std::size_t count) {
    std::uint64_t checksum = 0;
    for (std::size_t i = 0; i < count; ++i) {
        auto value = co_await channel.recv();
        if (!value) std::abort();
        checksum += static_cast<unsigned>(*value);
    }
    co_return checksum;
}

coro::task<std::uint64_t> receive_ready_token(
        sync::channel<int>& channel, coro::cancel_token token,
        std::size_t count) {
    std::uint64_t checksum = 0;
    for (std::size_t i = 0; i < count; ++i) {
        auto result = co_await channel.recv(token);
        if (!result.success()) std::abort();
        checksum += static_cast<unsigned>(*result.value);
    }
    co_return checksum;
}

coro::task<void> send_many(sync::channel<int>& channel, std::size_t count) {
    for (std::size_t i = 0; i < count; ++i) {
        if (!co_await channel.send(static_cast<int>(i))) std::abort();
    }
    co_return;
}

coro::task<std::uint64_t> receive_many(sync::channel<int>& channel,
                                       std::size_t count) {
    std::uint64_t checksum = 0;
    for (std::size_t i = 0; i < count; ++i) {
        auto value = co_await channel.recv();
        if (!value) std::abort();
        checksum += static_cast<unsigned>(*value);
    }
    co_return checksum;
}

coro::task<void> receive_cancelled(sync::channel<int>& channel,
                                   coro::cancel_token token,
                                   std::size_t count) {
    for (std::size_t i = 0; i < count; ++i) {
        auto result = co_await channel.recv(token);
        if (!result.was_cancelled()) std::abort();
    }
    co_return;
}

coro::task<void> receive_closed(sync::channel<int>& channel,
                                std::size_t count) {
    for (std::size_t i = 0; i < count; ++i) {
        if (co_await channel.recv()) std::abort();
    }
    co_return;
}

template<typename Task>
double resume_ns_per_operation(Task& operation, std::size_t count) {
    auto handle = coro::detail::task_access::handle(operation);
    const auto start = steady_clock::now();
    {
        coro::detail::frame_context_scope frame_scope(
            std::addressof(handle.promise()));
        handle.resume();
    }
    const auto end = steady_clock::now();
    if (!handle.done()) std::abort();
    return duration<double, std::nano>(end - start).count() /
           static_cast<double>(count);
}

void fill(sync::channel<int>& channel, std::size_t count) {
    for (std::size_t i = 0; i < count; ++i) {
        if (!channel.try_send(static_cast<int>(i))) std::abort();
    }
}

double benchmark_ready_recv(std::size_t count) {
    sync::channel<int> channel(count);
    fill(channel, count);
    auto receiver = receive_ready(channel, count);
    return resume_ns_per_operation(receiver, count);
}

double benchmark_ready_try_recv(std::size_t count) {
    sync::channel<int> channel(count);
    fill(channel, count);
    std::uint64_t checksum = 0;
    const auto start = steady_clock::now();
    for (std::size_t i = 0; i < count; ++i) {
        auto value = channel.try_recv();
        if (!value) std::abort();
        checksum += static_cast<unsigned>(*value);
    }
    const auto end = steady_clock::now();
    if (checksum == std::numeric_limits<std::uint64_t>::max()) std::abort();
    return duration<double, std::nano>(end - start).count() /
           static_cast<double>(count);
}

double benchmark_ready_token_recv(std::size_t count) {
    sync::channel<int> channel(count);
    fill(channel, count);
    coro::cancel_source source;
    auto receiver = receive_ready_token(channel, source.get_token(), count);
    return resume_ns_per_operation(receiver, count);
}

double benchmark_forced_refill(std::size_t count) {
    sync::channel<int> channel(1);
    auto producer = send_many(channel, count);
    auto producer_handle = coro::detail::task_access::handle(producer);
    {
        coro::detail::frame_context_scope frame_scope(
            std::addressof(producer_handle.promise()));
        producer_handle.resume();
    }
    if (producer_handle.done()) std::abort();

    auto consumer = receive_many(channel, count);
    const double result = resume_ns_per_operation(consumer, count);
    if (!producer_handle.done()) std::abort();
    return result;
}

double benchmark_cancelled_recv(std::size_t count) {
    sync::channel<int> channel(64);
    coro::cancel_source source;
    source.cancel();
    auto receiver = receive_cancelled(channel, source.get_token(), count);
    return resume_ns_per_operation(receiver, count);
}

double benchmark_closed_recv(std::size_t count) {
    sync::channel<int> channel(64);
    channel.close();
    auto receiver = receive_closed(channel, count);
    return resume_ns_per_operation(receiver, count);
}

double benchmark_throughput(std::size_t pairs, std::size_t total_operations) {
    const std::size_t operations_per_pair = total_operations / pairs;
    const std::size_t actual_operations = operations_per_pair * pairs;
    sync::channel<int> channel(64);
    std::atomic<std::size_t> consumed{0};

    auto producer = [&channel](std::size_t count) -> coro::task<void> {
        for (std::size_t i = 0; i < count; ++i) {
            if (!co_await channel.send(static_cast<int>(i))) std::abort();
        }
        co_return;
    };
    auto consumer = [&channel, &consumed]() -> coro::task<void> {
        while (auto value = co_await channel.recv()) {
            (void)value;
            consumed.fetch_add(1, std::memory_order_relaxed);
        }
        co_return;
    };

    runtime::scheduler scheduler(pairs * 2);
    scheduler.start();
    std::vector<coro::join_handle<void>> producers;
    std::vector<coro::join_handle<void>> consumers;
    producers.reserve(pairs);
    consumers.reserve(pairs);

    const auto start = steady_clock::now();
    for (std::size_t i = 0; i < pairs; ++i) {
        producers.push_back(
            scheduler.go_joinable(producer, operations_per_pair));
    }
    for (std::size_t i = 0; i < pairs; ++i) {
        consumers.push_back(scheduler.go_joinable(consumer));
    }
    for (auto& producer_handle : producers) {
        producer_handle.wait_destroyed();
    }
    channel.close();
    for (auto& consumer_handle : consumers) {
        consumer_handle.wait_destroyed();
    }
    const auto end = steady_clock::now();
    scheduler.shutdown();

    if (consumed.load(std::memory_order_relaxed) != actual_operations) {
        std::abort();
    }
    return duration<double, std::nano>(end - start).count() /
           static_cast<double>(actual_operations);
}

void report(std::string_view label, double nanoseconds) {
    std::cout << label << " ns/op=" << nanoseconds << '\n';
}

}  // namespace

int main(int argc, char** argv) {
    bool smoke = false;
    if (argc == 2 && std::string_view(argv[1]) == "--smoke") {
        smoke = true;
    } else if (argc != 1) {
        std::cerr << "usage: bench_channel_refill [--smoke]\n";
        return 2;
    }

    log::logger::instance().set_level(log::level::error);
    const std::size_t ready_operations = smoke ? 1'000 : 200'000;
    const std::size_t handoff_operations = smoke ? 500 : 100'000;
    const std::size_t throughput_operations = smoke ? 2'000 : 100'000;

    report("ready bounded recv", benchmark_ready_recv(ready_operations));
    report("ready bounded try_recv",
           benchmark_ready_try_recv(ready_operations));
    report("ready bounded recv token",
           benchmark_ready_token_recv(ready_operations));
    report("forced full sender refill",
           benchmark_forced_refill(handoff_operations));
    report("cancelled recv control",
           benchmark_cancelled_recv(ready_operations));
    report("closed recv control", benchmark_closed_recv(ready_operations));
    for (const std::size_t pairs : {1, 2, 4}) {
        report(std::to_string(pairs) + "/" + std::to_string(pairs) +
                   " producer consumer",
               benchmark_throughput(pairs, throughput_operations));
    }
}
