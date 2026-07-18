#pragma once

#include <atomic>
#include <condition_variable>
#include <coroutine>
#include <cstddef>
#include <cstdint>
#include <exception>
#include <memory>
#include <mutex>
#include <new>
#include <stdexcept>
#include <thread>
#include <type_traits>
#include <utility>

namespace elio::runtime {
class scheduler;  // Forward declaration

// Defined in scheduler.hpp; cross-thread-safe handle dispatch that restores
// the virtual-stack frame context before resuming.
void schedule_handle(std::coroutine_handle<> handle) noexcept;
}

namespace elio::coro {

/// Result of a cancellable operation
enum class cancel_result {
    completed,   ///< Operation completed normally
    cancelled    ///< Operation was cancelled
};

namespace detail {

/// Type-erased callback node for the cancel_state intrusive list.
///
/// Each registration owns exactly one heap-allocated callback_node. The
/// callable lives in a small inline buffer when it fits (the typical case
/// for lambdas capturing 1-3 references); larger callables fall back to a
/// single secondary heap allocation. This eliminates the vector growth and
/// the per-callable std::function allocation of the previous design.
struct callback_node {
    enum class phase : uint8_t {
        registered,
        claimed,
        invoking,
        completed,
        unregistered
    };

    std::shared_ptr<callback_node> next;

    static constexpr std::size_t inline_buf_size = 48;
    alignas(alignof(std::max_align_t)) std::byte buf[inline_buf_size];
    void* heap_ptr = nullptr;

    void (*invoke_fn)(callback_node*) = nullptr;
    void (*destroy_fn)(callback_node*) = nullptr;

    callback_node() = default;
    callback_node(const callback_node&) = delete;
    callback_node& operator=(const callback_node&) = delete;
    callback_node(callback_node&&) = delete;
    callback_node& operator=(callback_node&&) = delete;

    template<typename F>
    void emplace(F&& f) {
        using T = std::decay_t<F>;
        if constexpr (sizeof(T) <= inline_buf_size &&
                      alignof(T) <= alignof(std::max_align_t) &&
                      std::is_nothrow_move_constructible_v<T>) {
            ::new (static_cast<void*>(buf)) T(std::forward<F>(f));
            invoke_fn = [](callback_node* n) {
                (*std::launder(reinterpret_cast<T*>(n->buf)))();
            };
            destroy_fn = [](callback_node* n) {
                std::launder(reinterpret_cast<T*>(n->buf))->~T();
            };
        } else {
            heap_ptr = new T(std::forward<F>(f));
            invoke_fn = [](callback_node* n) {
                (*static_cast<T*>(n->heap_ptr))();
            };
            destroy_fn = [](callback_node* n) {
                delete static_cast<T*>(n->heap_ptr);
                n->heap_ptr = nullptr;
            };
        }
    }

    void invoke_payload() {
        if (invoke_fn) invoke_fn(this);
    }

    void destroy_payload() noexcept {
        auto destroy = std::exchange(destroy_fn, nullptr);
        invoke_fn = nullptr;
        if (destroy) destroy(this);
    }

    void dispatch() {
        {
            std::lock_guard<std::mutex> lock(dispatch_mutex);
            if (dispatch_phase == phase::registered) {
                invoking_thread = std::this_thread::get_id();
            } else if (dispatch_phase != phase::claimed) {
                return;
            }
            dispatch_phase = phase::invoking;
        }

        std::exception_ptr exception;
        try {
            invoke_payload();
        } catch (...) {
            exception = std::current_exception();
        }
        destroy_payload();

        {
            std::lock_guard<std::mutex> lock(dispatch_mutex);
            invoking_thread = {};
            dispatch_phase = phase::completed;
        }
        dispatch_cv.notify_all();

        if (exception) {
            std::rethrow_exception(exception);
        }
    }

    void claim_for_dispatch(std::thread::id dispatch_thread) noexcept {
        std::lock_guard<std::mutex> lock(dispatch_mutex);
        if (dispatch_phase != phase::registered) return;
        invoking_thread = dispatch_thread;
        dispatch_phase = phase::claimed;
    }

    void unregister_and_wait() noexcept {
        std::unique_lock<std::mutex> lock(dispatch_mutex);
        if (dispatch_phase == phase::registered) {
            dispatch_phase = phase::unregistered;
            lock.unlock();
            destroy_payload();
            return;
        }

        if (dispatch_phase == phase::claimed &&
            invoking_thread == std::this_thread::get_id()) {
            // A callback may unregister a later callback selected by the same
            // synchronous cancel() dispatch. Waiting here would deadlock the
            // dispatcher, so suppress that not-yet-invoked callback.
            dispatch_phase = phase::unregistered;
            invoking_thread = {};
            lock.unlock();
            destroy_payload();
            dispatch_cv.notify_all();
            return;
        }

        if ((dispatch_phase == phase::claimed ||
             dispatch_phase == phase::invoking) &&
            invoking_thread != std::this_thread::get_id()) {
            dispatch_cv.wait(lock, [this] {
                return dispatch_phase != phase::claimed &&
                       dispatch_phase != phase::invoking;
            });
        }
    }

    ~callback_node() {
        destroy_payload();
    }

private:
    std::mutex dispatch_mutex;
    std::condition_variable dispatch_cv;
    phase dispatch_phase = phase::registered;
    std::thread::id invoking_thread;
};

/// Shared cancellation state (implementation detail).
///
/// Stores active callbacks as an intrusive shared-ownership list guarded by a
/// mutex. Each node also synchronizes callback dispatch with unregistration so
/// a registration can be destroyed concurrently without releasing callback
/// captures while they are still in use. Compared with the previous
/// std::vector<std::pair<id, std::function>> implementation:
///   * registration is O(1) without vector growth/copies;
///   * each registration owns a single heap-allocated node, and its
///     small-buffer-optimized payload typically avoids the second
///     std::function allocation;
///   * unregistration is O(N) but performs no heap shuffling.
struct cancel_state {
    std::atomic<bool> cancelled{false};
    std::mutex mutex;
    std::shared_ptr<callback_node> head;

    cancel_state() = default;
    cancel_state(const cancel_state&) = delete;
    cancel_state& operator=(const cancel_state&) = delete;

    template<typename F>
    std::shared_ptr<callback_node> add_callback(F&& cb) {
        // Allocate the node before taking the lock to keep the critical
        // section short.
        auto node = std::make_shared<callback_node>();
        node->emplace(std::forward<F>(cb));

        std::unique_lock<std::mutex> lock(mutex);
        if (cancelled.load(std::memory_order_relaxed)) {
            // Already cancelled: invoke and drop without inserting.
            lock.unlock();
            node->dispatch();
            return {};
        }
        node->next = head;
        head = node;
        return node;
    }

    void remove_callback(const std::shared_ptr<callback_node>& node) noexcept {
        if (!node) return;
        {
            std::lock_guard<std::mutex> lock(mutex);
            auto* link = &head;
            while (*link) {
                if (link->get() == node.get()) {
                    auto removed = std::move(*link);
                    *link = std::move(removed->next);
                    break;
                }
                link = &((*link)->next);
            }
        }
        node->unregister_and_wait();
    }

    void trigger() {
        std::shared_ptr<callback_node> list;
        {
            std::lock_guard<std::mutex> lock(mutex);
            if (cancelled.exchange(true, std::memory_order_release)) {
                return;  // Already cancelled
            }
            list = std::move(head);
            const auto dispatch_thread = std::this_thread::get_id();
            for (auto node = list; node; node = node->next) {
                node->claim_for_dispatch(dispatch_thread);
            }
        }
        // List is now owned by this call. Invoke each callback outside the
        // state lock. Concurrent remove_callback() calls synchronize through
        // the individual node and either suppress a not-yet-started callback
        // or wait for an in-progress callback to finish.
        std::exception_ptr first_exception;
        while (list) {
            auto node = std::move(list);
            list = std::move(node->next);
            try {
                node->dispatch();
            } catch (...) {
                if (!first_exception) {
                    first_exception = std::current_exception();
                }
            }
        }
        if (first_exception) {
            std::rethrow_exception(first_exception);
        }
    }

    ~cancel_state() {
        // Drop any callbacks that were never triggered or unregistered.
        while (head) {
            auto node = std::move(head);
            head = std::move(node->next);
            node->unregister_and_wait();
        }
    }
};

} // namespace detail

/// Forward declaration
class cancel_source;

/// Registration handle for cancel callbacks
class cancel_registration {
public:
    cancel_registration() = default;
    cancel_registration(cancel_registration&& other) noexcept
        : state_(std::move(other.state_)), node_(std::move(other.node_)) {}
    cancel_registration& operator=(cancel_registration&& other) noexcept {
        if (this != &other) {
            unregister();
            state_ = std::move(other.state_);
            node_ = std::move(other.node_);
        }
        return *this;
    }
    ~cancel_registration() { unregister(); }

    // Non-copyable
    cancel_registration(const cancel_registration&) = delete;
    cancel_registration& operator=(const cancel_registration&) = delete;

    /// Manually unregister the callback
    void unregister() noexcept {
        if (state_ && node_) {
            auto state = std::move(state_);
            auto node = std::move(node_);
            state->remove_callback(node);
        }
    }

private:
    friend class cancel_token;

    cancel_registration(std::shared_ptr<detail::cancel_state> state,
                        std::shared_ptr<detail::callback_node> node)
        : state_(std::move(state)), node_(std::move(node)) {}

    std::shared_ptr<detail::cancel_state> state_;
    std::shared_ptr<detail::callback_node> node_;
};

/// A token that can be used to check for and respond to cancellation requests.
///
/// cancel_token is a lightweight handle that can be copied and passed to
/// functions that should be cancellable. Multiple tokens can share the same
/// cancellation state via a cancel_source.
///
/// Example:
/// ```cpp
/// task<void> cancellable_work(cancel_token token) {
///     while (!token.is_cancelled()) {
///         // do work
///         auto result = co_await time::sleep_for(100ms, token);
///         if (result == cancel_result::cancelled) break;
///     }
/// }
/// ```
class cancel_token {
public:
    using registration = cancel_registration;

    /// Default constructor creates an empty (never-cancelled) token
    cancel_token() = default;

    /// Check if cancellation has been requested
    bool is_cancelled() const noexcept {
        return state_ && state_->cancelled.load(std::memory_order_acquire);
    }

    /// Implicit conversion to bool - returns true if NOT cancelled
    /// Allows: if (token) { /* not cancelled */ }
    explicit operator bool() const noexcept {
        return !is_cancelled();
    }

    /// Register a callback to be invoked when cancellation is requested.
    /// The callback will be invoked immediately if already cancelled.
    ///
    /// **Concurrency note:** If the token is already cancelled, the callback
    /// is invoked synchronously in the calling thread. This invocation may
    /// overlap with callbacks still being dispatched by a concurrent
    /// `cancel()` call on another thread. Callbacks must not assume mutual
    /// exclusion with other cancel callbacks; use external synchronization
    /// if shared mutable state is accessed.
    ///
    /// Destroying or unregistering the returned registration suppresses a
    /// callback that cancellation has not selected. Once selected or already
    /// running on a different thread, teardown waits for dispatch to finish. A
    /// callback may unregister itself, or a later callback selected by the same
    /// synchronous dispatch, without deadlocking.
    ///
    /// @param callback Function to call on cancellation
    /// @return Registration handle (callback unregisters when handle is destroyed)
    template<typename F>
    [[nodiscard]] registration on_cancel(F&& callback) const {
        if (!state_) {
            return registration{};
        }
        auto node = state_->add_callback(std::forward<F>(callback));
        if (!node) return registration{};
        return registration{state_, std::move(node)};
    }

    /// Register a coroutine handle to be resumed when cancelled.
    ///
    /// **Deprecated.** This API is unsafe when ``handle`` is concurrently
    /// suspended on an ``io::io_awaitable_base`` subclass (recv/send/accept/
    /// connect/timeout/poll/read/write/close/...). Each io awaitable owns an
    /// ``op_state`` whose phase atomic is shared with an in-flight SQE. If
    /// cancellation fires while the SQE is still pending, two paths race to
    /// resume the same coroutine:
    ///
    ///   1. ``on_cancel_resume`` schedules the handle on a worker. The worker
    ///      resumes the coroutine; the io awaitable's destructor attempts to
    ///      CAS the op_state phase from ``pending`` to ``orphaned``.
    ///   2. The kernel CQE arrives concurrently. ``dispatch_op_state`` CASes
    ///      ``pending`` → ``completed`` (winning before the destructor runs)
    ///      and schedules the same handle a second time, resuming an
    ///      already-destroyed frame.
    ///
    /// The correct pattern is to give the io awaitable its own ``cancel_token``
    /// at construction (see ``time::sleep_for(duration, token)`` /
    /// ``cancellable_sleep_awaitable``). That implementation registers
    /// ``on_cancel`` on the token and, from the cancel callback, schedules a
    /// fire-and-forget coroutine onto the awaiter's worker that submits an
    /// ``IORING_OP_ASYNC_CANCEL`` SQE via ``io_context::cancel``. The kernel
    /// then delivers exactly one terminal CQE, and the existing op_state
    /// CAS resolves the cancel-vs-completion race correctly.
    ///
    /// For non-io use cases, prefer ``on_cancel(callable)`` and have the
    /// coroutine poll an ``event`` / atomic flag, or wake via ``channel`` /
    /// ``condition_variable``.
    ///
    /// The implementation is retained (and routes through
    /// ``runtime::schedule_handle`` so that virtual-stack frame context is
    /// restored before resume) for callers who can prove their handle is
    /// **not** suspended on an io awaitable. Otherwise this is a footgun.
    /// @param handle Coroutine to resume on cancellation
    /// @return Registration handle
    [[deprecated(
        "Unsafe with handles suspended on io_awaitables (double-resume race "
        "with op_state CAS). Pass a cancel_token to the awaitable instead "
        "(see time::sleep_for(d, token) / cancellable_sleep_awaitable), or "
        "use on_cancel(callable) with an event/atomic flag.")]]
    [[nodiscard]] registration on_cancel_resume(std::coroutine_handle<> handle) const {
        return on_cancel([handle]() {
            if (handle && !handle.done()) {
                runtime::schedule_handle(handle);
            }
        });
    }

private:
    friend class cancel_source;

    explicit cancel_token(std::shared_ptr<detail::cancel_state> state)
        : state_(std::move(state)) {}

    std::shared_ptr<detail::cancel_state> state_;
};

/// A source of cancellation that can create tokens and trigger cancellation.
///
/// cancel_source owns the cancellation state and can create multiple tokens
/// that share the same state. When cancel() is called, all associated tokens
/// become cancelled and their registered callbacks are invoked.
///
/// Example:
/// ```cpp
/// cancel_source source;
/// auto token = source.get_token();
///
/// // Pass token to cancellable operations
/// auto task = do_work(token);
///
/// // Later, cancel the operation
/// source.cancel();
/// ```
class cancel_source {
public:
    /// Create a new cancel source
    cancel_source()
        : state_(std::make_shared<detail::cancel_state>()) {}

    /// Get a token associated with this source
    cancel_token get_token() const noexcept {
        return cancel_token{state_};
    }

    /// Request cancellation
    /// All registered callbacks will be invoked and all tokens will report
    /// is_cancelled() == true. Callbacks run synchronously on this thread; all
    /// are dispatched, then the first callback exception is rethrown.
    void cancel() {
        if (state_) {
            state_->trigger();
        }
    }

    /// Check if cancellation has been requested
    bool is_cancelled() const noexcept {
        return state_ && state_->cancelled.load(std::memory_order_acquire);
    }

private:
    std::shared_ptr<detail::cancel_state> state_;
};

namespace detail {

/// Task-lifetime cancellation authority. The source remains valid through the
/// shared task_execution_context even after the coroutine frame is destroyed.
/// A lazy child links to its active awaiter's token before first resume, so a
/// request flows down the running task chain without granting cancellation
/// authority to the lazy task owner itself.
class cancellation_context final {
public:
    cancellation_context() = default;

    cancellation_context(const cancellation_context&) = delete;
    cancellation_context& operator=(const cancellation_context&) = delete;
    cancellation_context(cancellation_context&&) = delete;
    cancellation_context& operator=(cancellation_context&&) = delete;

    [[nodiscard]] cancel_token token() const noexcept {
        return source_.get_token();
    }

    void request_cancel() {
        source_.cancel();
    }

    [[nodiscard]] bool is_cancellation_requested() const noexcept {
        return source_.is_cancelled();
    }

    void link_parent(cancel_token parent) {
        auto registration = parent.on_cancel([source = source_]() mutable {
            source.cancel();
        });

        std::lock_guard<std::mutex> lock(parent_mutex_);
        if (parent_linked_) {
            throw std::logic_error(
                "task cancellation context already has a parent");
        }
        parent_registration_ = std::move(registration);
        parent_linked_ = true;
    }

private:
    // Keep registration last so it unregisters before the owned source is
    // released during destruction.
    cancel_source source_;
    std::mutex parent_mutex_;
    bool parent_linked_ = false;
    cancel_registration parent_registration_;
};

} // namespace detail

} // namespace elio::coro
