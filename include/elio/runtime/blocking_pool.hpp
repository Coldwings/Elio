#pragma once

#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <latch>
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
        std::latch ready(static_cast<std::ptrdiff_t>(num_threads));
        for (size_t i = 0; i < num_threads; ++i) {
            threads_.emplace_back([this, &ready] {
                ready.count_down();
                worker_loop();
            });
        }
        ready.wait();
    }

    ~blocking_pool() {
        shutdown();
    }

    blocking_pool(const blocking_pool&) = delete;
    blocking_pool& operator=(const blocking_pool&) = delete;

    // Submit a task for execution. Thread-safe.
    // If num_threads == 0, spawns a detached thread directly.
    // Otherwise enqueues and wakes one worker.
    //
    // Returns true if the task was accepted (and will eventually run).
    // Returns false if the pool has been shut down; in that case the task
    // is left untouched (still owned by the caller) so the caller can run
    // it elsewhere (e.g. on a detached thread). Without this, a submit that
    // races with shutdown() could enqueue a task no worker will ever run,
    // stranding the caller's awaiter forever.
    [[nodiscard]] bool submit(std::function<void()>&& task) {
        if (num_threads_ == 0) {
            if (stopped_.load(std::memory_order_acquire)) return false;
            std::thread(std::move(task)).detach();
            return true;
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            // Re-check under the mutex so shutdown()'s drain phase (which
            // also acquires this mutex) cannot miss a task we just pushed.
            if (stopped_.load(std::memory_order_relaxed)) return false;
            queue_.push_back(std::move(task));
        }
        cv_.notify_one();
        return true;
    }

    // Graceful shutdown: signals stop, wakes all workers, joins threads.
    // Any tasks left in the queue after the workers exit are drained inline
    // on the calling thread. This catches the race where a submit happened
    // between the stop signal and a worker exiting (the worker may have
    // already returned from its wait loop without picking up the new task).
    // After shutdown() returns, submit() will reject all further submissions.
    void shutdown() {
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (stopped_.exchange(true, std::memory_order_release)) return;
        }
        cv_.notify_all();
        for (auto& t : threads_) {
            if (t.joinable()) t.join();
        }
        // Drain any leftover tasks inline. Running them here (rather than
        // discarding) keeps the "submitted task always runs" contract that
        // spawn_blocking awaiters rely on for resumption.
        std::deque<std::function<void()>> remaining;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            remaining.swap(queue_);
        }
        for (auto& task : remaining) {
            if (task) task();
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
