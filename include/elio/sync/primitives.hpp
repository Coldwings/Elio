#pragma once

#include <elio/log/macros.hpp>
#include <elio/runtime/wait_strategy.hpp>
#include <coroutine>
#include <atomic>
#include <mutex>
#include <queue>
#include <optional>
#include <vector>
#include <thread>
#include <chrono>

namespace elio::runtime {
class scheduler;  // Forward declaration

// Get current scheduler - defined in scheduler.hpp
scheduler* get_current_scheduler() noexcept;

// Schedule a handle to run - defined in scheduler.hpp  
void schedule_handle(std::coroutine_handle<> handle) noexcept;
}

namespace elio::sync {

/// Coroutine-aware mutex
/// Unlike std::mutex, this suspends the coroutine instead of blocking the thread
///
/// Lock-free implementation using an intrusive LIFO waiter stack.
/// A single atomic pointer encodes the full lock state:
///   nullptr           — unlocked
///   (void*)this       — locked, no waiters  (LOCKED_NO_WAITERS sentinel)
///   <lock_awaitable*> — locked, head of intrusive LIFO waiter stack
///
/// The uncontended fast path is a single CAS (~3 cycles) with no heap
/// allocation and no OS mutex.  On contention, waiters chain themselves
/// into a lock-free LIFO stack; unlock pops the head and re-schedules it
/// via the coroutine scheduler.
class mutex {
public:
    mutex() = default;
    ~mutex() = default;

    // Non-copyable, non-movable
    mutex(const mutex&) = delete;
    mutex& operator=(const mutex&) = delete;
    mutex(mutex&&) = delete;
    mutex& operator=(mutex&&) = delete;

    /// Lock awaitable — lives in the coroutine frame for the duration of a
    /// co_await m.lock() expression, so it is safe to store 'this' in the
    /// mutex's intrusive waiter list.
    class lock_awaitable {
    public:
        explicit lock_awaitable(mutex& m) noexcept : mutex_(m) {
            // Use release stores to ensure writes are visible to other threads
            // This also helps TSAN understand the synchronization
            next_.store(nullptr, std::memory_order_release);
            handle_.store(nullptr, std::memory_order_release);
        }

        bool await_ready() const noexcept {
            return mutex_.try_lock();
        }

        /// Either acquires the lock inline (returns false = do not suspend) or
        /// pushes this awaitable onto the mutex's LIFO waiter stack and returns
        /// true (suspend).  Loops until one of these two outcomes is achieved
        /// via lock-free CAS.
        bool await_suspend(std::coroutine_handle<> h) noexcept {
            handle_.store(h.address(), std::memory_order_relaxed);
            void* old_state = mutex_.state_.load(std::memory_order_acquire);
            while (true) {
                if (old_state == nullptr) {
                    // Unlocked — try to acquire inline
                    if (mutex_.state_.compare_exchange_weak(
                            old_state, mutex_.locked_no_waiters(),
                            std::memory_order_acquire,
                            std::memory_order_relaxed)) {
                        return false;  // acquired, do not suspend
                    }
                    // CAS failed, old_state refreshed — retry
                } else {
                    // Locked — push this awaitable onto the LIFO stack
                    next_.store((old_state == mutex_.locked_no_waiters())
                                ? nullptr
                                : static_cast<lock_awaitable*>(old_state),
                                std::memory_order_relaxed);
                    if (mutex_.state_.compare_exchange_weak(
                            old_state, this,
                            std::memory_order_release,
                            std::memory_order_relaxed)) {
                        return true;  // enqueued, suspend
                    }
                    // CAS failed, old_state refreshed — retry
                }
            }
        }

        void await_resume() const noexcept {}

    private:
        friend class mutex;
        mutex& mutex_;
        std::atomic<lock_awaitable*> next_;      // intrusive LIFO linkage
        std::atomic<void*> handle_;     // handle to resume on unlock
    };

    /// Acquire the mutex
    [[nodiscard]] auto lock() noexcept { return lock_awaitable(*this); }

    /// Try to acquire the mutex without waiting (lock-free, single CAS)
    bool try_lock() noexcept {
        void* expected = nullptr;
        return state_.compare_exchange_strong(
            expected, locked_no_waiters(),
            std::memory_order_acquire,
            std::memory_order_relaxed);
    }

    /// Release the mutex
    void unlock() noexcept {
        void* state = state_.load(std::memory_order_relaxed);

        if (state == locked_no_waiters()) {
            // Fast path: no waiters — just release
            if (state_.compare_exchange_strong(
                    state, nullptr,
                    std::memory_order_release,
                    std::memory_order_relaxed)) {
                return;
            }
            // A waiter pushed itself between our load and CAS; reload
            state = state_.load(std::memory_order_acquire);
        }

        // Pop head waiter and transfer lock ownership to it (LIFO)
        auto* head = static_cast<lock_awaitable*>(state);
        auto* next = head->next_.load(std::memory_order_acquire);
        void* next_state = (next == nullptr)
                               ? locked_no_waiters()
                               : static_cast<void*>(next);
        state_.store(next_state, std::memory_order_release);

        // Schedule the waiter — it now holds the lock
        auto handle_addr = head->handle_.load(std::memory_order_acquire);
        runtime::schedule_handle(std::coroutine_handle<>::from_address(handle_addr));
    }

    /// Check if mutex is currently locked
    bool is_locked() const noexcept {
        return state_.load(std::memory_order_acquire) != nullptr;
    }

private:
    /// Sentinel value meaning "locked but no waiters".
    /// Uses the mutex's own address — guaranteed to differ from any
    /// lock_awaitable* (awaitables live in coroutine frames, not inside mutexes).
    void* locked_no_waiters() const noexcept {
        return const_cast<void*>(static_cast<const void*>(this));
    }

    // Single atomic encodes the full state (see class-level comment).
    // No separate std::mutex or std::queue needed.
    std::atomic<void*> state_{nullptr};
};

/// RAII lock guard for coroutine mutex
class lock_guard {
public:
    explicit lock_guard(mutex& m) : mutex_(&m), owns_lock_(true) {}
    
    ~lock_guard() {
        if (owns_lock_) {
            mutex_->unlock();
        }
    }
    
    // Non-copyable, movable
    lock_guard(const lock_guard&) = delete;
    lock_guard& operator=(const lock_guard&) = delete;
    
    lock_guard(lock_guard&& other) noexcept
        : mutex_(other.mutex_), owns_lock_(other.owns_lock_) {
        other.owns_lock_ = false;
    }
    
    lock_guard& operator=(lock_guard&& other) noexcept {
        if (this != &other) {
            if (owns_lock_) {
                mutex_->unlock();
            }
            mutex_ = other.mutex_;
            owns_lock_ = other.owns_lock_;
            other.owns_lock_ = false;
        }
        return *this;
    }
    
    void unlock() {
        if (owns_lock_) {
            mutex_->unlock();
            owns_lock_ = false;
        }
    }
    
private:
    mutex* mutex_;
    bool owns_lock_;
};

/// Coroutine-aware shared mutex (read-write lock)
/// Allows multiple readers or a single writer
///
/// Optimized with atomic fast paths for readers:
/// - try_lock_shared uses atomic fetch_add without mutex
/// - Reader-heavy workloads see ~100x improvement
///
/// State encoding (64-bit):
/// - Bit 63: writer_waiting flag
/// - Bit 62: writer_active flag
/// - Bits 0-61: reader_count (max ~4.6 quintillion readers)
class shared_mutex {
public:
    shared_mutex() = default;
    ~shared_mutex() = default;

    // Non-copyable, non-movable
    shared_mutex(const shared_mutex&) = delete;
    shared_mutex& operator=(const shared_mutex&) = delete;
    shared_mutex(shared_mutex&&) = delete;
    shared_mutex& operator=(shared_mutex&&) = delete;

private:
    // State bit masks
    static constexpr uint64_t WRITER_ACTIVE = 1ULL << 62;
    static constexpr uint64_t WRITER_WAITING = 1ULL << 63;
    static constexpr uint64_t READER_MASK = (1ULL << 62) - 1;
    static constexpr uint64_t WRITER_FLAGS = WRITER_ACTIVE | WRITER_WAITING;

public:
    /// Shared lock awaitable (for readers)
    class lock_shared_awaitable {
    public:
        explicit lock_shared_awaitable(shared_mutex& m) : mutex_(m) {}

        bool await_ready() const noexcept {
            return mutex_.try_lock_shared();
        }

        bool await_suspend(std::coroutine_handle<> awaiter) noexcept {
            std::lock_guard<std::mutex> guard(mutex_.internal_mutex_);

            // Check state under lock
            uint64_t state = mutex_.state_.load(std::memory_order_relaxed);
            if (!(state & WRITER_FLAGS)) {
                // No writer active or waiting - acquire read lock
                mutex_.state_.fetch_add(1, std::memory_order_acquire);
                return false;  // Don't suspend, we got the lock
            }

            // Add to reader wait queue
            mutex_.reader_waiters_.push(awaiter);
            return true;  // Suspend
        }

        void await_resume() const noexcept {}

    private:
        shared_mutex& mutex_;
    };

    /// Exclusive lock awaitable (for writers)
    class lock_awaitable {
    public:
        explicit lock_awaitable(shared_mutex& m) : mutex_(m) {}

        bool await_ready() const noexcept {
            return mutex_.try_lock();
        }

        bool await_suspend(std::coroutine_handle<> awaiter) noexcept {
            std::lock_guard<std::mutex> guard(mutex_.internal_mutex_);

            uint64_t state = mutex_.state_.load(std::memory_order_relaxed);
            if (state == 0) {
                // No readers or writers - acquire write lock
                mutex_.state_.store(WRITER_ACTIVE, std::memory_order_release);
                return false;  // Don't suspend, we got the lock
            }

            // Mark writer waiting and add to wait queue
            mutex_.state_.fetch_or(WRITER_WAITING, std::memory_order_relaxed);
            ++mutex_.pending_writers_;
            mutex_.writer_waiters_.push(awaiter);
            return true;  // Suspend
        }

        void await_resume() const noexcept {}

    private:
        shared_mutex& mutex_;
    };

    /// Acquire shared (read) lock
    auto lock_shared() {
        return lock_shared_awaitable(*this);
    }

    /// Acquire exclusive (write) lock
    auto lock() {
        return lock_awaitable(*this);
    }

    /// Try to acquire shared lock without waiting
    /// Lock-free fast path using atomic CAS - no mutex needed in common case
    bool try_lock_shared() noexcept {
        uint64_t state = state_.load(std::memory_order_relaxed);

        // Fast path: if no writer active/waiting, try to increment reader count
        while (!(state & WRITER_FLAGS)) {
            if (state_.compare_exchange_weak(state, state + 1,
                    std::memory_order_acquire, std::memory_order_relaxed)) {
                return true;
            }
            // CAS failed, state was updated - loop will re-check
        }
        return false;
    }

    /// Try to acquire exclusive lock without waiting
    bool try_lock() noexcept {
        uint64_t expected = 0;
        return state_.compare_exchange_strong(expected, WRITER_ACTIVE,
            std::memory_order_acquire, std::memory_order_relaxed);
    }

    /// Release shared (read) lock
    void unlock_shared() {
        // Decrement reader count atomically
        uint64_t prev_state = state_.fetch_sub(1, std::memory_order_release);
        uint64_t new_readers = (prev_state & READER_MASK) - 1;

        // Fast path: if there are still readers or no writer waiting, done
        if (new_readers > 0 || !(prev_state & WRITER_WAITING)) {
            return;
        }

        // Slow path: might need to wake a writer
        std::coroutine_handle<> to_resume;
        {
            std::lock_guard<std::mutex> guard(internal_mutex_);

            // Double-check under lock
            uint64_t state = state_.load(std::memory_order_relaxed);
            if ((state & READER_MASK) == 0 && !writer_waiters_.empty()) {
                auto writer = writer_waiters_.front();
                writer_waiters_.pop();
                --pending_writers_;

                // Clear WRITER_WAITING if no more pending writers, set WRITER_ACTIVE
                uint64_t new_state = WRITER_ACTIVE;
                if (pending_writers_ > 0) {
                    new_state |= WRITER_WAITING;
                }
                state_.store(new_state, std::memory_order_release);
                to_resume = writer;
            }
        }

        if (to_resume) {
            runtime::schedule_handle(to_resume);
        }
    }

    /// Release exclusive (write) lock
    void unlock() {
        std::vector<std::coroutine_handle<>> to_resume;

        {
            std::lock_guard<std::mutex> guard(internal_mutex_);

            // Prefer writers over readers to prevent writer starvation
            if (!writer_waiters_.empty()) {
                auto writer = writer_waiters_.front();
                writer_waiters_.pop();
                --pending_writers_;

                // Keep WRITER_ACTIVE, update WRITER_WAITING based on remaining writers
                uint64_t new_state = WRITER_ACTIVE;
                if (pending_writers_ > 0) {
                    new_state |= WRITER_WAITING;
                }
                state_.store(new_state, std::memory_order_release);
                to_resume.push_back(writer);
            } else {
                // Wake all waiting readers
                size_t reader_count = reader_waiters_.size();
                while (!reader_waiters_.empty()) {
                    to_resume.push_back(reader_waiters_.front());
                    reader_waiters_.pop();
                }
                // Set reader count, preserve WRITER_WAITING if there are pending writers
                uint64_t new_state = reader_count;
                if (pending_writers_ > 0) {
                    new_state |= WRITER_WAITING;
                }
                state_.store(new_state, std::memory_order_release);
            }
        }

        for (auto& h : to_resume) {
            runtime::schedule_handle(h);
        }
    }

    /// Get current reader count
    size_t reader_count() const noexcept {
        return state_.load(std::memory_order_acquire) & READER_MASK;
    }

    /// Check if a writer holds the lock
    bool is_writer_active() const noexcept {
        return (state_.load(std::memory_order_acquire) & WRITER_ACTIVE) != 0;
    }

private:
    // state_ is the hot field: read on every lock_shared() fast path,
    // and written on every reader acquire/release.  Keeping it isolated
    // avoids false sharing with the slow-path internal_mutex_.
    alignas(64) std::atomic<uint64_t> state_{0};  // Packed: [writer_waiting:1][writer_active:1][readers:62]

    // slow-path fields: only accessed under internal_mutex_
    alignas(64) mutable std::mutex internal_mutex_;
    size_t pending_writers_ = 0;       // Count of pending writers (for WRITER_WAITING flag management)
    std::queue<std::coroutine_handle<>> reader_waiters_;
    std::queue<std::coroutine_handle<>> writer_waiters_;
};

/// RAII shared lock guard for shared_mutex (reader lock)
class shared_lock_guard {
public:
    explicit shared_lock_guard(shared_mutex& m) : mutex_(&m), owns_lock_(true) {}
    
    ~shared_lock_guard() {
        if (owns_lock_) {
            mutex_->unlock_shared();
        }
    }
    
    // Non-copyable, movable
    shared_lock_guard(const shared_lock_guard&) = delete;
    shared_lock_guard& operator=(const shared_lock_guard&) = delete;
    
    shared_lock_guard(shared_lock_guard&& other) noexcept
        : mutex_(other.mutex_), owns_lock_(other.owns_lock_) {
        other.owns_lock_ = false;
    }
    
    shared_lock_guard& operator=(shared_lock_guard&& other) noexcept {
        if (this != &other) {
            if (owns_lock_) {
                mutex_->unlock_shared();
            }
            mutex_ = other.mutex_;
            owns_lock_ = other.owns_lock_;
            other.owns_lock_ = false;
        }
        return *this;
    }
    
    void unlock() {
        if (owns_lock_) {
            mutex_->unlock_shared();
            owns_lock_ = false;
        }
    }
    
private:
    shared_mutex* mutex_;
    bool owns_lock_;
};

/// RAII unique lock guard for shared_mutex (writer lock)
class unique_lock_guard {
public:
    explicit unique_lock_guard(shared_mutex& m) : mutex_(&m), owns_lock_(true) {}
    
    ~unique_lock_guard() {
        if (owns_lock_) {
            mutex_->unlock();
        }
    }
    
    // Non-copyable, movable
    unique_lock_guard(const unique_lock_guard&) = delete;
    unique_lock_guard& operator=(const unique_lock_guard&) = delete;
    
    unique_lock_guard(unique_lock_guard&& other) noexcept
        : mutex_(other.mutex_), owns_lock_(other.owns_lock_) {
        other.owns_lock_ = false;
    }
    
    unique_lock_guard& operator=(unique_lock_guard&& other) noexcept {
        if (this != &other) {
            if (owns_lock_) {
                mutex_->unlock();
            }
            mutex_ = other.mutex_;
            owns_lock_ = other.owns_lock_;
            other.owns_lock_ = false;
        }
        return *this;
    }
    
    void unlock() {
        if (owns_lock_) {
            mutex_->unlock();
            owns_lock_ = false;
        }
    }
    
private:
    shared_mutex* mutex_;
    bool owns_lock_;
};

/// Coroutine-aware semaphore
class semaphore {
public:
    explicit semaphore(int initial_count = 0) 
        : count_(initial_count) {}
    
    ~semaphore() = default;
    
    // Non-copyable, non-movable
    semaphore(const semaphore&) = delete;
    semaphore& operator=(const semaphore&) = delete;
    
    /// Acquire awaitable
    class acquire_awaitable {
    public:
        explicit acquire_awaitable(semaphore& s) : sem_(s) {}
        
        bool await_ready() const noexcept {
            return sem_.try_acquire();
        }
        
        bool await_suspend(std::coroutine_handle<> awaiter) noexcept {
            std::lock_guard<std::mutex> guard(sem_.mutex_);
            
            if (sem_.count_ > 0) {
                --sem_.count_;
                return false;  // Don't suspend
            }
            
            sem_.waiters_.push(awaiter);
            return true;  // Suspend
        }
        
        void await_resume() const noexcept {}
        
    private:
        semaphore& sem_;
    };
    
    /// Acquire (decrement) the semaphore
    auto acquire() {
        return acquire_awaitable(*this);
    }
    
    /// Try to acquire without waiting
    bool try_acquire() noexcept {
        std::lock_guard<std::mutex> guard(mutex_);
        if (count_ > 0) {
            --count_;
            return true;
        }
        return false;
    }
    
    /// Release (increment) the semaphore
    void release(int count = 1) {
        std::vector<std::coroutine_handle<>> to_resume;

        {
            std::lock_guard<std::mutex> guard(mutex_);

            // Calculate how many waiters to wake (up to 'count')
            const int to_wake = std::min(count, static_cast<int>(waiters_.size()));

            // Wake up the calculated number of waiters
            for (int i = 0; i < to_wake; ++i) {
                to_resume.push_back(waiters_.front());
                waiters_.pop();
            }

            // Always add the full release count to count_
            // (waiters that were woken will consume permits when they run)
            count_ += count;
        }

        // Re-schedule waiters through the scheduler
        for (auto& h : to_resume) {
            runtime::schedule_handle(h);
        }
    }
    
    /// Get current count
    int count() const noexcept {
        std::lock_guard<std::mutex> guard(mutex_);
        return count_;
    }
    
private:
    mutable std::mutex mutex_;
    int count_;
    std::queue<std::coroutine_handle<>> waiters_;
};

/// Coroutine-aware event (manual reset)
class event {
public:
    event() = default;
    ~event() = default;
    
    // Non-copyable, non-movable
    event(const event&) = delete;
    event& operator=(const event&) = delete;
    
    /// Wait awaitable
    class wait_awaitable {
    public:
        explicit wait_awaitable(event& e) : event_(e) {}
        
        bool await_ready() const noexcept {
            return event_.signaled_.load(std::memory_order_acquire);
        }
        
        bool await_suspend(std::coroutine_handle<> awaiter) noexcept {
            std::lock_guard<std::mutex> guard(event_.mutex_);
            
            if (event_.signaled_.load(std::memory_order_relaxed)) {
                return false;  // Already signaled
            }
            
            event_.waiters_.push(awaiter);
            return true;
        }
        
        void await_resume() const noexcept {}
        
    private:
        event& event_;
    };
    
    /// Wait for the event to be signaled
    auto wait() {
        return wait_awaitable(*this);
    }
    
    /// Signal the event (wake all waiters)
    void set() {
        std::vector<std::coroutine_handle<>> to_resume;
        
        {
            std::lock_guard<std::mutex> guard(mutex_);
            signaled_.store(true, std::memory_order_release);
            
            while (!waiters_.empty()) {
                to_resume.push_back(waiters_.front());
                waiters_.pop();
            }
        }
        
        // Re-schedule waiters through the scheduler
        for (auto& h : to_resume) {
            runtime::schedule_handle(h);
        }
    }
    
    /// Reset the event
    void reset() {
        signaled_.store(false, std::memory_order_release);
    }
    
    /// Check if signaled
    bool is_set() const noexcept {
        return signaled_.load(std::memory_order_acquire);
    }
    
private:
    std::mutex mutex_;
    std::atomic<bool> signaled_{false};
    std::queue<std::coroutine_handle<>> waiters_;
};

/// Multi-producer multi-consumer channel
template<typename T>
class channel {
public:
    /// Create a channel with the given capacity
    /// @param capacity Maximum number of items (0 = unbounded)
    explicit channel(size_t capacity = 0)
        : capacity_(capacity), closed_(false) {}

    ~channel() {
        close();
    }

    // Non-copyable
    channel(const channel&) = delete;
    channel& operator=(const channel&) = delete;

    // Movable
    channel(channel&& other) noexcept
        : capacity_(other.capacity_)
        , closed_(other.closed_)
        , queue_(std::move(other.queue_))
        , recv_waiters_(std::move(other.recv_waiters_))
        , send_waiters_(std::move(other.send_waiters_)) {
        other.capacity_ = 0;
        other.closed_ = true;
    }

    channel& operator=(channel&& other) noexcept(false) {
        if (this != &other) {
            close();
            capacity_ = other.capacity_;
            closed_ = other.closed_;
            queue_ = std::move(other.queue_);
            recv_waiters_ = std::move(other.recv_waiters_);
            send_waiters_ = std::move(other.send_waiters_);
            other.capacity_ = 0;
            other.closed_ = true;
        }
        return *this;
    }
    
    /// Send awaitable
    class send_awaitable {
    public:
        send_awaitable(channel& ch, T value)
            : channel_(ch), value_(std::move(value)) {}
        
        bool await_ready() const noexcept {
            std::lock_guard<std::mutex> guard(channel_.mutex_);
            return channel_.closed_ || 
                   channel_.capacity_ == 0 || 
                   channel_.queue_.size() < channel_.capacity_;
        }
        
        bool await_suspend(std::coroutine_handle<> awaiter) noexcept {
            std::lock_guard<std::mutex> guard(channel_.mutex_);
            
            if (channel_.closed_) {
                return false;  // Channel closed, don't suspend
            }
            
            if (channel_.capacity_ == 0 || 
                channel_.queue_.size() < channel_.capacity_) {
                return false;  // Space available
            }
            
            // Wait for space
            channel_.send_waiters_.push({awaiter, std::move(value_)});
            value_moved_ = true;
            return true;
        }
        
        bool await_resume() {
            std::coroutine_handle<> to_wake;
            bool result;
            
            {
                std::lock_guard<std::mutex> guard(channel_.mutex_);
                
                if (channel_.closed_) {
                    return false;  // Failed to send
                }
                
                if (!value_moved_) {
                    // We weren't suspended, add value now
                    channel_.queue_.push(std::move(value_));
                    
                    // Wake a receiver if any
                    if (!channel_.recv_waiters_.empty()) {
                        to_wake = channel_.recv_waiters_.front();
                        channel_.recv_waiters_.pop();
                    }
                }
                
                result = true;
            }
            
            // Re-schedule outside the lock
            if (to_wake) {
                runtime::schedule_handle(to_wake);
            }
            
            return result;
        }
        
    private:
        channel& channel_;
        T value_;
        bool value_moved_ = false;
    };
    
    /// Receive awaitable
    class recv_awaitable {
    public:
        explicit recv_awaitable(channel& ch) : channel_(ch) {}
        
        bool await_ready() const noexcept {
            std::lock_guard<std::mutex> guard(channel_.mutex_);
            return !channel_.queue_.empty() || channel_.closed_;
        }
        
        bool await_suspend(std::coroutine_handle<> awaiter) noexcept {
            std::lock_guard<std::mutex> guard(channel_.mutex_);
            
            if (!channel_.queue_.empty() || channel_.closed_) {
                return false;
            }
            
            channel_.recv_waiters_.push(awaiter);
            return true;
        }
        
        std::optional<T> await_resume() {
            std::coroutine_handle<> to_wake;
            std::optional<T> result;
            
            {
                std::lock_guard<std::mutex> guard(channel_.mutex_);
                
                // Check if there's a blocked sender
                if (!channel_.send_waiters_.empty()) {
                    auto& [waiter, value] = channel_.send_waiters_.front();
                    channel_.queue_.push(std::move(value));
                    to_wake = waiter;
                    channel_.send_waiters_.pop();
                }
                
                if (!channel_.queue_.empty()) {
                    result = std::move(channel_.queue_.front());
                    channel_.queue_.pop();
                }
                // else: Channel closed and empty - return nullopt
            }
            
            // Re-schedule outside the lock
            if (to_wake) {
                runtime::schedule_handle(to_wake);
            }
            
            return result;
        }
        
    private:
        channel& channel_;
    };
    
    /// Send a value to the channel
    auto send(T value) {
        return send_awaitable(*this, std::move(value));
    }
    
    /// Try to send without waiting
    bool try_send(T value) {
        std::coroutine_handle<> to_wake;
        
        {
            std::lock_guard<std::mutex> guard(mutex_);
            
            if (closed_) {
                return false;
            }
            
            if (capacity_ > 0 && queue_.size() >= capacity_) {
                return false;
            }
            
            queue_.push(std::move(value));
            
            if (!recv_waiters_.empty()) {
                to_wake = recv_waiters_.front();
                recv_waiters_.pop();
            }
        }
        
        // Re-schedule outside the lock
        if (to_wake) {
            runtime::schedule_handle(to_wake);
        }
        
        return true;
    }
    
    /// Receive a value from the channel
    auto recv() {
        return recv_awaitable(*this);
    }
    
    /// Try to receive without waiting
    std::optional<T> try_recv() {
        std::coroutine_handle<> to_wake;
        std::optional<T> result;
        
        {
            std::lock_guard<std::mutex> guard(mutex_);
            
            if (queue_.empty()) {
                return std::nullopt;
            }
            
            result = std::move(queue_.front());
            queue_.pop();
            
            // Wake a sender if any
            if (!send_waiters_.empty()) {
                auto& [waiter, send_value] = send_waiters_.front();
                queue_.push(std::move(send_value));
                to_wake = waiter;
                send_waiters_.pop();
            }
        }
        
        // Re-schedule outside the lock
        if (to_wake) {
            runtime::schedule_handle(to_wake);
        }
        
        return result;
    }
    
    /// Close the channel
    void close() {
        std::vector<std::coroutine_handle<>> to_resume;
        
        {
            std::lock_guard<std::mutex> guard(mutex_);
            
            if (closed_) {
                return;
            }
            
            closed_ = true;
            
            // Wake all waiters
            while (!recv_waiters_.empty()) {
                to_resume.push_back(recv_waiters_.front());
                recv_waiters_.pop();
            }
            
            while (!send_waiters_.empty()) {
                to_resume.push_back(send_waiters_.front().first);
                send_waiters_.pop();
            }
        }
        
        // Re-schedule all waiters through the scheduler
        for (auto& h : to_resume) {
            runtime::schedule_handle(h);
        }
    }
    
    /// Check if channel is closed
    bool is_closed() const noexcept {
        std::lock_guard<std::mutex> guard(mutex_);
        return closed_;
    }
    
    /// Get current queue size
    size_t size() const noexcept {
        std::lock_guard<std::mutex> guard(mutex_);
        return queue_.size();
    }
    
    /// Check if channel is empty
    bool empty() const noexcept {
        std::lock_guard<std::mutex> guard(mutex_);
        return queue_.empty();
    }
    
private:
    mutable std::mutex mutex_;
    std::queue<T> queue_;
    std::queue<std::coroutine_handle<>> recv_waiters_;
    std::queue<std::pair<std::coroutine_handle<>, T>> send_waiters_;
    size_t capacity_;
    bool closed_;
};

/// Coroutine-aware spinlock
/// Uses atomic CAS with cpu_relax() for low-latency locking.
/// Suitable for short critical sections where contention is low.
/// Unlike std::mutex-based elio::sync::mutex, this avoids OS-level synchronization
/// entirely, trading CPU cycles for lower latency.
///
/// For coroutine suspension under contention, use elio::sync::mutex instead.
/// This spinlock is designed for scenarios where the lock is held very briefly
/// and the overhead of thread/coroutine suspension would exceed the spin time.
class spinlock {
public:
    spinlock() = default;
    ~spinlock() = default;

    // Non-copyable, non-movable
    spinlock(const spinlock&) = delete;
    spinlock& operator=(const spinlock&) = delete;
    spinlock(spinlock&&) = delete;
    spinlock& operator=(spinlock&&) = delete;

    /// Acquire the spinlock (spins until acquired)
    void lock() noexcept {
        // Fast path: try once with CAS
        bool expected = false;
        if (locked_.compare_exchange_weak(expected, true,
                std::memory_order_acquire, std::memory_order_relaxed)) {
            return;
        }

        // Slow path: TTAS (Test-and-Test-and-Set) with backoff
        lock_slow();
    }

    /// Try to acquire without spinning
    bool try_lock() noexcept {
        bool expected = false;
        return locked_.compare_exchange_strong(expected, true,
            std::memory_order_acquire, std::memory_order_relaxed);
    }

    /// Release the spinlock
    void unlock() noexcept {
        locked_.store(false, std::memory_order_release);
    }

    /// Check if locked (for debugging only, not reliable for synchronization)
    bool is_locked() const noexcept {
        return locked_.load(std::memory_order_relaxed);
    }

private:
    void lock_slow() noexcept {
        // Exponential backoff parameters
        constexpr int spin_threshold = 8;    // spins before yielding
        constexpr int yield_threshold = 64;  // iterations before sleeping
        constexpr auto max_sleep = std::chrono::microseconds{512};

        int iterations = 0;
        auto sleep_duration = std::chrono::microseconds{1};

        for (;;) {
            // TTAS: spin on read first (avoids cache-line bouncing from CAS)
            while (locked_.load(std::memory_order_relaxed)) {
                ++iterations;

                if (iterations < spin_threshold) {
                    // Initial spinning phase - burn CPU cycles
                    runtime::cpu_relax();
                } else if (iterations < yield_threshold) {
                    // Yield phase - allow other threads to run
                    std::this_thread::yield();
                } else {
                    // Sleep phase - exponential backoff sleep
                    std::this_thread::sleep_for(sleep_duration);
                    sleep_duration = std::min(sleep_duration * 2, max_sleep);
                }
            }

            // Try to acquire
            bool expected = false;
            if (locked_.compare_exchange_weak(expected, true,
                    std::memory_order_acquire, std::memory_order_relaxed)) {
                return;
            }

            // Reset iterations on CAS failure to give the lock holder
            // a chance to release the lock
            if (iterations >= yield_threshold) {
                sleep_duration = std::chrono::microseconds{1};
            }
        }
    }

    std::atomic<bool> locked_{false};
};

/// RAII lock guard for spinlock
class spinlock_guard {
public:
    explicit spinlock_guard(spinlock& s) : spinlock_(&s), owns_lock_(false) {
        spinlock_->lock();
        owns_lock_ = true;
    }

    ~spinlock_guard() {
        if (owns_lock_) {
            spinlock_->unlock();
        }
    }

    // Non-copyable, movable
    spinlock_guard(const spinlock_guard&) = delete;
    spinlock_guard& operator=(const spinlock_guard&) = delete;

    spinlock_guard(spinlock_guard&& other) noexcept
        : spinlock_(other.spinlock_), owns_lock_(other.owns_lock_) {
        other.owns_lock_ = false;
    }

    spinlock_guard& operator=(spinlock_guard&& other) noexcept {
        if (this != &other) {
            if (owns_lock_) {
                spinlock_->unlock();
            }
            spinlock_ = other.spinlock_;
            owns_lock_ = other.owns_lock_;
            other.owns_lock_ = false;
        }
        return *this;
    }

    void unlock() {
        if (owns_lock_) {
            spinlock_->unlock();
            owns_lock_ = false;
        }
    }

private:
    spinlock* spinlock_;
    bool owns_lock_ = true;
};

/// Lock concept for condition_variable - any type with lock()/unlock()
/// that can be used with the condition_variable
namespace detail {

template<typename Lock>
concept lockable = requires(Lock& l) {
    l.unlock();
};

} // namespace detail

/// Coroutine-aware condition variable
/// Suspends the coroutine instead of blocking the thread.
///
/// Can be used with:
/// - elio::sync::mutex (via co_await cv.wait(mutex))
/// - elio::sync::spinlock (via co_await cv.wait(spinlock))
/// - No lock at all (via co_await cv.wait_unlocked()) when all participants
///   are guaranteed to run on the same worker thread
///
/// Supports both notify_one() and notify_all() semantics.
/// The predicate-based wait variants provide spurious-wakeup protection.
class condition_variable {
public:
    condition_variable() = default;
    ~condition_variable() = default;

    // Non-copyable, non-movable
    condition_variable(const condition_variable&) = delete;
    condition_variable& operator=(const condition_variable&) = delete;
    condition_variable(condition_variable&&) = delete;
    condition_variable& operator=(condition_variable&&) = delete;

    /// Wait awaitable for use with elio::sync::mutex
    /// Atomically releases the mutex and suspends the coroutine.
    /// Re-acquires the mutex before resuming.
    class wait_awaitable_mutex {
    public:
        wait_awaitable_mutex(condition_variable& cv, mutex& m)
            : cv_(cv), mutex_(m) {}

        bool await_ready() const noexcept { return false; }

        bool await_suspend(std::coroutine_handle<> awaiter) noexcept {
            awaiter_ = awaiter;
            {
                std::lock_guard<std::mutex> guard(cv_.internal_mutex_);
                cv_.waiters_.push(awaiter);
            }
            // Release the user's mutex after enqueuing
            // This ensures no signal is lost between unlock and suspend
            mutex_.unlock();
            return true;
        }

        // Re-acquire the mutex upon resume by returning a lock awaitable
        // The caller must co_await this result
        auto await_resume() {
            return mutex_.lock();
        }

    private:
        condition_variable& cv_;
        mutex& mutex_;
        std::coroutine_handle<> awaiter_;
    };

    /// Wait awaitable for use with a generic lockable type (e.g., spinlock)
    /// Atomically releases the lock and suspends the coroutine.
    /// Re-acquires the lock before resuming.
    template<detail::lockable Lock>
    class wait_awaitable_lock {
    public:
        wait_awaitable_lock(condition_variable& cv, Lock& lock)
            : cv_(cv), lock_(lock) {}

        bool await_ready() const noexcept { return false; }

        bool await_suspend(std::coroutine_handle<> awaiter) noexcept {
            {
                std::lock_guard<std::mutex> guard(cv_.internal_mutex_);
                cv_.waiters_.push(awaiter);
            }
            // Release the user's lock after enqueuing
            lock_.unlock();
            return true;
        }

        void await_resume() {
            // Re-acquire the lock synchronously (spinlock)
            lock_.lock();
        }

    private:
        condition_variable& cv_;
        Lock& lock_;
    };

    /// Wait awaitable without any external lock
    /// Use only when all participants are guaranteed to run on the same worker thread.
    class wait_awaitable_unlocked {
    public:
        explicit wait_awaitable_unlocked(condition_variable& cv) : cv_(cv) {}

        bool await_ready() const noexcept { return false; }

        bool await_suspend(std::coroutine_handle<> awaiter) noexcept {
            std::lock_guard<std::mutex> guard(cv_.internal_mutex_);
            cv_.waiters_.push(awaiter);
            return true;
        }

        void await_resume() const noexcept {}

    private:
        condition_variable& cv_;
    };

    /// Wait with elio::sync::mutex
    /// The mutex must be locked before calling wait().
    /// Usage:
    ///   co_await mtx.lock();
    ///   while (!condition) {
    ///       co_await co_await cv.wait(mtx);  // double co_await: outer suspends, inner re-locks
    ///   }
    ///   mtx.unlock();
    auto wait(mutex& m) {
        return wait_awaitable_mutex(*this, m);
    }

    /// Wait with a generic lockable (e.g., spinlock)
    /// The lock must be held before calling wait().
    /// Usage:
    ///   sl.lock();
    ///   while (!condition) {
    ///       co_await cv.wait(sl);
    ///   }
    ///   sl.unlock();
    template<detail::lockable Lock>
    auto wait(Lock& lock) {
        return wait_awaitable_lock<Lock>(*this, lock);
    }

    /// Wait without external lock (single-worker only)
    /// Usage:
    ///   while (!condition) {
    ///       co_await cv.wait_unlocked();
    ///   }
    auto wait_unlocked() {
        return wait_awaitable_unlocked(*this);
    }

    /// Wake one waiting coroutine
    void notify_one() {
        std::coroutine_handle<> to_resume;
        {
            std::lock_guard<std::mutex> guard(internal_mutex_);
            if (waiters_.empty()) return;
            to_resume = waiters_.front();
            waiters_.pop();
        }
        runtime::schedule_handle(to_resume);
    }

    /// Wake all waiting coroutines
    void notify_all() {
        std::vector<std::coroutine_handle<>> to_resume;
        {
            std::lock_guard<std::mutex> guard(internal_mutex_);
            while (!waiters_.empty()) {
                to_resume.push_back(waiters_.front());
                waiters_.pop();
            }
        }
        for (auto& h : to_resume) {
            runtime::schedule_handle(h);
        }
    }

    /// Check if there are waiting coroutines
    bool has_waiters() const noexcept {
        std::lock_guard<std::mutex> guard(internal_mutex_);
        return !waiters_.empty();
    }

private:
    mutable std::mutex internal_mutex_;
    std::queue<std::coroutine_handle<>> waiters_;
};

} // namespace elio::sync
