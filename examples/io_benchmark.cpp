#include <elio/runtime/scheduler.hpp>
#include <elio/coro/task.hpp>
#include <elio/io/io_awaitables.hpp>
#include <elio/log/macros.hpp>
#include <iostream>
#include <chrono>
#include <atomic>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>

using namespace elio;
using namespace std::chrono;

// Benchmark file I/O throughput
void benchmark_file_io() {
    constexpr int N = 10000;

    // Create a temp file
    char tmpfile[] = "/tmp/elio_bench_XXXXXX";
    int fd = mkstemp(tmpfile);
    if (fd < 0) {
        std::cerr << "Failed to create temp file" << std::endl;
        return;
    }

    char buf[4096];
    std::memset(buf, 'A', sizeof(buf));

    // Warm up - write some data
    for (int i = 0; i < 100; ++i) {
        [[maybe_unused]] auto ret = ::write(fd, buf, sizeof(buf));
    }
    lseek(fd, 0, SEEK_SET);

    runtime::scheduler sched(1);
    sched.start();

    std::atomic<int> completed{0};

    auto io_task = [&]() -> coro::task<void> {
        for (int i = 0; i < N; ++i) {
            auto result = co_await io::async_read(fd, buf, sizeof(buf), (i % 100) * sizeof(buf));
            (void)result;
        }
        completed.store(1, std::memory_order_release);
        co_return;
    };

    auto start = high_resolution_clock::now();
    io_task().go();

    while (completed.load(std::memory_order_acquire) == 0) {
        std::this_thread::sleep_for(microseconds(100));
    }
    auto end = high_resolution_clock::now();

    sched.shutdown();
    close(fd);
    unlink(tmpfile);

    auto ns = duration_cast<nanoseconds>(end - start).count();
    std::cout << "File I/O Benchmark:" << std::endl;
    std::cout << "  " << N << " reads: " << (ns / N) << " ns/read" << std::endl;
    std::cout << "  Throughput: " << (N * 1000000000.0 / ns) << " IOPS" << std::endl;
}

// Benchmark concurrent file I/O
void benchmark_concurrent_file_io() {
    constexpr int N = 5000;
    constexpr int NUM_TASKS = 4;

    // Create a temp file
    char tmpfile[] = "/tmp/elio_bench_XXXXXX";
    int fd = mkstemp(tmpfile);
    if (fd < 0) {
        std::cerr << "Failed to create temp file" << std::endl;
        return;
    }

    char buf[4096];
    std::memset(buf, 'A', sizeof(buf));

    // Warm up - write some data
    for (int i = 0; i < 100; ++i) {
        [[maybe_unused]] auto ret = ::write(fd, buf, sizeof(buf));
    }
    lseek(fd, 0, SEEK_SET);

    runtime::scheduler sched(NUM_TASKS);
    sched.start();

    std::atomic<int> completed{0};

    auto io_task = [&]() -> coro::task<void> {
        char local_buf[4096];
        for (int i = 0; i < N; ++i) {
            auto result = co_await io::async_read(fd, local_buf, sizeof(local_buf), (i % 100) * sizeof(local_buf));
            (void)result;
        }
        completed.fetch_add(1, std::memory_order_release);
        co_return;
    };

    auto start = high_resolution_clock::now();
    for (int i = 0; i < NUM_TASKS; ++i) {
        io_task().go();
    }

    while (completed.load(std::memory_order_acquire) < NUM_TASKS) {
        std::this_thread::sleep_for(microseconds(100));
    }
    auto end = high_resolution_clock::now();

    sched.shutdown();
    close(fd);
    unlink(tmpfile);

    auto ns = duration_cast<nanoseconds>(end - start).count();
    int total_reads = N * NUM_TASKS;
    std::cout << "Concurrent File I/O Benchmark (" << NUM_TASKS << " tasks):" << std::endl;
    std::cout << "  " << total_reads << " reads: " << (ns / total_reads) << " ns/read" << std::endl;
    std::cout << "  Throughput: " << (total_reads * 1000000000.0 / ns) << " IOPS" << std::endl;
}

int main() {
    log::logger::instance().set_level(log::level::error);

    std::cout << "=== Elio I/O Benchmark ===" << std::endl;
    std::cout << std::endl;

    benchmark_file_io();
    std::cout << std::endl;

    benchmark_concurrent_file_io();

    std::cout << std::endl;
    std::cout << "=== Done ===" << std::endl;
    return 0;
}
