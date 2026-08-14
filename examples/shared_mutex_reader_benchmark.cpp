#include <elio/coro/task.hpp>
#include <elio/log/macros.hpp>
#include <elio/runtime/scheduler.hpp>
#include <elio/runtime/worker_thread.hpp>
#include <elio/sync/shared_mutex.hpp>
#include <elio/time/timer.hpp>

#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <coroutine>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string_view>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

using namespace elio;
using namespace std::chrono;

namespace {

struct tail_summary {
    std::int64_t p95 = 0;
    std::int64_t p99 = 0;
    std::int64_t max = 0;
};

struct concurrent_summary {
    double ns_per_operation = 0.0;
    double operations_per_second = 0.0;
    std::size_t operations_completed = 0;
};

struct forced_summary {
    double ns_per_operation = 0.0;
    std::size_t operations_completed = 0;
    std::size_t attempted = 0;
    std::size_t resumed = 0;
};

struct mixed_summary : concurrent_summary {
    tail_summary writer_wait;
    std::size_t readers = 0;
    std::size_t writers = 0;
};

struct pressure_summary : concurrent_summary {
    tail_summary writer_wait;
    std::size_t writer_progress = 0;
    std::size_t zero_progress_samples = 0;
};

std::size_t parse_positive(std::string_view text) {
    std::size_t result = 0;
    const auto parsed = std::from_chars(
        text.data(), text.data() + text.size(), result);
    if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() ||
        result == 0) {
        return 0;
    }
    return result;
}

tail_summary summarize_tail(std::vector<std::int64_t> samples) {
    if (samples.empty()) std::abort();
    std::sort(samples.begin(), samples.end());
    const auto percentile = [&](std::size_t percent) {
        return samples[(samples.size() - 1) * percent / 100];
    };
    return {percentile(95), percentile(99), samples.back()};
}

void wait_for_count(std::atomic<std::size_t>& value, std::size_t expected) {
    auto observed = value.load(std::memory_order_acquire);
    while (observed != expected) {
        value.wait(observed, std::memory_order_acquire);
        observed = value.load(std::memory_order_acquire);
    }
}

coro::task<void> persistent_reader(
        sync::shared_mutex& mutex, std::atomic<std::size_t>& ready,
        std::atomic<std::size_t>& warmed, std::atomic<bool>& warmup,
        std::atomic<bool>& start, std::size_t iterations,
        std::atomic<std::size_t>& completed) {
    ready.fetch_add(1, std::memory_order_release);
    ready.notify_one();
    while (!warmup.load(std::memory_order_acquire)) {
        co_await time::yield();
    }

    co_await mutex.lock_shared();
    mutex.unlock_shared();
    warmed.fetch_add(1, std::memory_order_release);
    warmed.notify_one();
    while (!start.load(std::memory_order_acquire)) {
        co_await time::yield();
    }

    for (std::size_t i = 0; i < iterations; ++i) {
        co_await mutex.lock_shared();
        mutex.unlock_shared();
    }
    completed.fetch_add(iterations, std::memory_order_release);
    completed.notify_one();
    co_return;
}

concurrent_summary concurrent_readers(std::size_t workers,
                                      std::size_t iterations) {
    sync::shared_mutex mutex;
    runtime::scheduler scheduler(workers);
    scheduler.start();
    std::atomic<std::size_t> ready{0};
    std::atomic<std::size_t> warmed{0};
    std::atomic<std::size_t> completed{0};
    std::atomic<bool> warmup{false};
    std::atomic<bool> start{false};
    std::vector<coro::join_handle<void>> readers;
    readers.reserve(workers);
    for (std::size_t i = 0; i < workers; ++i) {
        readers.push_back(scheduler.go_joinable_to(i, persistent_reader(
            mutex, ready, warmed, warmup, start, iterations, completed)));
    }

    wait_for_count(ready, workers);
    warmup.store(true, std::memory_order_release);
    wait_for_count(warmed, workers);
    const auto begin = steady_clock::now();
    start.store(true, std::memory_order_release);
    const auto total = workers * iterations;
    wait_for_count(completed, total);
    const auto elapsed = steady_clock::now() - begin;
    for (auto& reader : readers) reader.wait_destroyed();
    for (auto& reader : readers) reader.await_resume();
    if (!scheduler.shutdown(seconds(5))) std::abort();

    if (completed.load(std::memory_order_acquire) != total ||
        mutex.reader_count() != 0 || mutex.is_writer_active()) {
        std::abort();
    }
    const double elapsed_ns = duration<double, std::nano>(elapsed).count();
    const double elapsed_seconds = duration<double>(elapsed).count();
    return {elapsed_ns / static_cast<double>(total),
            static_cast<double>(total) / elapsed_seconds,
            completed.load(std::memory_order_relaxed)};
}

concurrent_summary ready_reader(std::size_t iterations) {
    return concurrent_readers(1, iterations);
}

class handoff_yield {
public:
    [[nodiscard]] bool await_ready() const noexcept { return false; }

    void await_suspend(std::coroutine_handle<> handle) const noexcept {
        auto* worker = runtime::worker_thread::current();
        if (worker == nullptr || !worker->schedule(handle)) std::abort();
    }

    void await_resume() const noexcept {}
};

coro::task<void> persistent_forced_reader(
        sync::shared_mutex& mutex, std::size_t operations,
        std::atomic<std::size_t>& generation,
        std::atomic<std::size_t>& attempted,
        std::atomic<std::size_t>& completed) {
    for (std::size_t operation = 1; operation <= operations; ++operation) {
        while (generation.load(std::memory_order_acquire) != operation) {
            co_await handoff_yield{};
        }
        attempted.store(operation, std::memory_order_release);
        attempted.notify_one();
        co_await mutex.lock_shared();
        mutex.unlock_shared();
        completed.store(operation, std::memory_order_release);
        completed.notify_one();
    }
    co_return;
}

coro::task<void> persistent_forced_driver(
        sync::shared_mutex& mutex, std::size_t iterations,
        std::atomic<std::size_t>& generation,
        std::atomic<std::size_t>& attempted,
        std::atomic<std::size_t>& completed,
        std::atomic<std::int64_t>& elapsed_ns) {
    steady_clock::time_point begin;
    const std::size_t operations = iterations + 1;
    for (std::size_t operation = 1; operation <= operations; ++operation) {
        if (!mutex.try_lock()) std::abort();
        generation.store(operation, std::memory_order_release);

        // Both tasks share one scheduler worker. Once this driver observes the
        // attempt after yielding, the reader's await_suspend has returned and
        // its waiter publication is complete; unlocking cannot race resumption
        // with await_suspend itself.
        while (attempted.load(std::memory_order_acquire) != operation) {
            co_await handoff_yield{};
        }
        mutex.unlock();
        while (completed.load(std::memory_order_acquire) != operation) {
            co_await handoff_yield{};
        }
        if (operation == 1) {
            begin = steady_clock::now();
        }
    }
    elapsed_ns.store(duration_cast<nanoseconds>(
        steady_clock::now() - begin).count(), std::memory_order_release);
    co_return;
}

forced_summary forced_handoff(std::size_t iterations) {
    sync::shared_mutex mutex;
    runtime::scheduler scheduler(1);
    scheduler.start();
    std::atomic<std::size_t> generation{0};
    std::atomic<std::size_t> attempted{0};
    std::atomic<std::size_t> completed{0};
    std::atomic<std::int64_t> elapsed_ns{0};
    const std::size_t operations = iterations + 1;

    auto reader = scheduler.go_joinable_to(0, persistent_forced_reader(
        mutex, operations, generation, attempted, completed));
    auto driver = scheduler.go_joinable_to(0, persistent_forced_driver(
        mutex, iterations, generation, attempted, completed, elapsed_ns));
    reader.wait_destroyed();
    driver.wait_destroyed();
    reader.await_resume();
    driver.await_resume();
    if (!scheduler.shutdown(seconds(5)) ||
        attempted.load(std::memory_order_acquire) != operations ||
        completed.load(std::memory_order_acquire) != operations ||
        elapsed_ns.load(std::memory_order_acquire) <= 0 ||
        mutex.reader_count() != 0 || mutex.is_writer_active()) {
        std::abort();
    }
    const auto attempted_count = attempted.load(std::memory_order_relaxed);
    const auto resumed_count = completed.load(std::memory_order_relaxed);
    return {
        static_cast<double>(elapsed_ns.load(std::memory_order_relaxed)) /
            static_cast<double>(iterations),
        resumed_count - 1,
        attempted_count,
        resumed_count,
    };
}

coro::task<void> mixed_worker(
        sync::shared_mutex& mutex, std::atomic<std::size_t>& ready,
        std::atomic<std::size_t>& warmed, std::atomic<bool>& warmup,
        std::atomic<bool>& start,
        std::size_t iterations, std::size_t reader_percent,
        std::uint64_t seed, std::vector<std::int64_t>& writer_waits,
        std::atomic<std::size_t>& readers,
        std::atomic<std::size_t>& writers,
        std::atomic<std::size_t>& finished) {
    ready.fetch_add(1, std::memory_order_release);
    ready.notify_one();
    while (!warmup.load(std::memory_order_acquire)) {
        co_await time::yield();
    }
    co_await mutex.lock_shared();
    mutex.unlock_shared();
    warmed.fetch_add(1, std::memory_order_release);
    warmed.notify_one();
    while (!start.load(std::memory_order_acquire)) {
        co_await time::yield();
    }

    std::size_t local_readers = 0;
    std::size_t local_writers = 0;
    for (std::size_t i = 0; i < iterations; ++i) {
        seed = seed * 6364136223846793005ULL + 1442695040888963407ULL;
        if ((seed >> 32) % 100 < reader_percent) {
            co_await mutex.lock_shared();
            ++local_readers;
            mutex.unlock_shared();
        } else {
            const auto wait_start = steady_clock::now();
            co_await mutex.lock();
            writer_waits.push_back(duration_cast<nanoseconds>(
                steady_clock::now() - wait_start).count());
            ++local_writers;
            mutex.unlock();
        }
        co_await time::yield();
    }

    readers.fetch_add(local_readers, std::memory_order_relaxed);
    writers.fetch_add(local_writers, std::memory_order_relaxed);
    finished.fetch_add(1, std::memory_order_release);
    finished.notify_one();
    co_return;
}

mixed_summary mixed_workload(std::size_t workers, std::size_t iterations,
                             std::size_t reader_percent) {
    sync::shared_mutex mutex;
    runtime::scheduler scheduler(workers);
    scheduler.start();
    std::atomic<std::size_t> ready{0};
    std::atomic<std::size_t> warmed{0};
    std::atomic<bool> warmup{false};
    std::atomic<bool> start{false};
    std::atomic<std::size_t> readers{0};
    std::atomic<std::size_t> writers{0};
    std::atomic<std::size_t> finished{0};
    std::vector<std::vector<std::int64_t>> waits(workers);
    std::vector<coro::join_handle<void>> handles;
    handles.reserve(workers);
    for (std::size_t i = 0; i < workers; ++i) {
        waits[i].reserve(iterations * (100 - reader_percent) / 100 + 1);
        handles.push_back(scheduler.go_joinable_to(i, mixed_worker(
            mutex, ready, warmed, warmup, start, iterations,
            reader_percent, i + 1,
            waits[i], readers, writers, finished)));
    }

    auto observed = ready.load(std::memory_order_acquire);
    while (observed != workers) {
        ready.wait(observed, std::memory_order_acquire);
        observed = ready.load(std::memory_order_acquire);
    }
    warmup.store(true, std::memory_order_release);
    wait_for_count(warmed, workers);
    const auto begin = steady_clock::now();
    start.store(true, std::memory_order_release);
    wait_for_count(finished, workers);
    const auto elapsed = steady_clock::now() - begin;
    for (auto& handle : handles) handle.wait_destroyed();
    for (auto& handle : handles) handle.await_resume();
    if (!scheduler.shutdown(seconds(5))) std::abort();

    std::vector<std::int64_t> merged;
    for (auto& local : waits) {
        merged.insert(merged.end(), local.begin(), local.end());
    }
    const auto reader_count = readers.load(std::memory_order_relaxed);
    const auto writer_count = writers.load(std::memory_order_relaxed);
    const auto total = workers * iterations;
    if (reader_count + writer_count != total || merged.size() != writer_count ||
        writer_count == 0 || mutex.reader_count() != 0 ||
        mutex.is_writer_active()) {
        std::abort();
    }

    const double elapsed_ns = duration<double, std::nano>(elapsed).count();
    const double elapsed_seconds = duration<double>(elapsed).count();
    mixed_summary result;
    result.ns_per_operation = elapsed_ns / static_cast<double>(total);
    result.operations_per_second =
        static_cast<double>(total) / elapsed_seconds;
    result.operations_completed = total;
    result.writer_wait = summarize_tail(std::move(merged));
    result.readers = reader_count;
    result.writers = writer_count;
    return result;
}

coro::task<void> queued_writer(
        sync::shared_mutex& mutex, std::vector<std::int64_t>& waits,
        std::atomic<std::size_t>& progress) {
    const auto start = steady_clock::now();
    co_await mutex.lock();
    waits.push_back(duration_cast<nanoseconds>(
        steady_clock::now() - start).count());
    progress.fetch_add(1, std::memory_order_relaxed);
    mutex.unlock();
    co_return;
}

coro::task<void> pressure_reader(
        sync::shared_mutex& mutex, std::atomic<std::size_t>& ready,
        std::atomic<std::size_t>& warmed, std::atomic<bool>& warmup,
        std::atomic<bool>& start, std::atomic<bool>& stop,
        std::atomic<std::uint64_t>& progress,
        std::atomic<std::size_t>& finished) {
    ready.fetch_add(1, std::memory_order_release);
    ready.notify_one();
    while (!warmup.load(std::memory_order_acquire)) {
        co_await time::yield();
    }
    co_await mutex.lock_shared();
    mutex.unlock_shared();
    warmed.fetch_add(1, std::memory_order_release);
    warmed.notify_one();
    while (!start.load(std::memory_order_acquire)) {
        co_await time::yield();
    }

    std::uint64_t local_progress = 0;
    while (!stop.load(std::memory_order_acquire)) {
        co_await mutex.lock_shared();
        ++local_progress;
        mutex.unlock_shared();
        co_await time::yield();
    }
    progress.fetch_add(local_progress, std::memory_order_relaxed);
    finished.fetch_add(1, std::memory_order_release);
    finished.notify_one();
    co_return;
}

pressure_summary queued_writer_pressure(std::size_t reader_threads,
                                        std::size_t writer_samples) {
    sync::shared_mutex mutex;
    runtime::scheduler scheduler(reader_threads + 1);
    scheduler.start();
    std::atomic<std::size_t> ready{0};
    std::atomic<std::size_t> warmed{0};
    std::atomic<bool> warmup{false};
    std::atomic<bool> start{false};
    std::atomic<bool> stop{false};
    std::atomic<std::uint64_t> reader_progress{0};
    std::atomic<std::size_t> writer_progress{0};
    std::atomic<std::size_t> finished{0};
    std::vector<coro::join_handle<void>> readers;
    readers.reserve(reader_threads);
    for (std::size_t i = 0; i < reader_threads; ++i) {
        readers.push_back(scheduler.go_joinable_to(i, pressure_reader(
            mutex, ready, warmed, warmup, start, stop, reader_progress,
            finished)));
    }

    std::vector<std::int64_t> waits;
    waits.reserve(writer_samples);
    wait_for_count(ready, reader_threads);
    warmup.store(true, std::memory_order_release);
    wait_for_count(warmed, reader_threads);
    const auto begin = steady_clock::now();
    start.store(true, std::memory_order_release);
    std::size_t zero_progress_samples = 0;
    for (std::size_t sample = 0; sample < writer_samples; ++sample) {
        while (!mutex.try_lock_shared()) std::this_thread::yield();
        const auto before = writer_progress.load(std::memory_order_relaxed);
        auto writer = scheduler.go_joinable_to(
            reader_threads,
            queued_writer(mutex, waits, writer_progress));
        const auto deadline = steady_clock::now() + seconds(1);
        while (mutex.try_lock_shared()) {
            mutex.unlock_shared();
            if (steady_clock::now() >= deadline) std::abort();
            std::this_thread::yield();
        }
        mutex.unlock_shared();
        writer.wait_destroyed();
        writer.await_resume();
        if (writer_progress.load(std::memory_order_acquire) == before) {
            ++zero_progress_samples;
        }
    }

    stop.store(true, std::memory_order_release);
    wait_for_count(finished, reader_threads);
    const auto elapsed = steady_clock::now() - begin;
    for (auto& reader : readers) reader.wait_destroyed();
    for (auto& reader : readers) reader.await_resume();
    if (!scheduler.shutdown(seconds(5))) std::abort();
    const auto reader_count = reader_progress.load(std::memory_order_relaxed);
    const auto writer_count = writer_progress.load(std::memory_order_relaxed);
    if (waits.size() != writer_samples || writer_count != writer_samples ||
        reader_count == 0 || mutex.reader_count() != 0 ||
        mutex.is_writer_active()) {
        std::abort();
    }

    const double elapsed_ns = duration<double, std::nano>(elapsed).count();
    const double elapsed_seconds = duration<double>(elapsed).count();
    pressure_summary result;
    result.ns_per_operation =
        elapsed_ns / static_cast<double>(reader_count);
    result.operations_per_second =
        static_cast<double>(reader_count) / elapsed_seconds;
    result.operations_completed = reader_count;
    result.writer_wait = summarize_tail(std::move(waits));
    result.writer_progress = writer_count;
    result.zero_progress_samples = zero_progress_samples;
    return result;
}

void report_tail(const tail_summary& tail) {
    std::cout << " writer_p95_ns=" << tail.p95
              << " writer_p99_ns=" << tail.p99
              << " writer_max_ns=" << tail.max;
}

void usage() {
    std::cerr
        << "usage: shared_mutex_reader_benchmark --suite core --iterations N\n"
        << "       shared_mutex_reader_benchmark --suite readers --workers "
           "{1|2|4|8} --iterations N\n"
        << "       shared_mutex_reader_benchmark --suite mixed --workers "
           "{2|4|8} --reader-percent {90|50} --iterations N\n"
        << "       shared_mutex_reader_benchmark --suite pressure --readers "
           "{1|2|4|8} --iterations N\n"
        << "       shared_mutex_reader_benchmark --smoke\n";
}

bool allowed_count(std::size_t value, bool allow_one) {
    return (allow_one && value == 1) || value == 2 || value == 4 || value == 8;
}

}  // namespace

int main(int argc, char** argv) {
    log::logger::instance().set_level(log::level::error);
    if (argc == 2 && std::string_view(argv[1]) == "--smoke") {
        std::cout << "suite=smoke\n";
        const auto ready = ready_reader(100);
        std::cout << "ready_reader ns/op=" << ready.ns_per_operation
                  << " operations_completed=" << ready.operations_completed
                  << '\n';
        const auto forced = forced_handoff(100);
        std::cout << "forced_writer_reader_handoff ns/op="
                  << forced.ns_per_operation
                  << " operations_completed=" << forced.operations_completed
                  << " forced_attempted=" << forced.attempted
                  << " forced_resumed=" << forced.resumed << '\n';
        const auto readers = concurrent_readers(2, 100);
        std::cout << "reader_concurrency ns/op=" << readers.ns_per_operation
                  << " ops/s=" << readers.operations_per_second
                  << " operations_completed="
                  << readers.operations_completed << '\n';
        const auto mixed = mixed_workload(2, 100, 90);
        std::cout << "mixed_90 ns/op=" << mixed.ns_per_operation
                  << " ops/s=" << mixed.operations_per_second;
        report_tail(mixed.writer_wait);
        std::cout << " readers=" << mixed.readers
                  << " writers=" << mixed.writers << '\n';
        const auto pressure = queued_writer_pressure(1, 10);
        std::cout << "queued_writer_pressure ns/reader="
                  << pressure.ns_per_operation
                  << " reader_ops/s=" << pressure.operations_per_second;
        report_tail(pressure.writer_wait);
        std::cout << " writer_progress=" << pressure.writer_progress
                  << " zero_progress=" << pressure.zero_progress_samples
                  << '\n';
        return 0;
    }

    if (argc == 5 && std::string_view(argv[1]) == "--suite" &&
        std::string_view(argv[2]) == "core" &&
        std::string_view(argv[3]) == "--iterations") {
        const auto iterations = parse_positive(argv[4]);
        if (iterations == 0) {
            usage();
            return 2;
        }
        std::cout << "suite=core iterations=" << iterations << '\n';
        const auto ready = ready_reader(iterations);
        std::cout << "ready_reader ns/op=" << ready.ns_per_operation
                  << " operations_completed=" << ready.operations_completed
                  << '\n';
        const auto forced = forced_handoff(iterations);
        std::cout << "forced_writer_reader_handoff ns/op="
                  << forced.ns_per_operation
                  << " operations_completed=" << forced.operations_completed
                  << " forced_attempted=" << forced.attempted
                  << " forced_resumed=" << forced.resumed << '\n';
        return 0;
    }

    if (argc == 7 && std::string_view(argv[1]) == "--suite" &&
        std::string_view(argv[3]) == "--workers" &&
        std::string_view(argv[5]) == "--iterations") {
        const auto suite = std::string_view(argv[2]);
        const auto workers = parse_positive(argv[4]);
        const auto iterations = parse_positive(argv[6]);
        if (suite != "readers" || !allowed_count(workers, true) ||
            iterations == 0) {
            usage();
            return 2;
        }
        const auto result = concurrent_readers(workers, iterations);
        std::cout << "suite=readers workers=" << workers
                  << " iterations=" << iterations << '\n';
        std::cout << "reader_concurrency ns/op=" << result.ns_per_operation
                  << " ops/s=" << result.operations_per_second
                  << " operations_completed="
                  << result.operations_completed << '\n';
        return 0;
    }

    if (argc == 9 && std::string_view(argv[1]) == "--suite" &&
        std::string_view(argv[2]) == "mixed" &&
        std::string_view(argv[3]) == "--workers" &&
        std::string_view(argv[5]) == "--reader-percent" &&
        std::string_view(argv[7]) == "--iterations") {
        const auto workers = parse_positive(argv[4]);
        const auto reader_percent = parse_positive(argv[6]);
        const auto iterations = parse_positive(argv[8]);
        if (!allowed_count(workers, false) ||
            (reader_percent != 90 && reader_percent != 50) ||
            iterations == 0) {
            usage();
            return 2;
        }
        const auto result = mixed_workload(
            workers, iterations, reader_percent);
        std::cout << "suite=mixed workers=" << workers
                  << " reader_percent=" << reader_percent
                  << " iterations=" << iterations << '\n';
        std::cout << "mixed_" << reader_percent
                  << " ns/op=" << result.ns_per_operation
                  << " ops/s=" << result.operations_per_second;
        report_tail(result.writer_wait);
        std::cout << " readers=" << result.readers
                  << " writers=" << result.writers << '\n';
        return 0;
    }

    if (argc == 7 && std::string_view(argv[1]) == "--suite" &&
        std::string_view(argv[2]) == "pressure" &&
        std::string_view(argv[3]) == "--readers" &&
        std::string_view(argv[5]) == "--iterations") {
        const auto readers = parse_positive(argv[4]);
        const auto iterations = parse_positive(argv[6]);
        if (!allowed_count(readers, true) || iterations == 0) {
            usage();
            return 2;
        }
        const auto result = queued_writer_pressure(readers, iterations);
        std::cout << "suite=pressure readers=" << readers
                  << " iterations=" << iterations << '\n';
        std::cout << "queued_writer_pressure ns/reader="
                  << result.ns_per_operation
                  << " reader_ops/s=" << result.operations_per_second;
        report_tail(result.writer_wait);
        std::cout << " writer_progress=" << result.writer_progress
                  << " zero_progress=" << result.zero_progress_samples
                  << '\n';
        return 0;
    }

    usage();
    return 2;
}
