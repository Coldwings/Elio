#pragma once

#include "promise_base.hpp"
#include "vthread_stack.hpp"
#include "detail/completion_waiter.hpp"
#include <coroutine>
#include <optional>
#include <exception>
#include <utility>
#include <type_traits>
#include <atomic>
#include <memory>
#include <mutex>
#include <condition_variable>

namespace elio::runtime {
class scheduler;  // Forward declaration
scheduler* get_current_scheduler() noexcept;
void schedule_handle(std::coroutine_handle<> handle) noexcept;
/// Report an unhandled exception from a detached (go/go_to) task.
/// Routes through the scheduler's exception handler if set, otherwise logs ERROR.
/// Defined in scheduler.hpp.
void report_detached_exception(std::exception_ptr ex) noexcept;
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
            // Detached task with no continuation - self-destruct
            // IMPORTANT: Notify join_state before destruction so waiters know
            // the coroutine frame is about to be destroyed
            if (h.promise().join_state_) {
                h.promise().join_state_->mark_destroyed();
            }
            // If this detached task threw an unhandled exception, report it
            // via the scheduler's exception handler (or default log ERROR).
            if (auto ex = h.promise().exception()) {
                runtime::report_detached_exception(std::move(ex));
            }
            // IMPORTANT: If this coroutine owns its vstack, we must release ownership
            // BEFORE destroying the coroutine frame, because the frame itself is allocated
            // on the vstack. Destroying the frame triggers ~promise_base() which would
            // delete the vstack, causing the frame (and its members) to become invalid
            // while still being accessed.
            auto* vstack_to_delete = h.promise().release_vstack_ownership();
            h.destroy();
            // Now safe to delete vstack - the coroutine frame is fully destroyed
            delete vstack_to_delete;
            return std::noop_coroutine();
        } else {
            // Owned task with no continuation - stay suspended for owner to destroy
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
    // Get handle without transferring ownership (for testing)
    template<typename TaskT>
    static auto handle(TaskT& t) noexcept {
        return t.handle_;
    }
    // Set detached flag for go_joinable (needed for destruction notification)
    template<typename TaskT>
    static void set_detached(TaskT& t, bool detached) noexcept {
        if (t.handle_) {
            t.handle_.promise().detached_ = detached;
        }
    }
    // Access join_state from promise (for destruction notification)
    template<typename PromiseT>
    static auto get_join_state(PromiseT& p) noexcept {
        return p.join_state_;
    }
};

struct join_state_base {
    alignas(64) completion_waiter_slot waiter_;
    std::atomic<bool> completed_{false};
    std::atomic<bool> destroyed_{false};
    std::mutex destroyed_mtx_;
    std::condition_variable destroyed_cv_;

    void complete() {
        completed_.store(true, std::memory_order_release);
        auto waiter = waiter_.take();
        if (waiter) {
            runtime::schedule_handle(waiter);
        }
    }

    void mark_destroyed() noexcept {
        {
            std::lock_guard<std::mutex> lock(destroyed_mtx_);
            destroyed_.store(true, std::memory_order_release);
        }
        destroyed_cv_.notify_all();
    }

    void wait_destroyed() {
        std::unique_lock<std::mutex> lock(destroyed_mtx_);
        destroyed_cv_.wait(lock, [&] {
            return destroyed_.load(std::memory_order_acquire);
        });
    }

    [[nodiscard]] bool is_destroyed() const noexcept {
        return destroyed_.load(std::memory_order_acquire);
    }

    [[nodiscard]] bool is_completed() const noexcept {
        return completed_.load(std::memory_order_acquire);
    }

    bool set_waiter(completion_waiter& waiter,
                    std::coroutine_handle<> h) noexcept {
        return waiter_.register_waiter(waiter, h, [this] {
            return completed_.load(std::memory_order_acquire);
        });
    }
};

template<typename T>
struct join_state : join_state_base {
    std::optional<T> value_;
    std::exception_ptr exception_;

    void set_value(T&& value) {
        value_.emplace(std::move(value));
        complete();
    }

    void set_exception(std::exception_ptr ex) {
        exception_ = ex;
        complete();
    }

    T get_value() {
        if (exception_) std::rethrow_exception(exception_);
        return std::move(*value_);
    }
};

template<>
struct join_state<void> : join_state_base {
    std::exception_ptr exception_;

    void set_value() { complete(); }

    void set_exception(std::exception_ptr ex) {
        exception_ = ex;
        complete();
    }

    void get_value() {
        if (exception_) std::rethrow_exception(exception_);
    }
};

} // namespace detail

/// Join handle for awaiting spawned tasks
/// Returned by elio::spawn(), allows co_await to get the result
template<typename T>
class join_handle {
public:
    explicit join_handle(std::shared_ptr<detail::join_state<T>> state) noexcept
        : state_(std::move(state)), waiter_(state_->waiter_) {}

    join_handle(join_handle&&) noexcept = default;
    join_handle& operator=(join_handle&& other) noexcept {
        if (this != &other) {
            waiter_ = std::move(other.waiter_);
            state_ = std::move(other.state_);
        }
        return *this;
    }

    join_handle(const join_handle&) = delete;
    join_handle& operator=(const join_handle&) = delete;

    [[nodiscard]] bool await_ready() const noexcept {
        return state_->is_completed();
    }

    bool await_suspend(std::coroutine_handle<> awaiter) noexcept {
        // Keep a local copy of the shared_ptr to prevent use-after-free.
        auto state = state_;
        return state->set_waiter(waiter_, awaiter);
    }

    T await_resume() {
        // Keep join_state alive during get_value() execution.
        // get_value() may rethrow an exception, and the catch block needs
        // to access the exception object. If join_handle is destroyed before
        // the catch block completes, join_state would be destroyed, potentially
        // causing the exception object to be freed while still being accessed.
        auto state = state_;
        return state->get_value();
    }

    /// Check if the spawned task has completed
    [[nodiscard]] bool is_ready() const noexcept {
        return state_->is_completed();
    }
    
    /// Check if the coroutine has been destroyed (stack released)
    [[nodiscard]] bool is_destroyed() const noexcept {
        return state_->is_destroyed();
    }
    
    /// Block until the coroutine is destroyed (safe from non-coroutine context)
    /// Use this for TSAN-safe synchronization before shutdown()
    void wait_destroyed() const {
        state_->wait_destroyed();
    }

private:
    std::shared_ptr<detail::join_state<T>> state_;
    detail::completion_waiter waiter_;
};

/// Specialization for void
template<>
class join_handle<void> {
public:
    explicit join_handle(std::shared_ptr<detail::join_state<void>> state) noexcept
        : state_(std::move(state)), waiter_(state_->waiter_) {}
    
    join_handle(join_handle&&) noexcept = default;
    join_handle& operator=(join_handle&& other) noexcept {
        if (this != &other) {
            waiter_ = std::move(other.waiter_);
            state_ = std::move(other.state_);
        }
        return *this;
    }
    
    join_handle(const join_handle&) = delete;
    join_handle& operator=(const join_handle&) = delete;
    
    [[nodiscard]] bool await_ready() const noexcept {
        return state_->is_completed();
    }

    bool await_suspend(std::coroutine_handle<> awaiter) noexcept {
        // Keep a local copy of the shared_ptr to prevent use-after-free.
        // See join_handle<T>::await_suspend for detailed explanation.
        auto state = state_;
        return state->set_waiter(waiter_, awaiter);
    }

    void await_resume() {
        // Keep join_state alive during get_value() execution.
        // get_value() may rethrow an exception, and the catch block needs
        // to access the exception object. If join_handle is destroyed before
        // the catch block completes, join_state would be destroyed, potentially
        // causing the exception object to be freed while still being accessed.
        auto state = state_;
        state->get_value();
    }

    [[nodiscard]] bool is_ready() const noexcept {
        return state_->is_completed();
    }
    
    /// Check if the coroutine has been destroyed (stack released)
    [[nodiscard]] bool is_destroyed() const noexcept {
        return state_->is_destroyed();
    }
    
    /// Block until the coroutine is destroyed (safe from non-coroutine context)
    /// Use this for TSAN-safe synchronization before shutdown()
    void wait_destroyed() const {
        state_->wait_destroyed();
    }

private:
    std::shared_ptr<detail::join_state<void>> state_;
    detail::completion_waiter waiter_;
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

        // Safety net: if the coroutine frame is destroyed without going
        // through final_awaiter (e.g., force-destroy during shutdown drain),
        // notify join_state so wait_destroyed() does not deadlock.
        ~promise_type() {
            if (join_state_) {
                join_state_->mark_destroyed();
            }
        }

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
            if (join_state_) {
                try {
                    join_state_->set_value(std::move(*value_));
                } catch (...) {
                    join_state_->set_exception(std::current_exception());
                }
            }
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

    // co_await interface
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

        // Safety net: if the coroutine frame is destroyed without going
        // through final_awaiter (e.g., force-destroy during shutdown drain),
        // notify join_state so wait_destroyed() does not deadlock.
        ~promise_type() {
            if (join_state_) {
                join_state_->mark_destroyed();
            }
        }

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

    // Non-copyable, non-movable
    task(const task&) = delete;
    task& operator=(const task&) = delete;
    task(task&&) = delete;
    task& operator=(task&&) = delete;

    ~task() { if (handle_) handle_.destroy(); }

    // co_await interface
    [[nodiscard]] bool await_ready() const noexcept { return false; }
    [[nodiscard]] std::coroutine_handle<> await_suspend(std::coroutine_handle<> awaiter) noexcept {
        handle_.promise().continuation_ = awaiter;
        return handle_;
    }
    void await_resume() {
        auto& promise = handle_.promise();
        if (promise.exception()) std::rethrow_exception(promise.exception());
    }

private:
    handle_type handle_;
};

} // namespace elio::coro
