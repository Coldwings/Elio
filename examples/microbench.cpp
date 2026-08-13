#include <elio/runtime/scheduler.hpp>
#include <elio/coro/task.hpp>
#include <elio/log/macros.hpp>
#include <atomic>
#include <cstdlib>
#include <iostream>
#include <chrono>
#include <memory>
#include <vector>
#include <sys/eventfd.h>
#include <unistd.h>

using namespace elio;
using namespace std::chrono;

coro::task<void> empty_task() {
    co_return;
}

coro::task<void> direct_await_chain(size_t frames) {
    if (frames > 1) {
        co_await direct_await_chain(frames - 1);
    }
}

int main() {
    log::logger::instance().set_level(log::level::error);

    constexpr int N = 100000;

    // NOTE: Tests 1-2 below use detail::task_access to directly measure
    // coroutine frame allocation overhead. This is intentional for low-level
    // performance analysis and requires manual handle management.

    // 1. Measure coroutine frame allocation (cold - first time)
    {
        std::vector<std::coroutine_handle<>> handles;
        handles.reserve(N);

        auto start = high_resolution_clock::now();
        for (int i = 0; i < N; ++i) {
            auto t = empty_task();
            handles.push_back(coro::detail::task_access::release(std::move(t)));
        }
        auto end = high_resolution_clock::now();
        auto ns = duration_cast<nanoseconds>(end - start).count();

        std::cout << "Coroutine frame alloc (cold): " << (ns / N) << " ns/task" << std::endl;

        // Destroy the first-pass frames.
        for (auto h : handles) h.destroy();
    }

    // 2. Repeat frame allocation to expose allocator/cache warm-up effects.
    {
        std::vector<std::coroutine_handle<>> handles;
        handles.reserve(N);

        auto start = high_resolution_clock::now();
        for (int i = 0; i < N; ++i) {
            auto t = empty_task();
            handles.push_back(coro::detail::task_access::release(std::move(t)));
        }
        auto end = high_resolution_clock::now();
        auto ns = duration_cast<nanoseconds>(end - start).count();

        std::cout << "Coroutine frame alloc (warm): " << (ns / N) << " ns/task" << std::endl;

        // Cleanup
        for (auto h : handles) h.destroy();
    }

    // 3. Measure direct Elio task composition.
    {
        constexpr int chain_iterations = 25000;
        constexpr size_t chain_frames = 8;

        auto start = high_resolution_clock::now();
        for (int i = 0; i < chain_iterations; ++i) {
            auto chain = direct_await_chain(chain_frames);
            auto handle = coro::detail::task_access::handle(chain);
            {
                coro::detail::frame_context_scope frame_scope(
                    std::addressof(handle.promise()));
                handle.resume();
            }
            if (!handle.done()) {
                std::abort();
            }
        }
        auto end = high_resolution_clock::now();
        auto ns = duration_cast<nanoseconds>(end - start).count();

        std::cout << "Direct await chain (8 frames): "
                  << (static_cast<double>(ns) /
                      (chain_iterations * chain_frames))
                  << " ns/frame" << std::endl;
    }

    // 4. Measure MPSC push only (no scheduler overhead)
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

    // 5. Measure Chase-Lev push only
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

    // 6. Compare atomic RMW with single-writer snapshot publication
    {
        std::atomic<size_t> published{0};

        auto start = high_resolution_clock::now();
        for (int i = 0; i < N; ++i) {
            published.fetch_add(1, std::memory_order_relaxed);
        }
        auto end = high_resolution_clock::now();
        auto ns = duration_cast<nanoseconds>(end - start).count();

        std::cout << "Atomic counter fetch_add: "
                  << (static_cast<double>(ns) / N)
                  << " ns/update" << std::endl;
    }

    {
        size_t local = 0;
        std::atomic<size_t> published{0};

        auto start = high_resolution_clock::now();
        for (int i = 0; i < N; ++i) {
            published.store(++local, std::memory_order_relaxed);
        }
        auto end = high_resolution_clock::now();
        auto ns = duration_cast<nanoseconds>(end - start).count();

        std::cout << "Single-writer counter publish: "
                  << (static_cast<double>(ns) / N)
                  << " ns/update" << std::endl;
    }

    // 7. Compare exact timestamps with the disabled diagnostic fast path
    {
        std::atomic<steady_clock::time_point> last_task_time{
            steady_clock::now()};

        auto start = high_resolution_clock::now();
        for (int i = 0; i < N; ++i) {
            last_task_time.store(steady_clock::now(),
                                 std::memory_order_relaxed);
        }
        auto end = high_resolution_clock::now();
        auto ns = duration_cast<nanoseconds>(end - start).count();

        std::cout << "Exact task timestamp publish: "
                  << (static_cast<double>(ns) / N)
                  << " ns/update" << std::endl;
    }

    {
        std::atomic<bool> track_task_time{false};

        auto start = high_resolution_clock::now();
        for (int i = 0; i < N; ++i) {
            if (track_task_time.load(std::memory_order_relaxed)) {
                std::atomic_signal_fence(std::memory_order_seq_cst);
            }
        }
        auto end = high_resolution_clock::now();
        auto ns = duration_cast<nanoseconds>(end - start).count();

        std::cout << "Disabled task timestamp check: "
                  << (static_cast<double>(ns) / N)
                  << " ns/update" << std::endl;
    }

    // 8. Measure atomic fence alone
    {
        auto start = high_resolution_clock::now();
        for (int i = 0; i < N; ++i) {
            std::atomic_thread_fence(std::memory_order_release);
        }
        auto end = high_resolution_clock::now();
        auto ns = duration_cast<nanoseconds>(end - start).count();

        std::cout << "Atomic release fence: " << (ns / N) << " ns" << std::endl;
    }

    // 9. Measure eventfd write
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

    // 10. Full spawn path (with running scheduler) - includes alloc + spawn
    {
        runtime::scheduler sched(4);
        sched.start();

        auto start = high_resolution_clock::now();
        for (int i = 0; i < N; ++i) {
            sched.go(empty_task);
        }
        auto end = high_resolution_clock::now();

        // Wait for completion
        while (sched.pending_tasks() > 0) {
            std::this_thread::sleep_for(microseconds(10));
        }

        auto ns = duration_cast<nanoseconds>(end - start).count();
        std::cout << "sched.go() full path: " << (ns / N) << " ns/go" << std::endl;

        sched.shutdown();
    }

    // 11. Measure warmed-up worker overhead
    {
        runtime::scheduler sched(4);
        sched.start();

        // Let workers warm up
        std::this_thread::sleep_for(std::chrono::milliseconds(50));

        auto start = high_resolution_clock::now();
        for (int i = 0; i < N; ++i) {
            sched.go(empty_task);
        }
        auto end = high_resolution_clock::now();

        // Wait for completion
        while (sched.pending_tasks() > 0) {
            std::this_thread::sleep_for(microseconds(10));
        }

        auto ns = duration_cast<nanoseconds>(end - start).count();
        std::cout << "sched.go() (workers warmed): " << (ns / N) << " ns/go" << std::endl;

        sched.shutdown();
    }

    return 0;
}
