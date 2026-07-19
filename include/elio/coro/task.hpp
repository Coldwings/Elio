#pragma once

#include "promise_base.hpp"
#include "detail/completion_waiter.hpp"
#include <cassert>
#include <coroutine>
#include <optional>
#include <exception>
#include <utility>
#include <type_traits>
#include <atomic>
#include <memory>
#include <mutex>
#include <condition_variable>
#include <stdexcept>

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

#ifdef ELIO_RUNTIME_TEST_HOOKS
inline std::atomic<bool> pause_before_detached_frame_destroy_for_test{false};
inline std::atomic<bool> detached_frame_destroy_paused_for_test{false};
#endif

struct final_awaiter {
    [[nodiscard]] bool await_ready() const noexcept { return false; }
    
    template<typename Promise>
    [[nodiscard]] std::coroutine_handle<> await_suspend(std::coroutine_handle<Promise> h) noexcept {
        auto continuation = h.promise().continuation_;
        h.promise().leave_frame_context();
        if (continuation) {
            return continuation;
        } else if (h.promise().detached_) {
            // Keep the externally owned join state after freeing the frame so
            // wait_destroyed() observes completed parameter/capture teardown.
            auto join_state = std::move(h.promise().join_state_);
            // If this detached task threw an unhandled exception, report it
            // via the scheduler's exception handler (or default log ERROR).
            if (auto ex = h.promise().exception()) {
                runtime::report_detached_exception(std::move(ex));
            }
#ifdef ELIO_RUNTIME_TEST_HOOKS
            if (join_state &&
                pause_before_detached_frame_destroy_for_test.load(
                    std::memory_order_acquire)) {
                detached_frame_destroy_paused_for_test.store(
                    true, std::memory_order_release);
                detached_frame_destroy_paused_for_test.notify_all();
                while (pause_before_detached_frame_destroy_for_test.load(
                    std::memory_order_acquire)) {
                    pause_before_detached_frame_destroy_for_test.wait(
                        true, std::memory_order_acquire);
                }
                detached_frame_destroy_paused_for_test.store(
                    false, std::memory_order_release);
                detached_frame_destroy_paused_for_test.notify_all();
            }
#endif
            h.destroy();
            if (join_state) {
                join_state->mark_destroyed();
            }
            return std::noop_coroutine();
        } else {
            // Owned task with no continuation - stay suspended for owner to destroy
            return std::noop_coroutine();
        }
    }
    
    void await_resume() const noexcept {}
};

// Friend accessor: explicitly transfer lazy task ownership to the runtime.
struct task_access {
    template<typename TaskT>
        requires (!std::is_lvalue_reference_v<TaskT>)
    static auto release(TaskT&& t) noexcept {
        assert(t.handle_ && "cannot release an empty task");
        t.handle_.promise().detached_ = true;
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

    template<typename T>
    static std::shared_ptr<task_execution_context>
    get_join_execution_context(const join_handle<T>& handle) noexcept;
};

struct join_state_base {
    join_state_base()
        : execution_context_(std::make_shared<task_execution_context>()) {}

    explicit join_state_base(
        std::shared_ptr<task_execution_context> execution_context)
        : execution_context_(require_execution_context(
              std::move(execution_context))) {}

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

    [[nodiscard]] std::shared_ptr<task_execution_context>
    execution_context() const noexcept {
        return execution_context_;
    }

    bool set_waiter(completion_waiter& waiter,
                    std::coroutine_handle<> h) noexcept {
        return waiter_.register_waiter(waiter, h, [this] {
            return completed_.load(std::memory_order_acquire);
        });
    }

private:
    static std::shared_ptr<task_execution_context> require_execution_context(
        std::shared_ptr<task_execution_context> context) {
        if (!context) {
            throw std::invalid_argument(
                "join state requires a task execution context");
        }
        return context;
    }

    const std::shared_ptr<task_execution_context> execution_context_;
};

template<typename T>
struct join_state : join_state_base {
    join_state() = default;
    explicit join_state(std::shared_ptr<task_execution_context> context)
        : join_state_base(std::move(context)) {}

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
    join_state() = default;
    explicit join_state(std::shared_ptr<task_execution_context> context)
        : join_state_base(std::move(context)) {}

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
    friend struct detail::task_access;
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

    /// Request cooperative cancellation of the spawned task chain. This does
    /// not change a completed result, destroy the frame, or guarantee prompt
    /// completion; the task must observe this_coro::cancel_token() directly or
    /// pass it to a cancellation-aware operation.
    void request_cancel() const {
        auto state = state_;
        if (state) {
            state->execution_context()->request_cancel();
        }
    }

    [[nodiscard]] bool is_cancellation_requested() const noexcept {
        auto state = state_;
        return state &&
               state->execution_context()->is_cancellation_requested();
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
    friend struct detail::task_access;
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

    /// Request cooperative cancellation of the spawned task chain. See the
    /// primary join_handle<T> template for the best-effort contract.
    void request_cancel() const {
        auto state = state_;
        if (state) {
            state->execution_context()->request_cancel();
        }
    }

    [[nodiscard]] bool is_cancellation_requested() const noexcept {
        auto state = state_;
        return state &&
               state->execution_context()->is_cancellation_requested();
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

template<typename T>
std::shared_ptr<task_execution_context>
detail::task_access::get_join_execution_context(
    const join_handle<T>& handle) noexcept {
    return handle.state_ ? handle.state_->execution_context() : nullptr;
}

/// Move-only, single-shot lazy coroutine owner for a non-void result.
/// Moving transfers frame ownership and leaves the source empty.
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

    explicit task(handle_type h) noexcept : handle_(h) {
        if (handle_) {
            handle_.promise().leave_creation_context();
        }
    }

    // Move-only lazy coroutine ownership.
    task(const task&) = delete;
    task& operator=(const task&) = delete;
    task(task&& other) noexcept
        : handle_(std::exchange(other.handle_, nullptr)) {}
    task& operator=(task&& other) noexcept {
        if (this != &other) {
            if (handle_) {
                handle_.destroy();
            }
            handle_ = std::exchange(other.handle_, nullptr);
        }
        return *this;
    }

    ~task() { if (handle_) handle_.destroy(); }

    [[nodiscard]] bool valid() const noexcept { return handle_ != nullptr; }
    explicit operator bool() const noexcept { return valid(); }

    // co_await interface
    [[nodiscard]] bool await_ready() const noexcept {
        assert(handle_ && "cannot await an empty task");
        assert(!handle_.done() && "task can only be awaited once");
        return false;
    }
    template<typename AwaiterPromise>
    [[nodiscard]] std::coroutine_handle<> await_suspend(
        std::coroutine_handle<AwaiterPromise> awaiter) {
        assert(handle_ && "cannot await an empty task");
        assert(!handle_.done() && "task can only be awaited once");
        assert(!handle_.promise().continuation_ &&
               "task cannot have multiple awaiters");
        auto* parent = promise_base::current_frame();
        cancel_token parent_token;
        // Runtime cancellation crosses direct awaits between Elio promises.
        // A foreign coroutine promise is an explicit propagation boundary.
        if constexpr (std::is_convertible_v<AwaiterPromise*, promise_base*>) {
            parent = std::addressof(awaiter.promise());
            parent_token = parent->execution_context()->get_cancel_token();
        }
        handle_.promise().link_parent_cancellation(std::move(parent_token));
        handle_.promise().continuation_ = awaiter;
        handle_.promise().enter_frame_context(parent);
        return handle_;
    }
    T await_resume() {
        assert(handle_ && "cannot resume an empty task");
        auto& promise = handle_.promise();
        auto exception = promise.exception();
        promise.detach_from_parent();
        if (exception) std::rethrow_exception(exception);
        return std::move(*promise.value_);
    }

private:
    handle_type handle_;
};

/// Move-only, single-shot lazy coroutine owner for a void result.
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

    explicit task(handle_type h) noexcept : handle_(h) {
        if (handle_) {
            handle_.promise().leave_creation_context();
        }
    }

    // Move-only lazy coroutine ownership.
    task(const task&) = delete;
    task& operator=(const task&) = delete;
    task(task&& other) noexcept
        : handle_(std::exchange(other.handle_, nullptr)) {}
    task& operator=(task&& other) noexcept {
        if (this != &other) {
            if (handle_) {
                handle_.destroy();
            }
            handle_ = std::exchange(other.handle_, nullptr);
        }
        return *this;
    }

    ~task() { if (handle_) handle_.destroy(); }

    [[nodiscard]] bool valid() const noexcept { return handle_ != nullptr; }
    explicit operator bool() const noexcept { return valid(); }

    // co_await interface
    [[nodiscard]] bool await_ready() const noexcept {
        assert(handle_ && "cannot await an empty task");
        assert(!handle_.done() && "task can only be awaited once");
        return false;
    }
    template<typename AwaiterPromise>
    [[nodiscard]] std::coroutine_handle<> await_suspend(
        std::coroutine_handle<AwaiterPromise> awaiter) {
        assert(handle_ && "cannot await an empty task");
        assert(!handle_.done() && "task can only be awaited once");
        assert(!handle_.promise().continuation_ &&
               "task cannot have multiple awaiters");
        auto* parent = promise_base::current_frame();
        cancel_token parent_token;
        // Runtime cancellation crosses direct awaits between Elio promises.
        // A foreign coroutine promise is an explicit propagation boundary.
        if constexpr (std::is_convertible_v<AwaiterPromise*, promise_base*>) {
            parent = std::addressof(awaiter.promise());
            parent_token = parent->execution_context()->get_cancel_token();
        }
        handle_.promise().link_parent_cancellation(std::move(parent_token));
        handle_.promise().continuation_ = awaiter;
        handle_.promise().enter_frame_context(parent);
        return handle_;
    }
    void await_resume() {
        assert(handle_ && "cannot resume an empty task");
        auto& promise = handle_.promise();
        auto exception = promise.exception();
        promise.detach_from_parent();
        if (exception) std::rethrow_exception(exception);
    }

private:
    handle_type handle_;
};

} // namespace elio::coro
