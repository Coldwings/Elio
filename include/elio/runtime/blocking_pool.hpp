#pragma once

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace elio::runtime {

// A simple thread pool for executing blocking tasks.
// Supports both pooled mode (fixed threads) and non-pooled mode (spawn per task).
class blocking_pool {
public:
    // num_threads: pool size. 0 means no pooling, each submit spawns a new thread.
    explicit blocking_pool(size_t num_threads)
        : num_threads_(num_threads) {
        threads_.reserve(num_threads);
        for (size_t i = 0; i < num_threads; ++i) {
            threads_.emplace_back([this] { worker_loop(); });
        }
    }

    ~blocking_pool() {
        shutdown();
    }

    blocking_pool(const blocking_pool&) = delete;
    blocking_pool& operator=(const blocking_pool&) = delete;

    // Submit a task for execution. Thread-safe.
    // If num_threads == 0, spawns a detached thread directly.
    // Otherwise enqueues and wakes one worker.
    void submit(std::function<void()> task) {
        if (num_threads_ == 0) {
            std::thread(std::move(task)).detach();
            return;
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            queue_.push_back(std::move(task));
        }
        cv_.notify_one();
    }

    // Graceful shutdown: signals stop, wakes all workers, joins threads.
    // Pending tasks in queue are discarded.
    void shutdown() {
        if (stopped_.exchange(true)) return;  // idempotent
        cv_.notify_all();
        for (auto& t : threads_) {
            if (t.joinable()) t.join();
        }
    }

private:
    void worker_loop() {
        while (!stopped_.load(std::memory_order_relaxed)) {
            std::function<void()> task;

            // Block until task available or stopped
            std::unique_lock<std::mutex> lock(mutex_);
            cv_.wait(lock, [this] {
                return stopped_.load(std::memory_order_relaxed) || !queue_.empty();
            });
            if (stopped_.load(std::memory_order_relaxed) && queue_.empty()) return;
            if (!queue_.empty()) {
                task = std::move(queue_.front());
                queue_.pop_front();
            }

            lock.unlock();
            if (task) {
                task();
            }
        }
    }

    std::vector<std::thread> threads_;
    std::deque<std::function<void()>> queue_;
    std::mutex mutex_;
    std::condition_variable cv_;
    std::atomic<bool> stopped_{false};
    size_t num_threads_;
};

}  // namespace elio::runtime
