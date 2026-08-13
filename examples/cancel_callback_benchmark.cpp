#include <elio/coro/cancel_token.hpp>

#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <new>
#include <numeric>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <vector>

#if defined(__GNUC__) || defined(__clang__)
#define ELIO_BENCH_NOINLINE __attribute__((noinline))
#else
#define ELIO_BENCH_NOINLINE
#endif

namespace allocation_probe {
thread_local bool enabled = false;
thread_local std::size_t allocations = 0;
thread_local std::size_t requested_bytes = 0;
}

ELIO_BENCH_NOINLINE void* allocate_for_probe(std::size_t size) {
    if (void* ptr = std::malloc(size == 0 ? 1 : size)) {
        if (allocation_probe::enabled) {
            ++allocation_probe::allocations;
            allocation_probe::requested_bytes += size;
        }
        return ptr;
    }
    throw std::bad_alloc();
}

ELIO_BENCH_NOINLINE void deallocate_for_probe(void* ptr) noexcept {
    std::free(ptr);
}

ELIO_BENCH_NOINLINE void* operator new(std::size_t size) {
    return allocate_for_probe(size);
}

ELIO_BENCH_NOINLINE void* operator new[](std::size_t size) {
    return allocate_for_probe(size);
}

ELIO_BENCH_NOINLINE void operator delete(void* ptr) noexcept {
    deallocate_for_probe(ptr);
}

ELIO_BENCH_NOINLINE void operator delete[](void* ptr) noexcept {
    deallocate_for_probe(ptr);
}

ELIO_BENCH_NOINLINE void operator delete(void* ptr, std::size_t) noexcept {
    deallocate_for_probe(ptr);
}

ELIO_BENCH_NOINLINE void operator delete[](void* ptr, std::size_t) noexcept {
    deallocate_for_probe(ptr);
}

#undef ELIO_BENCH_NOINLINE

namespace {

using clock_type = std::chrono::steady_clock;
using elio::coro::cancel_registration;
using elio::coro::cancel_source;

template<typename F>
double time_ns(F&& operation) {
    const auto start = clock_type::now();
    operation();
    const auto end = clock_type::now();
    return std::chrono::duration<double, std::nano>(end - start).count();
}

template<typename F>
std::vector<double> collect_samples(std::size_t count, F&& operation) {
    std::vector<double> samples;
    samples.reserve(count);
    for (std::size_t i = 0; i < count; ++i) {
        samples.push_back(operation());
    }
    return samples;
}

double percentile(const std::vector<double>& sorted, double fraction) {
    const auto index = static_cast<std::size_t>(
        fraction * static_cast<double>(sorted.size() - 1));
    return sorted[index];
}

void report(std::string_view label, std::vector<double> samples) {
    std::sort(samples.begin(), samples.end());
    const double mean = std::accumulate(samples.begin(), samples.end(), 0.0) /
                        static_cast<double>(samples.size());
    std::cout << std::left << std::setw(42) << label << std::right
              << " mean=" << std::setw(10) << mean
              << " p50=" << std::setw(10) << percentile(samples, 0.50)
              << " p95=" << std::setw(10) << percentile(samples, 0.95)
              << " p99=" << std::setw(10) << percentile(samples, 0.99)
              << " ns\n";
}

std::vector<cancel_registration> register_callbacks(cancel_source& source,
                                                     std::size_t count) {
    std::vector<cancel_registration> registrations;
    registrations.reserve(count);
    auto token = source.get_token();
    for (std::size_t i = 0; i < count; ++i) {
        registrations.push_back(token.on_cancel([] {}));
    }
    return registrations;
}

void benchmark_allocation(std::size_t iterations) {
    cancel_source source;
    auto token = source.get_token();
    std::size_t allocations = 0;
    std::size_t bytes = 0;

    for (std::size_t i = 0; i < iterations; ++i) {
        allocation_probe::allocations = 0;
        allocation_probe::requested_bytes = 0;
        allocation_probe::enabled = true;
        auto registration = token.on_cancel([] {});
        allocation_probe::enabled = false;
        allocations += allocation_probe::allocations;
        bytes += allocation_probe::requested_bytes;
        registration.unregister();
    }

    std::cout << "callback_node sizeof="
              << sizeof(elio::coro::detail::callback_node)
              << " task_parent_callback_node sizeof="
              << sizeof(elio::coro::detail::task_parent_callback_node) << '\n'
              << "SBO registration requested allocations="
              << (static_cast<double>(allocations) / iterations)
              << " requested bytes="
              << (static_cast<double>(bytes) / iterations) << "\n\n";
}

void benchmark_register_unregister(std::size_t iterations) {
    cancel_source source;
    auto token = source.get_token();
    report("register + unregister (one callback)",
           collect_samples(iterations, [&] {
               return time_ns([&] {
                   auto registration = token.on_cancel([] {});
                   registration.unregister();
               });
           }));
}

void benchmark_unlink(std::size_t iterations, std::size_t list_size,
                      bool oldest) {
    report(std::string(oldest ? "unlink oldest / " : "unlink newest / ") +
               std::to_string(list_size),
           collect_samples(iterations, [&] {
               cancel_source source;
               auto registrations = register_callbacks(source, list_size);
               auto& target = oldest ? registrations.front()
                                     : registrations.back();
               return time_ns([&] { target.unregister(); });
           }));
}

void benchmark_dispatch(std::size_t iterations, std::size_t list_size) {
    report("cancel dispatch / " + std::to_string(list_size),
           collect_samples(iterations, [&] {
               cancel_source source;
               auto registrations = register_callbacks(source, list_size);
               return time_ns([&] { source.cancel(); });
           }));
}

void benchmark_immediate(std::size_t iterations) {
    std::atomic<std::size_t> invocations{0};
    report("already-cancelled registration",
           collect_samples(iterations, [&] {
               cancel_source source;
               source.cancel();
               auto token = source.get_token();
               return time_ns([&] {
                   auto registration = token.on_cancel([&] {
                       invocations.fetch_add(1, std::memory_order_relaxed);
                   });
               });
           }));
    if (invocations.load(std::memory_order_relaxed) != iterations) {
        std::abort();
    }
}

void benchmark_concurrent_unregister(std::size_t iterations,
                                     std::size_t list_size) {
    std::atomic<std::size_t> invocations{0};
    std::size_t unregister_wins = 0;
    std::size_t dispatch_wins = 0;
    report("cancel with concurrent unregister / " +
               std::to_string(list_size),
           collect_samples(iterations, [&] {
               cancel_source source;
               std::vector<cancel_registration> registrations;
               registrations.reserve(list_size);
               auto token = source.get_token();
               for (std::size_t i = 0; i < list_size; ++i) {
                   registrations.push_back(token.on_cancel([&] {
                       invocations.fetch_add(1, std::memory_order_relaxed);
                   }));
               }

               const auto invocations_before =
                   invocations.load(std::memory_order_relaxed);
               std::atomic<bool> ready{false};
               std::atomic<bool> start{false};
               std::thread unregisterer([&] {
                   ready.store(true, std::memory_order_release);
                   ready.notify_one();
                   start.wait(false, std::memory_order_acquire);
                   for (std::size_t i = 0; i < list_size / 2; ++i) {
                       registrations[i].unregister();
                   }
               });

               ready.wait(false, std::memory_order_acquire);
               const auto elapsed = time_ns([&] {
                   start.store(true, std::memory_order_release);
                   start.notify_one();
                   source.cancel();
               });
               unregisterer.join();
               const auto iteration_invocations =
                   invocations.load(std::memory_order_relaxed) -
                   invocations_before;
               unregister_wins += list_size - iteration_invocations;
               dispatch_wins += iteration_invocations;
               return elapsed;
           }));
    std::cout << "  outcomes removed=" << unregister_wins
              << " invoked=" << dispatch_wins << "\n";
}

}  // namespace

int main(int argc, char** argv) {
    std::size_t iterations = 2000;
    if (argc > 2) {
        std::cerr << "Usage: " << argv[0]
                  << " [--smoke|positive-sample-count]\n";
        return 2;
    }
    if (argc == 2) {
        const std::string_view argument(argv[1]);
        if (argument == "--smoke") {
            iterations = 20;
        } else {
            const auto* begin = argument.data();
            const auto* end = begin + argument.size();
            const auto result =
                std::from_chars(begin, end, iterations);
            if (result.ec != std::errc{} || result.ptr != end ||
                iterations == 0) {
                std::cerr << "Usage: " << argv[0]
                          << " [--smoke|positive-sample-count]\n";
                return 2;
            }
        }
    }

    std::cout << std::fixed << std::setprecision(2)
              << "samples=" << iterations << '\n';
    benchmark_allocation(iterations);
    benchmark_register_unregister(iterations);
    for (const std::size_t size : {8, 32, 256}) {
        benchmark_unlink(iterations, size, false);
        benchmark_unlink(iterations, size, true);
    }
    for (const std::size_t size : {1, 8, 32, 256}) {
        benchmark_dispatch(iterations, size);
    }
    benchmark_immediate(iterations);
    benchmark_concurrent_unregister(
        std::max<std::size_t>(iterations / 4, 100), 32);
}
