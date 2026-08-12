#include <elio/coro/task.hpp>
#include <elio/io/io_awaitables.hpp>
#include <elio/log/macros.hpp>
#include <elio/runtime/scheduler.hpp>
#include <elio/time/timer.hpp>

#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <sys/eventfd.h>
#include <unistd.h>

using namespace std::chrono_literals;

namespace {

using clock_type = std::chrono::steady_clock;

struct options {
    std::size_t trials = 40;
    std::chrono::milliseconds backlog_duration{20};
};

struct trial_state {
    std::atomic<bool> keep_backlog_running{true};
    std::atomic<bool> io_started{false};
    std::atomic<bool> io_completed{false};
    std::atomic<bool> backlog_started{false};
    std::atomic<bool> backlog_completed{false};
    std::atomic<bool> remote_completed{false};
    std::atomic<std::int64_t> io_completed_ns{0};
    std::atomic<std::int64_t> remote_completed_ns{0};
    std::atomic<std::uint64_t> backlog_yields{0};
    std::atomic<int> io_error{0};
};

struct trial_result {
    double io_latency_us = 0.0;
    double remote_latency_us = 0.0;
    std::uint64_t backlog_yields = 0;
    std::string backend;
};

struct summary {
    double p50 = 0.0;
    double p99 = 0.0;
    double maximum = 0.0;
};

std::int64_t now_ns() noexcept {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(
               clock_type::now().time_since_epoch())
        .count();
}

template<typename Predicate>
bool wait_until(Predicate&& predicate,
                std::chrono::milliseconds timeout = 2s) {
    const auto deadline = clock_type::now() + timeout;
    while (!predicate()) {
        if (clock_type::now() >= deadline) {
            return false;
        }
        std::this_thread::sleep_for(50us);
    }
    return true;
}

elio::coro::task<void> wait_for_io(int fd, trial_state* state) {
    state->io_started.store(true, std::memory_order_release);
    const auto result = co_await elio::io::async_poll_read(fd);
    state->io_error.store(result.error_code(), std::memory_order_relaxed);
    state->io_completed_ns.store(now_ns(), std::memory_order_relaxed);
    state->io_completed.store(true, std::memory_order_release);
}

elio::coro::task<void> keep_worker_runnable(trial_state* state) {
    std::uint64_t yields = 0;
    state->backlog_started.store(true, std::memory_order_release);
    while (state->keep_backlog_running.load(std::memory_order_acquire)) {
        ++yields;
        co_await elio::time::yield();
    }
    state->backlog_yields.store(yields, std::memory_order_relaxed);
    state->backlog_completed.store(true, std::memory_order_release);
}

elio::coro::task<void> record_remote_service(trial_state* state) {
    state->remote_completed_ns.store(now_ns(), std::memory_order_relaxed);
    state->remote_completed.store(true, std::memory_order_release);
    co_return;
}

summary summarize(std::vector<double> samples) {
    std::sort(samples.begin(), samples.end());
    const auto percentile = [&](double value) {
        const auto rank = static_cast<std::size_t>(
            std::ceil(value * static_cast<double>(samples.size()))) - 1;
        return samples[rank];
    };
    return summary{
        percentile(0.50),
        percentile(0.99),
        samples.back(),
    };
}

bool run_trial(const options& config,
               trial_result& result,
               std::string_view& failure) {
    const int event_fd = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (event_fd < 0) {
        failure = "eventfd creation failed";
        return false;
    }

    trial_state state;
    elio::runtime::scheduler scheduler(1);
    scheduler.start();

    const auto cleanup = [&] {
        state.keep_backlog_running.store(false, std::memory_order_release);
        const std::uint64_t value = 1;
        [[maybe_unused]] const auto written =
            ::write(event_fd, &value, sizeof(value));
        const bool stopped = scheduler.shutdown(2s);
        ::close(event_fd);
        return stopped;
    };

    scheduler.go_to(0, wait_for_io, event_fd, &state);
    auto* worker = scheduler.get_worker(0);
    if (worker) {
        result.backend = worker->io_context().get_backend_name();
    }
    if (!worker ||
        !wait_until([&] {
            return state.io_started.load(std::memory_order_acquire) &&
                   worker->io_context().has_pending() && worker->is_idle();
        })) {
        failure = "I/O probe did not become pending on the idle worker";
        cleanup();
        return false;
    }

    scheduler.go_to(0, keep_worker_runnable, &state);
    if (!wait_until([&] {
            return state.backlog_started.load(std::memory_order_acquire);
        })) {
        failure = "runnable backlog did not start";
        cleanup();
        return false;
    }

    const std::uint64_t value = 1;
    if (::write(event_fd, &value, sizeof(value)) !=
        static_cast<ssize_t>(sizeof(value))) {
        failure = "eventfd readiness write failed";
        cleanup();
        return false;
    }
    const auto io_trigger_ns = now_ns();

    const auto remote_trigger_ns = now_ns();
    scheduler.go_to(0, record_remote_service, &state);

    std::this_thread::sleep_for(config.backlog_duration);
    state.keep_backlog_running.store(false, std::memory_order_release);

    if (!wait_until([&] {
            return state.backlog_completed.load(std::memory_order_acquire) &&
                   state.remote_completed.load(std::memory_order_acquire) &&
                   state.io_completed.load(std::memory_order_acquire);
        })) {
        failure = "one or more probes did not complete after stopping the backlog";
        cleanup();
        return false;
    }

    if (state.io_error.load(std::memory_order_relaxed) != 0) {
        failure = "I/O readiness probe completed with an error";
        cleanup();
        return false;
    }

    result.io_latency_us = static_cast<double>(
                               state.io_completed_ns.load(
                                   std::memory_order_relaxed) -
                               io_trigger_ns) /
                           1000.0;
    result.remote_latency_us = static_cast<double>(
                                   state.remote_completed_ns.load(
                                       std::memory_order_relaxed) -
                                   remote_trigger_ns) /
                               1000.0;
    result.backlog_yields =
        state.backlog_yields.load(std::memory_order_relaxed);

    if (!cleanup()) {
        failure = "scheduler did not shut down cleanly";
        return false;
    }
    return true;
}

bool parse_positive(std::string_view text, std::size_t& value) {
    const auto* begin = text.data();
    const auto* end = begin + text.size();
    const auto [ptr, error] = std::from_chars(begin, end, value);
    return error == std::errc{} && ptr == end && value > 0;
}

void print_usage(std::string_view program) {
    std::cout
        << "Usage: " << program
        << " [--smoke] [--trials N] [--backlog-ms N]\n\n"
        << "Measures how quickly a single worker services an armed I/O "
           "completion and a remote inbox submission while a yield loop keeps "
           "its local runnable deque non-empty.\n\n"
        << "  --smoke         Run three 5 ms trials for CI termination coverage\n"
        << "  --trials N      Number of measured trials (default: 40)\n"
        << "  --backlog-ms N  Duration of each runnable backlog (default: 20)\n";
}

enum class parse_result {
    success,
    help,
    error,
};

parse_result parse_options(int argc, char** argv, options& config) {
    for (int i = 1; i < argc; ++i) {
        const std::string_view argument = argv[i];
        if (argument == "--help") {
            print_usage(argv[0]);
            return parse_result::help;
        }
        if (argument == "--smoke") {
            config.trials = 3;
            config.backlog_duration = 5ms;
            continue;
        }
        if ((argument == "--trials" || argument == "--backlog-ms") &&
            i + 1 < argc) {
            std::size_t value = 0;
            if (!parse_positive(argv[++i], value)) {
                std::cerr << "Invalid positive integer for " << argument
                          << '\n';
                return parse_result::error;
            }
            if (argument == "--trials") {
                config.trials = value;
            } else {
                config.backlog_duration = std::chrono::milliseconds(value);
            }
            continue;
        }

        std::cerr << "Unknown or incomplete option: " << argument << '\n';
        return parse_result::error;
    }
    return parse_result::success;
}

} // namespace

int main(int argc, char** argv) {
    options config;
    const auto parsed = parse_options(argc, argv, config);
    if (parsed == parse_result::help) {
        return 0;
    }
    if (parsed == parse_result::error) {
        print_usage(argv[0]);
        return 2;
    }

    elio::log::logger::instance().set_level(elio::log::level::error);

    std::vector<double> io_samples;
    std::vector<double> remote_samples;
    io_samples.reserve(config.trials);
    remote_samples.reserve(config.trials);
    std::uint64_t total_yields = 0;
    std::string backend;

    for (std::size_t trial = 0; trial < config.trials; ++trial) {
        trial_result result;
        std::string_view failure;
        if (!run_trial(config, result, failure)) {
            std::cerr << "Trial " << (trial + 1) << " failed: " << failure
                      << '\n';
            return 1;
        }
        io_samples.push_back(result.io_latency_us);
        remote_samples.push_back(result.remote_latency_us);
        total_yields += result.backlog_yields;
        if (backend.empty()) {
            backend = std::move(result.backend);
        } else if (backend != result.backend) {
            std::cerr << "I/O backend changed between trials (" << backend
                      << " -> " << result.backend
                      << "); refusing to combine unlike samples\n";
            return 1;
        }
    }

    const auto io = summarize(std::move(io_samples));
    const auto remote = summarize(std::move(remote_samples));
    std::cout << "Scheduler service latency under local runnable backlog\n"
              << "  trials: " << config.trials << '\n'
              << "  backend: " << backend << '\n'
              << "  backlog per trial: " << config.backlog_duration.count()
              << " ms\n"
              << "  total backlog yields: " << total_yields << '\n'
              << std::fixed << std::setprecision(2)
              << "  I/O completion latency (us): p50=" << io.p50
              << " p99=" << io.p99 << " max=" << io.maximum << '\n'
              << "  remote inbox latency (us): p50=" << remote.p50
              << " p99=" << remote.p99 << " max=" << remote.maximum << '\n';
    return 0;
}
