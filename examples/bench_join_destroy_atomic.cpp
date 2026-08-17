#include <elio/coro/task.hpp>
#include <elio/log/logger.hpp>
#include <elio/runtime/scheduler.hpp>
#include <elio/sync/event.hpp>

#include <atomic>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <iomanip>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <vector>

namespace {

using clock_type = std::chrono::steady_clock;

elio::coro::task<void> empty_task() {
    co_return;
}

elio::coro::task<void> wait_on_gate(elio::sync::event& gate,
                                    std::atomic<std::size_t>& entered) {
    entered.fetch_add(1, std::memory_order_release);
    entered.notify_all();
    co_await gate.wait();
}

double elapsed_ns(clock_type::time_point start,
                  clock_type::time_point end,
                  std::size_t operations) {
    return static_cast<double>(
               std::chrono::duration_cast<std::chrono::nanoseconds>(
                   end - start).count()) /
           static_cast<double>(operations);
}

void report(std::string_view row, std::size_t operations, double ns_per_op) {
    std::cout << "row=" << row << " operations=" << operations
              << " ns_per_op=" << std::fixed << std::setprecision(3)
              << ns_per_op << '\n';
}

void run_state_suite(std::size_t operations) {
    using join_state_base = elio::coro::detail::join_state_base;
    using void_state = elio::coro::detail::join_state<void>;
    using value_state = elio::coro::detail::join_state<std::uint64_t>;

    std::cout << "row=layout base_bytes=" << sizeof(join_state_base)
              << " void_bytes=" << sizeof(void_state)
              << " value_bytes=" << sizeof(value_state) << '\n';

    std::vector<std::unique_ptr<void_state>> states;
    states.reserve(operations);
    for (std::size_t i = 0; i < operations; ++i) {
        states.push_back(std::make_unique<void_state>());
    }
    auto start = clock_type::now();
    for (auto& state : states) {
        state->mark_destroyed();
    }
    auto end = clock_type::now();
    report("no_waiter_publish", operations,
           elapsed_ns(start, end, operations));

    void_state destroyed;
    destroyed.mark_destroyed();
    start = clock_type::now();
    for (std::size_t i = 0; i < operations; ++i) {
        destroyed.wait_destroyed();
    }
    end = clock_type::now();
    report("already_destroyed_wait", operations,
           elapsed_ns(start, end, operations));
}

void run_blocking_suite(std::size_t operations) {
    using void_state = elio::coro::detail::join_state<void>;

    for (const std::size_t waiter_count : {1U, 2U, 4U, 8U}) {
        std::chrono::nanoseconds total{0};
        for (std::size_t iteration = 0; iteration < operations; ++iteration) {
            void_state state;
            std::atomic<std::size_t> entered{0};
            std::vector<std::thread> waiters;
            waiters.reserve(waiter_count);
            for (std::size_t i = 0; i < waiter_count; ++i) {
                waiters.emplace_back([&] {
                    entered.fetch_add(1, std::memory_order_release);
                    entered.notify_all();
                    state.wait_destroyed();
                });
            }
            auto observed = entered.load(std::memory_order_acquire);
            while (observed != waiter_count) {
                entered.wait(observed, std::memory_order_acquire);
                observed = entered.load(std::memory_order_acquire);
            }
            std::this_thread::sleep_for(std::chrono::microseconds(50));

            const auto start = clock_type::now();
            state.mark_destroyed();
            for (auto& waiter : waiters) {
                waiter.join();
            }
            total += std::chrono::duration_cast<std::chrono::nanoseconds>(
                clock_type::now() - start);
        }
        report("blocking_waiters_" + std::to_string(waiter_count),
               operations,
               static_cast<double>(total.count()) /
                   static_cast<double>(operations));
    }
}

void run_lifecycle_suite(std::size_t operations) {
    elio::runtime::scheduler scheduler(1);
    scheduler.start();

    constexpr std::size_t warmup_iterations = 128;
    for (std::size_t i = 0; i < warmup_iterations; ++i) {
        auto joined = scheduler.go_joinable(empty_task());
        joined.wait_destroyed();
        joined.await_resume();
    }

    auto start = clock_type::now();
    for (std::size_t i = 0; i < operations; ++i) {
        auto joined = scheduler.go_joinable(empty_task());
        joined.wait_destroyed();
        joined.await_resume();
    }
    auto end = clock_type::now();
    report("direct_spawn_wait_destroyed", operations,
           elapsed_ns(start, end, operations));

    std::vector<elio::coro::join_handle<void>> batch;
    batch.reserve(operations);
    start = clock_type::now();
    for (std::size_t i = 0; i < operations; ++i) {
        batch.push_back(scheduler.go_joinable(empty_task()));
    }
    for (auto& joined : batch) {
        joined.wait_destroyed();
        joined.await_resume();
    }
    end = clock_type::now();
    report("batch_spawn_drain", operations,
           elapsed_ns(start, end, operations));

    elio::sync::event gate;
    std::atomic<std::size_t> entered{0};
    std::vector<elio::coro::join_handle<void>> pending;
    pending.reserve(operations);
    for (std::size_t i = 0; i < operations; ++i) {
        pending.push_back(scheduler.go_joinable(
            wait_on_gate(gate, entered)));
    }
    auto observed = entered.load(std::memory_order_acquire);
    while (observed != operations) {
        entered.wait(observed, std::memory_order_acquire);
        observed = entered.load(std::memory_order_acquire);
    }
    start = clock_type::now();
    gate.set();
    for (auto& joined : pending) {
        joined.wait_destroyed();
        joined.await_resume();
    }
    end = clock_type::now();
    report("pending_release_drain", operations,
           elapsed_ns(start, end, operations));

    if (!scheduler.shutdown(std::chrono::seconds(5))) {
        std::abort();
    }
}

bool parse_operations(std::string_view text, std::size_t& value) {
    const auto* begin = text.data();
    const auto* end = begin + text.size();
    const auto result = std::from_chars(begin, end, value);
    return result.ec == std::errc{} && result.ptr == end && value != 0;
}

void usage(const char* program) {
    std::cerr << "usage: " << program
              << " --smoke | --suite state|blocking|lifecycle"
                 " --operations N\n";
}

} // namespace

int main(int argc, char** argv) {
    elio::log::logger::instance().set_level(elio::log::level::error);

    if (argc == 2 && std::string_view(argv[1]) == "--smoke") {
        std::cout << "benchmark=join_destroy_atomic mode=smoke\n";
        run_state_suite(1000);
        run_blocking_suite(2);
        run_lifecycle_suite(64);
        return 0;
    }

    if (argc != 5 || std::string_view(argv[1]) != "--suite" ||
        std::string_view(argv[3]) != "--operations") {
        usage(argv[0]);
        return 2;
    }

    const std::string_view suite(argv[2]);
    std::size_t operations = 0;
    if (!parse_operations(argv[4], operations) ||
        (suite != "state" && suite != "blocking" &&
         suite != "lifecycle")) {
        usage(argv[0]);
        return 2;
    }

    std::cout << "benchmark=join_destroy_atomic mode=formal suite=" << suite
              << " operations=" << operations << '\n';
    if (suite == "state") {
        run_state_suite(operations);
    } else if (suite == "blocking") {
        run_blocking_suite(operations);
    } else {
        run_lifecycle_suite(operations);
    }
    return 0;
}
