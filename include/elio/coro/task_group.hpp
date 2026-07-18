#pragma once

#include "task.hpp"
#include "this_coro.hpp"
#include "traits.hpp"
#include "detail/completion_waiter.hpp"
#include "../runtime/scheduler.hpp"
#include "../sync/semaphore.hpp"

#include <climits>
#include <cstddef>
#include <coroutine>
#include <exception>
#include <functional>
#include <limits>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <tuple>
#include <type_traits>
#include <utility>
#include <vector>

namespace elio::coro {

/// Controls whether the first child failure cancels unfinished siblings.
enum class task_group_failure_policy {
    fail_fast,
    collect_all,
};

struct task_group_options {
    task_group_failure_policy failure_policy =
        task_group_failure_policy::fail_fast;

    /// Maximum child bodies executing concurrently. Zero means unlimited.
    size_t max_concurrency = 0;
};

/// Aggregate failure reported by a collect-all group after every child ends.
class task_group_error final : public std::exception {
public:
    explicit task_group_error(std::vector<std::exception_ptr> failures) noexcept
        : failures_(std::move(failures)) {}

    [[nodiscard]] const char* what() const noexcept override {
        return "one or more task_group children failed";
    }

    [[nodiscard]] const std::vector<std::exception_ptr>&
    failures() const noexcept {
        return failures_;
    }

private:
    std::vector<std::exception_ptr> failures_;
};

namespace detail {

class task_group_state final
    : public std::enable_shared_from_this<task_group_state> {
public:
    class all_done_awaitable final {
    public:
        explicit all_done_awaitable(task_group_state& state) noexcept
            : state_(state), waiter_(state.all_done_waiter_) {}

        [[nodiscard]] bool await_ready() const noexcept {
            return state_.outstanding_children() == 0;
        }

        bool await_suspend(std::coroutine_handle<> handle) noexcept {
            return state_.all_done_waiter_.register_waiter(
                waiter_, handle, [this] {
                    return state_.outstanding_children() == 0;
                });
        }

        void await_resume() const noexcept {}

    private:
        task_group_state& state_;
        completion_waiter waiter_;
    };

    task_group_state(runtime::scheduler& scheduler,
                     task_group_options options)
        : scheduler_(std::addressof(scheduler)), options_(options) {
        if (options_.max_concurrency > static_cast<size_t>(INT_MAX)) {
            throw std::invalid_argument(
                "task_group max_concurrency exceeds semaphore range");
        }
        if (options_.max_concurrency != 0) {
            permits_ = std::make_unique<sync::semaphore>(
                static_cast<int>(options_.max_concurrency));
        }
    }

    task_group_state(const task_group_state&) = delete;
    task_group_state& operator=(const task_group_state&) = delete;

    void link_parent(cancel_token parent) {
        auto weak_state = weak_from_this();
        parent_registration_ = parent.on_cancel(
            [weak_state = std::move(weak_state)] {
                if (auto state = weak_state.lock()) {
                    state->request_cancel();
                }
            });
    }

    [[nodiscard]] cancel_token token() const noexcept {
        return cancellation_.token();
    }

    void request_cancel() {
        auto* current_scheduler = runtime::scheduler::current();
        if (current_scheduler == scheduler_) {
            cancellation_.request_cancel();
            return;
        }

        if (current_scheduler != nullptr) {
            // Never block one scheduler worker on another scheduler: reciprocal
            // cancellation would otherwise deadlock two single-worker domains.
            auto state = shared_from_this();
            auto dispatched = scheduler_->go_joinable(
                [state = std::move(state)]() -> task<void> {
                    try {
                        state->cancellation_.request_cancel();
                    } catch (...) {
                        state->append_failure(std::current_exception());
                    }
                    co_return;
                });
            if (dispatched.is_ready()) {
                dispatched.await_resume();
            }
            return;
        }

        auto state = shared_from_this();
        auto dispatched = scheduler_->go_joinable(
            [state = std::move(state)]() -> task<void> {
                state->cancellation_.request_cancel();
                co_return;
            });
        dispatched.wait_destroyed();
        dispatched.await_resume();
    }

    void request_cancel_noexcept() noexcept {
        try {
            request_cancel();
        } catch (...) {
            append_failure(std::current_exception());
        }
    }

    [[nodiscard]] bool is_cancellation_requested() const noexcept {
        return cancellation_.is_cancellation_requested();
    }

    void register_child() {
        std::lock_guard<std::mutex> lock(children_mutex_);
        if (outstanding_children_ ==
            std::numeric_limits<size_t>::max()) {
            throw std::overflow_error("task_group child count overflow");
        }
        ++outstanding_children_;
    }

    void child_finished() noexcept {
        bool completed = false;
        {
            std::lock_guard<std::mutex> lock(children_mutex_);
            if (outstanding_children_ == 0) {
                return;
            }
            --outstanding_children_;
            completed = outstanding_children_ == 0;
        }

        if (completed) {
            auto waiter = all_done_waiter_.take();
            if (waiter) {
                resume_join_waiter(waiter);
            }
        }
    }

    [[nodiscard]] size_t outstanding_children() const noexcept {
        std::lock_guard<std::mutex> lock(children_mutex_);
        return outstanding_children_;
    }

    [[nodiscard]] all_done_awaitable wait_all() noexcept {
        return all_done_awaitable(*this);
    }

    [[nodiscard]] bool has_concurrency_limit() const noexcept {
        return static_cast<bool>(permits_);
    }

    auto acquire_permit(cancel_token token) {
        return permits_->acquire(std::move(token));
    }

    void release_permit() noexcept {
        if (!permits_) {
            return;
        }

        try {
            permits_->release();
        } catch (...) {
            append_failure(std::current_exception());
            // An unreleased permit could otherwise strand queued children.
            request_cancel_noexcept();
        }
    }

    void record_failure(std::exception_ptr failure) noexcept {
        bool cancel_siblings = false;
        {
            std::lock_guard<std::mutex> lock(failures_mutex_);
            if (!first_failure_) {
                first_failure_ = failure;
                cancel_siblings =
                    options_.failure_policy ==
                    task_group_failure_policy::fail_fast;
            }
            try {
                failures_.push_back(std::move(failure));
            } catch (...) {
                // first_failure_ still preserves one reportable failure.
            }
        }

        if (cancel_siblings) {
            request_cancel_noexcept();
        }
    }

    [[nodiscard]] std::exception_ptr first_failure() const noexcept {
        std::lock_guard<std::mutex> lock(failures_mutex_);
        return first_failure_;
    }

    [[nodiscard]] std::vector<std::exception_ptr> failures() const {
        std::lock_guard<std::mutex> lock(failures_mutex_);
        return failures_;
    }

    [[nodiscard]] task_group_options options() const noexcept {
        return options_;
    }

private:
    void resume_join_waiter(std::coroutine_handle<> waiter) noexcept {
        if (scheduler_->try_schedule(waiter)) {
            return;
        }

        if (runtime::scheduler::current() == scheduler_) {
            auto* promise = get_promise_base(waiter.address());
            detail::frame_context_scope frame_scope(promise);
            waiter.resume();
            return;
        }

        // Cross-thread child teardown is possible during ownership handoff.
        // Keep the continuation in its scheduler domain and rely on the public
        // lifetime contract that the scheduler runs until the group drains.
        while (scheduler_->is_running()) {
            std::this_thread::yield();
            if (scheduler_->try_schedule(waiter)) {
                return;
            }
        }
    }

    void append_failure(std::exception_ptr failure) noexcept {
        std::lock_guard<std::mutex> lock(failures_mutex_);
        if (!first_failure_) {
            first_failure_ = failure;
        }
        try {
            failures_.push_back(std::move(failure));
        } catch (...) {
            // first_failure_ still preserves one reportable failure.
        }
    }

    runtime::scheduler* const scheduler_;
    const task_group_options options_;
    cancellation_context cancellation_;
    std::unique_ptr<sync::semaphore> permits_;

    mutable std::mutex children_mutex_;
    size_t outstanding_children_ = 0;
    completion_waiter_slot all_done_waiter_;

    mutable std::mutex failures_mutex_;
    std::exception_ptr first_failure_;
    std::vector<std::exception_ptr> failures_;

    // Unregister from the parent before releasing the group cancellation state.
    cancel_registration parent_registration_;
};

class task_group_child_registration final {
public:
    explicit task_group_child_registration(
        std::shared_ptr<task_group_state> state) noexcept
        : state_(std::move(state)) {}

    task_group_child_registration(task_group_child_registration&& other) noexcept
        : state_(std::move(other.state_)) {}

    task_group_child_registration& operator=(
        task_group_child_registration&& other) noexcept {
        if (this != &other) {
            finish();
            state_ = std::move(other.state_);
        }
        return *this;
    }

    task_group_child_registration(const task_group_child_registration&) = delete;
    task_group_child_registration& operator=(
        const task_group_child_registration&) = delete;

    ~task_group_child_registration() {
        finish();
    }

    [[nodiscard]] std::shared_ptr<task_group_state> state() const noexcept {
        return state_;
    }

private:
    void finish() noexcept {
        if (state_) {
            state_->child_finished();
            state_.reset();
        }
    }

    std::shared_ptr<task_group_state> state_;
};

template<typename F, typename... Args>
class task_group_child_operation final {
public:
    task_group_child_operation(task_group_child_registration registration,
                               F function, Args... args)
        : registration_(std::move(registration))
        , function_(std::move(function))
        , args_(std::move(args)...) {}

    task_group_child_operation(task_group_child_operation&&) = default;
    task_group_child_operation& operator=(task_group_child_operation&&) = delete;
    task_group_child_operation(const task_group_child_operation&) = delete;
    task_group_child_operation& operator=(
        const task_group_child_operation&) = delete;

    [[nodiscard]] std::shared_ptr<task_group_state> state() const noexcept {
        return registration_.state();
    }

    auto invoke() && -> std::invoke_result_t<F, Args...> {
        return std::apply(
            [this](auto&... args) -> std::invoke_result_t<F, Args...> {
                return std::invoke(
                    std::move(function_), std::move(args)...);
            },
            args_);
    }

private:
    // Declared first so it is destroyed after the user callable and arguments.
    task_group_child_registration registration_;
    F function_;
    std::tuple<Args...> args_;
};

template<typename F>
class task_group_scope_operation final {
public:
    task_group_scope_operation(task_group_child_registration registration,
                               F function, task_group& group)
        : registration_(std::move(registration))
        , function_(std::move(function))
        , group_(std::addressof(group)) {}

    task_group_scope_operation(task_group_scope_operation&&) = default;
    task_group_scope_operation& operator=(task_group_scope_operation&&) = delete;
    task_group_scope_operation(const task_group_scope_operation&) = delete;
    task_group_scope_operation& operator=(
        const task_group_scope_operation&) = delete;

    auto invoke() && -> std::invoke_result_t<F, task_group&> {
        return std::invoke(std::move(function_), *group_);
    }

private:
    // Declared first so it is destroyed after the user callable.
    task_group_child_registration registration_;
    F function_;
    task_group* group_;
};

struct task_scope_access;

} // namespace detail

/// Scheduler-bound owner for a set of child tasks.
///
/// Call join() before destroying the group. The destructor requests cancellation
/// as a non-joining fallback, but cannot synchronously join from a scheduler
/// worker without risking deadlock. task_scope() provides lexical cancel-and-join
/// behavior when a callback-shaped scope is convenient.
class task_group final {
public:
    explicit task_group(task_group_options options = {})
        : task_group(require_current_scheduler(), options) {}

    explicit task_group(runtime::scheduler& scheduler,
                        task_group_options options = {})
        : scheduler_(std::addressof(scheduler))
        , state_(std::make_shared<detail::task_group_state>(
              scheduler, options)) {
        state_->link_parent(parent_token_for(scheduler));
    }

    task_group(const task_group&) = delete;
    task_group& operator=(const task_group&) = delete;
    task_group(task_group&&) = delete;
    task_group& operator=(task_group&&) = delete;

    ~task_group() {
        bool joined = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            joined = join_completed_;
            accepting_ = false;
        }
        if (!joined && state_->outstanding_children() != 0) {
            state_->request_cancel_noexcept();
        }
    }

    template<typename F, typename... Args>
        requires (std::invocable<std::decay_t<F>, std::decay_t<Args>...> &&
                  elio::detail::is_task_v<std::invoke_result_t<
                      std::decay_t<F>, std::decay_t<Args>...>>)
    void spawn(F&& function, Args&&... args) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!accepting_) {
            throw std::logic_error(
                "task_group cannot spawn after join has started");
        }

        auto child = make_child_task_(
            std::forward<F>(function), std::forward<Args>(args)...);
        const bool scheduled = scheduler_->do_go_task_linked_(
            state_->token(), std::move(child));

        if (!scheduled) {
            state_->record_failure(std::make_exception_ptr(
                std::logic_error(
                    "scheduler rejected task_group child before execution")));
        }
    }

    /// Stop accepting children and wait for every child frame to leave the
    /// group. Fail-fast rethrows the first failure; collect-all throws
    /// task_group_error with every recorded failure. Only one join is allowed.
    [[nodiscard("co_await task_group::join()")]]
    task<void> join() {
        if (runtime::scheduler::current() != scheduler_) {
            throw std::logic_error(
                "task_group must be joined from its scheduler domain");
        }
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (scope_body_active_) {
                throw std::logic_error(
                    "task_scope body cannot join its own task_group");
            }
            if (join_started_) {
                throw std::logic_error("task_group can only be joined once");
            }
            join_started_ = true;
            accepting_ = false;
        }

        co_await state_->wait_all();

        {
            std::lock_guard<std::mutex> lock(mutex_);
            join_completed_ = true;
        }
        if (state_->options().failure_policy ==
            task_group_failure_policy::collect_all) {
            auto failures = state_->failures();
            if (!failures.empty()) {
                throw task_group_error(std::move(failures));
            }
            if (auto failure = state_->first_failure()) {
                std::rethrow_exception(std::move(failure));
            }
        } else if (auto failure = state_->first_failure()) {
            std::rethrow_exception(std::move(failure));
        }
    }

    /// Request cancellation on the group scheduler. Ordinary external threads
    /// wait for callback dispatch; another scheduler's worker posts the request
    /// asynchronously so scheduler domains cannot block each other.
    void request_cancel() {
        state_->request_cancel();
    }

    [[nodiscard]] bool is_cancellation_requested() const noexcept {
        return state_->is_cancellation_requested();
    }

    [[nodiscard]] size_t outstanding_children() const noexcept {
        return state_->outstanding_children();
    }

    [[nodiscard]] std::vector<std::exception_ptr> failures() const {
        return state_->failures();
    }

    [[nodiscard]] task_group_options options() const noexcept {
        return state_->options();
    }

    [[nodiscard]] runtime::scheduler& scheduler_domain() const noexcept {
        return *scheduler_;
    }

private:
    template<typename F, typename... Args>
    task<void> make_child_task_(F&& function, Args&&... args) {
        state_->register_child();
        detail::task_group_child_registration registration(state_);
        using operation_type = detail::task_group_child_operation<
            std::decay_t<F>, std::decay_t<Args>...>;
        return run_child_(operation_type(
            std::move(registration),
            std::decay_t<F>(std::forward<F>(function)),
            std::decay_t<Args>(std::forward<Args>(args))...));
    }

    template<typename F>
    coro::join_handle<void> spawn_scope_body_(F body) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!accepting_) {
            throw std::logic_error(
                "task_group cannot start a scope body after join has started");
        }

        scope_body_active_ = true;
        try {
            state_->register_child();
            detail::task_group_child_registration registration(state_);
            using operation_type = detail::task_group_scope_operation<F>;
            auto child = run_scope_body_(operation_type(
                std::move(registration), std::move(body), *this));
            return scheduler_->do_go_task_joinable_linked_(
                state_->token(), std::move(child));
        } catch (...) {
            scope_body_active_ = false;
            throw;
        }
    }

    void finish_scope_body_() noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        scope_body_active_ = false;
    }

    template<typename F, typename... Args>
    static task<void> run_child_(
        detail::task_group_child_operation<F, Args...> operation) {
        auto state = operation.state();
        bool owns_permit = false;

        try {
            auto token = this_coro::cancel_token();
            if (token.is_cancelled()) {
                co_return;
            }
            if (state->has_concurrency_limit()) {
                const auto permit_result =
                    co_await state->acquire_permit(std::move(token));
                if (permit_result == cancel_result::cancelled) {
                    co_return;
                }
                owns_permit = true;
            }

            using child_task = std::invoke_result_t<F, Args...>;
            using child_result = elio::detail::task_value_t<child_task>;
            if constexpr (std::is_void_v<child_result>) {
                co_await std::move(operation).invoke();
            } else {
                (void)co_await std::move(operation).invoke();
            }
        } catch (...) {
            state->record_failure(std::current_exception());
        }

        if (owns_permit) {
            state->release_permit();
        }
    }

    template<typename F>
    static task<void> run_scope_body_(
        detail::task_group_scope_operation<F> operation) {
        if (this_coro::cancel_token().is_cancelled()) {
            co_return;
        }

        using body_task = std::invoke_result_t<F, task_group&>;
        using body_result = elio::detail::task_value_t<body_task>;
        if constexpr (std::is_void_v<body_result>) {
            co_await std::move(operation).invoke();
        } else {
            (void)co_await std::move(operation).invoke();
        }
    }

    static runtime::scheduler& require_current_scheduler() {
        auto* scheduler = runtime::scheduler::current();
        if (!scheduler) {
            throw std::logic_error(
                "task_group requires a current scheduler or an explicit scheduler");
        }
        return *scheduler;
    }

    static cancel_token parent_token_for(
        runtime::scheduler& scheduler) noexcept {
        if (runtime::scheduler::current() != std::addressof(scheduler)) {
            return {};
        }
        return this_coro::cancel_token();
    }

    void request_cancel_noexcept() noexcept {
        state_->request_cancel_noexcept();
    }

    runtime::scheduler* scheduler_;
    std::shared_ptr<detail::task_group_state> state_;
    mutable std::mutex mutex_;
    bool accepting_ = true;
    bool join_started_ = false;
    bool join_completed_ = false;
    bool scope_body_active_ = false;

    friend struct detail::task_scope_access;
};

namespace detail {

struct task_scope_access final {
    static void request_cancel_noexcept(task_group& group) noexcept {
        group.request_cancel_noexcept();
    }

    template<typename F>
    static coro::join_handle<void> spawn_body(task_group& group, F body) {
        return group.spawn_scope_body_(std::move(body));
    }

    static void finish_body(task_group& group) noexcept {
        group.finish_scope_body_();
    }
};

class scheduler_domain_handoff final {
public:
    explicit scheduler_domain_handoff(runtime::scheduler& scheduler) noexcept
        : scheduler_(std::addressof(scheduler)) {}

    [[nodiscard]] bool await_ready() const noexcept {
        return runtime::scheduler::current() == scheduler_;
    }

    bool await_suspend(std::coroutine_handle<> handle) {
        if (!scheduler_->try_schedule(handle)) {
            throw std::logic_error(
                "task_scope scheduler rejected its domain handoff");
        }
        return true;
    }

    void await_resume() const noexcept {}

private:
    runtime::scheduler* scheduler_;
};

inline std::exception_ptr combine_scope_failures(
    std::exception_ptr body_failure,
    std::exception_ptr join_failure) {
    std::vector<std::exception_ptr> failures;
    failures.push_back(std::move(body_failure));

    try {
        std::rethrow_exception(join_failure);
    } catch (const task_group_error& error) {
        failures.insert(
            failures.end(), error.failures().begin(), error.failures().end());
    } catch (...) {
        failures.push_back(std::move(join_failure));
    }

    return std::make_exception_ptr(task_group_error(std::move(failures)));
}

template<typename F>
task<void> task_scope_wrapper(runtime::scheduler* scheduler,
                              task_group_options options,
                              F body) {
    if (scheduler && runtime::scheduler::current() != scheduler) {
        throw std::logic_error(
            "task_scope must run in its scheduler domain");
    }

    std::unique_ptr<task_group> group;
    if (scheduler) {
        group = std::make_unique<task_group>(*scheduler, options);
    } else {
        group = std::make_unique<task_group>(options);
    }

    std::exception_ptr body_failure;
    bool body_started = false;
    try {
        auto body_handle = task_scope_access::spawn_body(
            *group, std::move(body));
        body_started = true;
        co_await body_handle;
    } catch (...) {
        body_failure = std::current_exception();
        task_scope_access::request_cancel_noexcept(*group);
    }
    if (body_started) {
        task_scope_access::finish_body(*group);
    }

    // A body may complete on a foreign executor if one of its awaitables owns
    // that resumption policy. Scope cleanup and its caller continuation still
    // belong to the scheduler selected for the group.
    co_await scheduler_domain_handoff(group->scheduler_domain());

    try {
        co_await group->join();
    } catch (...) {
        auto join_failure = std::current_exception();
        if (!body_failure) {
            body_failure = std::move(join_failure);
        } else if (options.failure_policy ==
                   task_group_failure_policy::collect_all) {
            body_failure = combine_scope_failures(
                std::move(body_failure), std::move(join_failure));
        }
    }

    if (body_failure) {
        std::rethrow_exception(std::move(body_failure));
    }
}

} // namespace detail

template<typename F>
    requires (std::invocable<std::decay_t<F>, task_group&> &&
              elio::detail::is_task_v<std::invoke_result_t<
                  std::decay_t<F>, task_group&>>)
[[nodiscard("co_await task_scope()")]]
task<void> task_scope(F&& body, task_group_options options = {}) {
    return detail::task_scope_wrapper(
        nullptr, options, std::decay_t<F>(std::forward<F>(body)));
}

template<typename F>
    requires (std::invocable<std::decay_t<F>, task_group&> &&
              elio::detail::is_task_v<std::invoke_result_t<
                  std::decay_t<F>, task_group&>>)
[[nodiscard("co_await task_scope()")]]
task<void> task_scope(runtime::scheduler& scheduler, F&& body,
                      task_group_options options = {}) {
    return detail::task_scope_wrapper(
        std::addressof(scheduler), options,
        std::decay_t<F>(std::forward<F>(body)));
}

} // namespace elio::coro

namespace elio {
using coro::task_group;
using coro::task_group_error;
using coro::task_group_failure_policy;
using coro::task_group_options;
using coro::task_scope;
}
