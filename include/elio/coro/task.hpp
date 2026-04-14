#pragma once

#include "promise_base.hpp"
#include "vthread_stack.hpp"
#include <coroutine>
#include <optional>
#include <exception>
#include <utility>
#include <type_traits>
#include <atomic>
#include <memory>

namespace elio::runtime {
class scheduler;
scheduler* get_current_scheduler() noexcept;
void schedule_handle(std::coroutine_handle<> handle) noexcept;
}

namespace elio::coro {

template<typename T = void>
class task;

template<typename T = void>
class join_handle;

namespace detail {

struct final_awaiter {
    [[nodiscard]] bool await_ready() const noexcept { return false; }
    
    template<typename Promise>
    [[nodiscard]] std::coroutine_handle<> await_suspend(std::coroutine_handle<Promise> h) noexcept {
        auto continuation = h.promise().continuation_;
        if (continuation) {
            return continuation;
        } else if (h.promise().detached_) {
            auto* vstack_to_delete = h.promise().release_vstack_ownership();
            h.destroy();
            delete vstack_to_delete;
            return std::noop_coroutine();
        } else {
            return std::noop_coroutine();
        }
    }
    
    void await_resume() const noexcept {}
};

// Friend accessor: extract handle from immovable task<T>
struct task_access {
    template<typename TaskT>
    static auto release(TaskT& t) noexcept {
        if (t.handle_) {
            t.handle_.promise().detached_ = true;
        }
        return std::exchange(t.handle_, nullptr);
    }
    template<typename TaskT>
    static auto handle(TaskT& t) noexcept {
        return t.handle_;
    }
};

/// Shared state for join_handle<T> — stores result and waiter
template<typename T>
struct join_state {
    std::optional<T> value_;
    std::exception_ptr exception_;
    alignas(64) std::atomic<void*> waiter_{nullptr};
    std::atomic<bool> completed_{false};
    
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
        if (waiter_.compare_exchange_strong(expected, h.address(), 
                std::memory_order_release, std::memory_order_acquire)) {
            if (completed_.load(std::memory_order_acquire)) {
                void* addr = waiter_.exchange(nullptr, std::memory_order_acq_rel);
                if (addr) {
                    runtime::schedule_handle(std::coroutine_handle<>::from_address(addr));
                }
                return false;
            }
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

template<>
struct join_state<void> {
    std::exception_ptr exception_;
    alignas(64) std::atomic<void*> waiter_{nullptr};
    std::atomic<bool> completed_{false};
    
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
        if (waiter_.compare_exchange_strong(expected, h.address(),
                std::memory_order_release, std::memory_order_acquire)) {
            if (completed_.load(std::memory_order_acquire)) {
                void* addr = waiter_.exchange(nullptr, std::memory_order_acq_rel);
                if (addr) {
                    runtime::schedule_handle(std::coroutine_handle<>::from_address(addr));
                }
                return false;
            }
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

} // namespace detail

/// Join handle for awaiting spawned tasks
/// Uses shared_ptr internally so it is safely movable — moving transfers
/// the shared_ptr ownership without invalidating the awaiting coroutine.
/// The join_state survives moves because it lives on the heap.
template<typename T>
class join_handle {
public:
    explicit join_handle(std::shared_ptr<detail::join_state<T>> state) noexcept
        : state_(std::move(state)) {}

    join_handle(join_handle&&) noexcept = default;
    join_handle& operator=(join_handle&&) noexcept = default;

    join_handle(const join_handle&) = delete;
    join_handle& operator=(const join_handle&) = delete;

    [[nodiscard]] bool await_ready() const noexcept {
        return state_ && state_->is_completed();
    }

    bool await_suspend(std::coroutine_handle<> awaiter) noexcept {
        if (!state_) {
            // Moved-from join_handle: nothing to await, resume immediately
            runtime::schedule_handle(awaiter);
            return false;
        }
        auto state = state_;
        return state->set_waiter(awaiter);
    }

    T await_resume() {
        if (!state_) {
            if constexpr (!std::is_void_v<T>) {
                return T{};
            }
            return;
        }
        return state_->get_value();
    }

    /// Check if the spawned task has completed
    [[nodiscard]] bool is_ready() const noexcept {
        return state_ && state_->is_completed();
    }

    /// Check if this handle is valid (not moved-from)
    [[nodiscard]] bool valid() const noexcept {
        return static_cast<bool>(state_);
    }

private:
    std::shared_ptr<detail::join_state<T>> state_;
};

template<>
class join_handle<void> {
public:
    explicit join_handle(std::shared_ptr<detail::join_state<void>> state) noexcept
        : state_(std::move(state)) {}
    
    join_handle(join_handle&&) noexcept = default;
    join_handle& operator=(join_handle&&) noexcept = default;
    
    join_handle(const join_handle&) = delete;
    join_handle& operator=(const join_handle&) = delete;
    
    [[nodiscard]] bool await_ready() const noexcept {
        return state_ && state_->is_completed();
    }

    bool await_suspend(std::coroutine_handle<> awaiter) noexcept {
        if (!state_) {
            runtime::schedule_handle(awaiter);
            return false;
        }
        auto state = state_;
        return state->set_waiter(awaiter);
    }

    void await_resume() {
        if (state_) {
            state_->get_value();
        }
    }

    [[nodiscard]] bool is_ready() const noexcept {
        return state_ && state_->is_completed();
    }

    [[nodiscard]] bool valid() const noexcept {
        return static_cast<bool>(state_);
    }

private:
    std::shared_ptr<detail::join_state<void>> state_;
};

/// Primary template for task<T> where T is not void
template<typename T>
class task {
    friend struct detail::task_access;
public:
    using value_type = T;

    struct promise_type : promise_base {
        std::optional<T> value_;
        std::coroutine_handle<> continuation_;
        bool detached_ = false;
        std::shared_ptr<detail::join_state<T>> join_state_;

        void* operator new(size_t size) {
            return vthread_stack::allocate(size);
        }
        void operator delete(void* ptr, size_t size) noexcept {
            vthread_stack::deallocate(ptr, size);
        }

        [[nodiscard]] task get_return_object() noexcept {
            return task{std::coroutine_handle<promise_type>::from_promise(*this)};
        }
        [[nodiscard]] std::suspend_always initial_suspend() noexcept { return {}; }
        [[nodiscard]] detail::final_awaiter final_suspend() noexcept { return {}; }

        template<typename U>
        void return_value(U&& value) {
            value_.emplace(std::forward<U>(value));
            if (join_state_) join_state_->set_value(std::move(*value_));
        }

        void unhandled_exception() noexcept {
            promise_base::unhandled_exception();
            if (join_state_) join_state_->set_exception(exception());
        }
    };

    using handle_type = std::coroutine_handle<promise_type>;

    explicit task(handle_type h) noexcept : handle_(h) {}

    // Non-copyable, non-movable
    task(const task&) = delete;
    task& operator=(const task&) = delete;
    task(task&&) = delete;
    task& operator=(task&&) = delete;

    ~task() { if (handle_) handle_.destroy(); }

    [[nodiscard]] bool await_ready() const noexcept { return false; }
    [[nodiscard]] std::coroutine_handle<> await_suspend(std::coroutine_handle<> awaiter) noexcept {
        handle_.promise().continuation_ = awaiter;
        return handle_;
    }
    T await_resume() {
        auto& promise = handle_.promise();
        if (promise.exception()) std::rethrow_exception(promise.exception());
        return std::move(*promise.value_);
    }

    /// Spawn this task and return a join_handle to await its result.
    /// After calling spawn(), this task is consumed (handle becomes null).
    join_handle<T> spawn() && {
        auto h = std::exchange(handle_, nullptr);
        auto state = std::make_shared<detail::join_state<T>>();
        h.promise().join_state_ = state;
        h.promise().detached_ = true;
        h.promise().detach_from_parent();
        runtime::schedule_handle(h);
        return join_handle<T>{std::move(state)};
    }

private:
    handle_type handle_;
};

/// Specialization for task<void>
template<>
class task<void> {
    friend struct detail::task_access;
public:
    using value_type = void;

    struct promise_type : promise_base {
        std::coroutine_handle<> continuation_;
        bool detached_ = false;
        std::shared_ptr<detail::join_state<void>> join_state_;

        void* operator new(size_t size) {
            return vthread_stack::allocate(size);
        }
        void operator delete(void* ptr, size_t size) noexcept {
            vthread_stack::deallocate(ptr, size);
        }

        [[nodiscard]] task get_return_object() noexcept {
            return task{std::coroutine_handle<promise_type>::from_promise(*this)};
        }
        [[nodiscard]] std::suspend_always initial_suspend() noexcept { return {}; }
        [[nodiscard]] detail::final_awaiter final_suspend() noexcept { return {}; }

        void return_void() noexcept {
            if (join_state_) join_state_->set_value();
        }

        void unhandled_exception() noexcept {
            promise_base::unhandled_exception();
            if (join_state_) join_state_->set_exception(exception());
        }
    };

    using handle_type = std::coroutine_handle<promise_type>;

    explicit task(handle_type h) noexcept : handle_(h) {}

    task(const task&) = delete;
    task& operator=(const task&) = delete;
    task(task&&) = delete;
    task& operator=(task&&) = delete;

    ~task() { if (handle_) handle_.destroy(); }

    [[nodiscard]] bool await_ready() const noexcept { return false; }
    [[nodiscard]] std::coroutine_handle<> await_suspend(std::coroutine_handle<> awaiter) noexcept {
        handle_.promise().continuation_ = awaiter;
        return handle_;
    }
    void await_resume() {
        auto& promise = handle_.promise();
        if (promise.exception()) std::rethrow_exception(promise.exception());
    }

    join_handle<void> spawn() && {
        auto h = std::exchange(handle_, nullptr);
        auto state = std::make_shared<detail::join_state<void>>();
        h.promise().join_state_ = state;
        h.promise().detached_ = true;
        h.promise().detach_from_parent();
        runtime::schedule_handle(h);
        return join_handle<void>{std::move(state)};
    }

private:
    handle_type handle_;
};

} // namespace elio::coro
