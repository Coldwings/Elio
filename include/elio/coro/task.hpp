#pragma once

#include "promise_base.hpp"
#include "frame_allocator.hpp"
#include <coroutine>
#include <optional>
#include <exception>
#include <utility>
#include <type_traits>
#include <atomic>
#include <memory>

namespace elio::runtime {
class scheduler;  // Forward declaration
scheduler* get_current_scheduler() noexcept;
void schedule_handle(std::coroutine_handle<> handle) noexcept;
}

namespace elio::coro {

template<typename T = void>
class task;

template<typename T = void>
class join_handle;

namespace detail {

/// Intrusive reference-counted join state for thread-safe lifetime management
/// Uses manual ref counting to avoid shared_ptr false positives with TSAN
template<typename T>
struct join_state {
    std::atomic<int> ref_count_{1};  // Start with 1 (owned by promise)
    std::optional<T> value_;
    std::exception_ptr exception_;
    alignas(64) std::atomic<void*> waiter_{nullptr};
    std::atomic<bool> completed_{false};

    void add_ref() noexcept {
        ref_count_.fetch_add(1, std::memory_order_acq_rel);
    }

    void release() noexcept {
        if (ref_count_.fetch_sub(1, std::memory_order_acq_rel) == 1) {
            delete this;
        }
    }

    void set_value(T&& value) {
        value_.emplace(std::move(value));
        complete();
    }

    void set_exception(std::exception_ptr ex) {
        exception_ = ex;
        complete();
    }

    void complete() {
        completed_.store(true, std::memory_order_release);
        void* waiter_addr = waiter_.exchange(nullptr, std::memory_order_acq_rel);
        if (waiter_addr) {
            auto waiter = std::coroutine_handle<>::from_address(waiter_addr);
            runtime::schedule_handle(waiter);
        }
    }

    [[nodiscard]] bool is_completed() const noexcept {
        return completed_.load(std::memory_order_acquire);
    }

    bool set_waiter(std::coroutine_handle<> h) noexcept {
        void* expected = nullptr;
        // Try to set waiter - if CAS succeeds, we own the waiter slot
        // The completion path (complete()) will handle waking us up
        if (waiter_.compare_exchange_strong(expected, h.address(),
                std::memory_order_seq_cst, std::memory_order_acquire)) {
            return true;
        }
        return false;
    }

    T get_value() {
        if (exception_) {
            std::rethrow_exception(exception_);
        }
        return std::move(*value_);
    }
};

/// Specialization for void
template<>
struct join_state<void> {
    std::atomic<int> ref_count_{1};  // Start with 1 (owned by promise)
    std::exception_ptr exception_;
    alignas(64) std::atomic<void*> waiter_{nullptr};
    std::atomic<bool> completed_{false};

    void add_ref() noexcept {
        ref_count_.fetch_add(1, std::memory_order_acq_rel);
    }

    void release() noexcept {
        if (ref_count_.fetch_sub(1, std::memory_order_acq_rel) == 1) {
            delete this;
        }
    }

    void set_value() {
        complete();
    }

    void set_exception(std::exception_ptr ex) {
        exception_ = ex;
        complete();
    }

    void complete() {
        completed_.store(true, std::memory_order_release);
        void* waiter_addr = waiter_.exchange(nullptr, std::memory_order_acq_rel);
        if (waiter_addr) {
            auto waiter = std::coroutine_handle<>::from_address(waiter_addr);
            runtime::schedule_handle(waiter);
        }
    }

    [[nodiscard]] bool is_completed() const noexcept {
        return completed_.load(std::memory_order_acquire);
    }

    bool set_waiter(std::coroutine_handle<> h) noexcept {
        void* expected = nullptr;
        // Try to set waiter - if CAS succeeds, we own the waiter slot
        // The completion path (complete()) will handle waking us up
        if (waiter_.compare_exchange_strong(expected, h.address(),
                std::memory_order_seq_cst, std::memory_order_acquire)) {
            return true;
        }
        return false;
    }

    void get_value() {
        if (exception_) {
            std::rethrow_exception(exception_);
        }
    }
};

struct final_awaiter {
    [[nodiscard]] bool await_ready() const noexcept { return false; }

    template<typename Promise>
    [[nodiscard]] std::coroutine_handle<> await_suspend(std::coroutine_handle<Promise> h) noexcept {
        auto& promise = h.promise();

        // Release join_state reference if this is a spawned task
        if (promise.join_state_) {
            promise.join_state_->release();
            promise.join_state_ = nullptr;
        }

        auto continuation = promise.continuation_;
        if (continuation) {
            return continuation;
        } else if (promise.detached_) {
            // Detached task with no continuation - self-destruct
            h.destroy();
            return std::noop_coroutine();
        } else {
            // Owned task with no continuation - stay suspended for owner to destroy
            return std::noop_coroutine();
        }
    }

    void await_resume() const noexcept {}
};

} // namespace detail

/// Join handle for awaiting spawned tasks
/// Returned by task<T>::spawn(), allows co_await to get the result
template<typename T>
class join_handle {
public:
    explicit join_handle(detail::join_state<T>* state) noexcept
        : state_(state) {}

    join_handle(join_handle&&) noexcept = default;
    join_handle& operator=(join_handle&&) noexcept = default;

    join_handle(const join_handle&) = delete;
    join_handle& operator=(const join_handle&) = delete;

    ~join_handle() {
        if (state_) {
            state_->release();
        }
    }

    [[nodiscard]] bool await_ready() const noexcept {
        return state_ && state_->is_completed();
    }

    bool await_suspend(std::coroutine_handle<> awaiter) noexcept {
        return state_ && state_->set_waiter(awaiter);
    }

    T await_resume() {
        return state_->get_value();
    }

    /// Check if the spawned task has completed
    [[nodiscard]] bool is_ready() const noexcept {
        return state_ && state_->is_completed();
    }

private:
    detail::join_state<T>* state_;
};

/// Specialization for void
template<>
class join_handle<void> {
public:
    explicit join_handle(detail::join_state<void>* state) noexcept
        : state_(state) {}

    join_handle(join_handle&&) noexcept = default;
    join_handle& operator=(join_handle&&) noexcept = default;

    join_handle(const join_handle&) = delete;
    join_handle& operator=(const join_handle&) = delete;

    ~join_handle() {
        if (state_) {
            state_->release();
        }
    }

    [[nodiscard]] bool await_ready() const noexcept {
        return state_ && state_->is_completed();
    }

    bool await_suspend(std::coroutine_handle<> awaiter) noexcept {
        return state_ && state_->set_waiter(awaiter);
    }

    void await_resume() {
        state_->get_value();
    }

    [[nodiscard]] bool is_ready() const noexcept {
        return state_ && state_->is_completed();
    }

private:
    detail::join_state<void>* state_;
};

/// Primary template for task<T> where T is not void
template<typename T>
class task {
public:
    struct promise_type : promise_base {
        std::optional<T> value_;
        std::coroutine_handle<> continuation_;
        bool detached_ = false;
        // Join state for spawn() - intrusive ref-counted for thread safety
        detail::join_state<T>* join_state_ = nullptr;

        promise_type() noexcept : promise_base() {}

        [[nodiscard]] task get_return_object() noexcept {
            return task{std::coroutine_handle<promise_type>::from_promise(*this)};
        }

        [[nodiscard]] std::suspend_always initial_suspend() noexcept { return {}; }
        [[nodiscard]] detail::final_awaiter final_suspend() noexcept { return {}; }

        template<typename U>
        void return_value(U&& value) {
            value_.emplace(std::forward<U>(value));
            // Notify join state if spawned
            if (join_state_) {
                join_state_->set_value(std::move(*value_));
            }
        }

        void unhandled_exception() noexcept {
            promise_base::unhandled_exception();
            // Notify join state if spawned
            if (join_state_) {
                join_state_->set_exception(exception());
            }
        }

        // Custom allocator for coroutine frames
        void* operator new(size_t size) {
            return frame_allocator::allocate(size);
        }

        void operator delete(void* ptr, size_t size) noexcept {
            frame_allocator::deallocate(ptr, size);
        }
    };

    using handle_type = std::coroutine_handle<promise_type>;

    explicit task(handle_type handle) noexcept : handle_(handle) {}
    task(task&& other) noexcept : handle_(std::exchange(other.handle_, nullptr)) {}

    task& operator=(task&& other) noexcept {
        if (this != &other) {
            if (handle_) handle_.destroy();
            handle_ = std::exchange(other.handle_, nullptr);
        }
        return *this;
    }

    ~task() { if (handle_) handle_.destroy(); }

    task(const task&) = delete;
    task& operator=(const task&) = delete;

    [[nodiscard]] handle_type handle() const noexcept { return handle_; }
    [[nodiscard]] handle_type release() noexcept {
        if (handle_) handle_.promise().detached_ = true;
        return std::exchange(handle_, nullptr);
    }

    /// Spawn this task on the current scheduler (fire-and-forget)
    /// The task will run asynchronously and self-destruct when complete
    void go() {
        runtime::schedule_handle(release());
    }

    /// Spawn this task and return a join_handle for awaiting the result
    /// Usage: auto handle = some_task().spawn(); T result = co_await handle;
    [[nodiscard]] join_handle<T> spawn();

    [[nodiscard]] bool await_ready() const noexcept { return false; }

    [[nodiscard]] std::coroutine_handle<> await_suspend(std::coroutine_handle<> awaiter) noexcept {
        handle_.promise().continuation_ = awaiter;
        return handle_;
    }

    T await_resume() {
        auto& promise = handle_.promise();
        if (promise.exception()) {
            std::rethrow_exception(promise.exception());
        }
        return std::move(*promise.value_);
    }

private:
    handle_type handle_;
};

/// Specialization for task<void>
template<>
class task<void> {
public:
    struct promise_type : promise_base {
        std::coroutine_handle<> continuation_;
        bool detached_ = false;
        // Join state for spawn() - intrusive ref-counted for thread safety
        detail::join_state<void>* join_state_ = nullptr;

        promise_type() noexcept : promise_base() {}

        [[nodiscard]] task get_return_object() noexcept {
            return task{std::coroutine_handle<promise_type>::from_promise(*this)};
        }

        [[nodiscard]] std::suspend_always initial_suspend() noexcept { return {}; }
        [[nodiscard]] detail::final_awaiter final_suspend() noexcept { return {}; }

        void return_void() noexcept {
            // Notify join state if spawned
            if (join_state_) {
                join_state_->set_value();
            }
        }

        void unhandled_exception() noexcept {
            promise_base::unhandled_exception();
            // Notify join state if spawned
            if (join_state_) {
                join_state_->set_exception(exception());
            }
        }

        // Custom allocator for coroutine frames
        void* operator new(size_t size) {
            return frame_allocator::allocate(size);
        }

        void operator delete(void* ptr, size_t size) noexcept {
            frame_allocator::deallocate(ptr, size);
        }
    };

    using handle_type = std::coroutine_handle<promise_type>;

    explicit task(handle_type handle) noexcept : handle_(handle) {}
    task(task&& other) noexcept : handle_(std::exchange(other.handle_, nullptr)) {}

    task& operator=(task&& other) noexcept {
        if (this != &other) {
            if (handle_) handle_.destroy();
            handle_ = std::exchange(other.handle_, nullptr);
        }
        return *this;
    }

    ~task() { if (handle_) handle_.destroy(); }

    task(const task&) = delete;
    task& operator=(const task&) = delete;

    [[nodiscard]] handle_type handle() const noexcept { return handle_; }
    [[nodiscard]] handle_type release() noexcept {
        if (handle_) handle_.promise().detached_ = true;
        return std::exchange(handle_, nullptr);
    }

    /// Spawn this task on the current scheduler (fire-and-forget)
    /// The task will run asynchronously and self-destruct when complete
    void go() {
        runtime::schedule_handle(release());
    }

    /// Spawn this task and return a join_handle for awaiting completion
    /// Usage: auto handle = some_task().spawn(); co_await handle;
    [[nodiscard]] join_handle<void> spawn();

    [[nodiscard]] bool await_ready() const noexcept { return false; }

    [[nodiscard]] std::coroutine_handle<> await_suspend(std::coroutine_handle<> awaiter) noexcept {
        handle_.promise().continuation_ = awaiter;
        return handle_;
    }

    void await_resume() {
        auto& promise = handle_.promise();
        if (promise.exception()) {
            std::rethrow_exception(promise.exception());
        }
    }

private:
    handle_type handle_;
};

// Out-of-line definitions for spawn() methods
template<typename T>
join_handle<T> task<T>::spawn() {
    // Create intrusive join_state - starts with ref_count=1 (owned by promise)
    auto* join_state = new detail::join_state<T>();
    handle_.promise().join_state_ = join_state;

    // Add ref for join_handle (so join_state lives as long as join_handle)
    join_state->add_ref();

    // Release and schedule - the promise will notify join state on completion
    runtime::schedule_handle(release());
    return join_handle<T>(join_state);
}

inline join_handle<void> task<void>::spawn() {
    auto* join_state = new detail::join_state<void>();
    handle_.promise().join_state_ = join_state;

    // Add ref for join_handle
    join_state->add_ref();

    runtime::schedule_handle(release());
    return join_handle<void>(join_state);
}

} // namespace elio::coro
