#include <elio/runtime/scheduler.hpp>
#include <elio/coro/task.hpp>
#include <elio/log/macros.hpp>
#include <iostream>
#include <chrono>
#include <vector>
#include <sys/eventfd.h>
#include <unistd.h>

using namespace elio;
using namespace std::chrono;

coro::task<void> empty_task() {
    co_return;
}

int main() {
    log::logger::instance().set_level(log::level::error);

    constexpr int N = 100000;

    // 1. Measure coroutine frame allocation (cold - first time)
    {
        std::vector<std::coroutine_handle<>> handles;
        handles.reserve(N);

        auto start = high_resolution_clock::now();
        for (int i = 0; i < N; ++i) {
            auto t = empty_task();
            handles.push_back(t.release());
        }
        auto end = high_resolution_clock::now();
        auto ns = duration_cast<nanoseconds>(end - start).count();

        std::cout << "Coroutine frame alloc (cold): " << (ns / N) << " ns/task" << std::endl;

        // Cleanup - this will return frames to pool
        for (auto h : handles) h.destroy();
    }

    // 2. Measure coroutine frame allocation (warm - pool has frames)
    {
        std::vector<std::coroutine_handle<>> handles;
        handles.reserve(N);

        auto start = high_resolution_clock::now();
        for (int i = 0; i < N; ++i) {
            auto t = empty_task();
            handles.push_back(t.release());
        }
        auto end = high_resolution_clock::now();
        auto ns = duration_cast<nanoseconds>(end - start).count();

        std::cout << "Coroutine frame alloc (warm): " << (ns / N) << " ns/task" << std::endl;

        // Cleanup
        for (auto h : handles) h.destroy();
    }

    // 3. Measure MPSC push only (no scheduler overhead)
    {
        runtime::mpsc_queue<void> queue;

        auto start = high_resolution_clock::now();
        for (int i = 0; i < N; ++i) {
            queue.push(reinterpret_cast<void*>(i + 1));
        }
        auto end = high_resolution_clock::now();
        auto ns = duration_cast<nanoseconds>(end - start).count();

        std::cout << "MPSC push: " << (ns / N) << " ns/push" << std::endl;

        // Drain
        while (queue.pop()) {}
    }

    // 4. Measure Chase-Lev push only
    {
        runtime::chase_lev_deque<void> queue;

        auto start = high_resolution_clock::now();
        for (int i = 0; i < N; ++i) {
            queue.push(reinterpret_cast<void*>(i + 1));
        }
        auto end = high_resolution_clock::now();
        auto ns = duration_cast<nanoseconds>(end - start).count();

        std::cout << "Chase-Lev push: " << (ns / N) << " ns/push" << std::endl;

        // Drain
        while (queue.pop()) {}
    }

    // 5. Measure atomic fence alone
    {
        auto start = high_resolution_clock::now();
        for (int i = 0; i < N; ++i) {
            std::atomic_thread_fence(std::memory_order_release);
        }
        auto end = high_resolution_clock::now();
        auto ns = duration_cast<nanoseconds>(end - start).count();

        std::cout << "Atomic release fence: " << (ns / N) << " ns" << std::endl;
    }

    // 6. Measure eventfd write
    {
        int fd = eventfd(0, EFD_NONBLOCK);
        uint64_t val = 1;

        auto start = high_resolution_clock::now();
        for (int i = 0; i < N; ++i) {
            [[maybe_unused]] auto ret = ::write(fd, &val, sizeof(val));
        }
        auto end = high_resolution_clock::now();
        auto ns = duration_cast<nanoseconds>(end - start).count();

        std::cout << "eventfd write: " << (ns / N) << " ns" << std::endl;
        close(fd);
    }

    // 7. Full spawn path (with running scheduler) - cold
    {
        runtime::scheduler sched(4);
        sched.start();

        std::vector<std::coroutine_handle<>> handles;
        handles.reserve(N);

        // Pre-create tasks
        for (int i = 0; i < N; ++i) {
            auto t = empty_task();
            handles.push_back(t.release());
        }

        auto start = high_resolution_clock::now();
        for (auto h : handles) {
            sched.spawn(h);
        }
        auto end = high_resolution_clock::now();

        // Wait for completion
        while (sched.pending_tasks() > 0) {
            std::this_thread::sleep_for(microseconds(10));
        }

        auto ns = duration_cast<nanoseconds>(end - start).count();
        std::cout << "spawn() only (pre-alloc): " << (ns / N) << " ns/spawn" << std::endl;

        sched.shutdown();
    }

    // 8. Measure idle worker overhead
    {
        runtime::scheduler sched(4);
        sched.start();

        // Let workers warm up
        std::this_thread::sleep_for(std::chrono::milliseconds(50));

        std::vector<std::coroutine_handle<>> handles;
        handles.reserve(N);

        // Pre-create tasks
        for (int i = 0; i < N; ++i) {
            auto t = empty_task();
            handles.push_back(t.release());
        }

        auto start = high_resolution_clock::now();
        for (auto h : handles) {
            sched.spawn(h);
        }
        auto end = high_resolution_clock::now();

        // Wait for completion
        while (sched.pending_tasks() > 0) {
            std::this_thread::sleep_for(microseconds(10));
        }

        auto ns = duration_cast<nanoseconds>(end - start).count();
        std::cout << "spawn() only (workers idle): " << (ns / N) << " ns/spawn" << std::endl;

        sched.shutdown();
    }

    return 0;
}
