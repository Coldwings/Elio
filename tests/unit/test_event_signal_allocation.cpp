#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <elio/sync/event.hpp>
#include "../test_main.cpp"

#include <array>
#include <atomic>
#include <chrono>
#include <exception>
#include <new>
#include <optional>
#include <thread>

namespace {
using elio::coro::task;
thread_local int dispatch_calls = 0;
thread_local int fail_dispatch_call = 0;

void fail_dispatch_storage() {
    if (dispatch_calls++ == fail_dispatch_call) throw std::bad_alloc();
}

struct dispatch_hook_guard {
    void (*previous)();
    explicit dispatch_hook_guard(int fail_call)
        : previous(elio::sync::detail::event_dispatch_storage_for_test.exchange(fail_dispatch_storage)) {
        dispatch_calls = 0;
        fail_dispatch_call = fail_call;
    }
    void release() const noexcept {
        elio::sync::detail::event_dispatch_storage_for_test.store(previous);
    }
    ~dispatch_hook_guard() { release(); }
};

// Inspect only after event's queue mutex confirms all waiters are published.
// Retained wake ownership permits normal scheduling recovery in the old code;
// no coroutine frame is force-destroyed by a failed baseline assertion.
struct probe_waiter : elio::sync::event::event_waiter {
    elio::coro::cancel_token::registration cancellation;
    probe_waiter(elio::sync::event& signal, bool cancellable, elio::coro::cancel_token token)
        : event_waiter(signal, cancellable) {
        if (cancellable) cancellation = token.on_cancel(
            [wake = cancellation_wake_state()] { wake->request_cancel(); });
    }
    bool await_ready() const noexcept { return await_ready_impl(); }
    bool await_suspend(std::coroutine_handle<> handle) { return await_suspend_impl(handle); }
    elio::coro::cancel_result await_resume() noexcept {
        cancellation.unregister();
        return await_resume_impl();
    }
    auto snapshot() const { return cancellation_wake_state(); }
};

struct waiter_probe {
    probe_waiter* suspended = nullptr;
    elio::coro::cancel_result result = elio::coro::cancel_result::completed;
};

task<void> wait_signal(elio::sync::event& signal, bool cancellable,
        elio::coro::cancel_token token, waiter_probe& observed) {
    probe_waiter waiter(signal, cancellable, std::move(token));
    observed.suspended = &waiter;
    observed.result = co_await waiter;
}

template<typename Predicate>
bool observe(Predicate predicate) {
    const auto deadline = std::chrono::steady_clock::now() + elio::test::scaled_ms(2000);
    while (!predicate()) {
        if (std::chrono::steady_clock::now() >= deadline) return false;
        std::this_thread::yield();
    }
    return true;
}
} // namespace

TEST_CASE("Event dispatch allocation failure preserves pending wakes",
          "[sync][event][allocation][issue-1291]") {
    const auto cancellable = GENERATE(false, true);
    const auto workers = GENERATE(size_t{1}, size_t{2});
    const auto fail_call = GENERATE(0, 1, 2);
    CAPTURE(cancellable, workers, fail_call);
    elio::sync::event signal;
    elio::coro::cancel_source stop;
    std::array<waiter_probe, 3> probes;
    std::array<std::optional<elio::coro::join_handle<void>>, 3> waits;
    std::array<elio::sync::detail::wake_state_ptr, 3> retained;
    elio::runtime::scheduler scheduler(workers);
    scheduler.start();
    std::exception_ptr launch_failure;
    try {
        for (size_t i = 0; i < waits.size(); ++i)
            waits[i].emplace(scheduler.go_joinable(wait_signal(
                signal, cancellable, stop.get_token(), probes[i])));
    } catch (...) {
        launch_failure = std::current_exception();
    }
    if (launch_failure) {
        // A later admission/allocation failure must not abandon an earlier
        // root on this event. Release both already-published and not-yet-run
        // roots, normally join every retained handle, then preserve the first
        // launch exception. No dispatch fault hook is armed on this path.
        if (cancellable) stop.cancel();
        else signal.release_waiters_for_test();
        for (auto& wait : waits) if (wait) wait->wait_destroyed();
        const bool launch_cleanup_drained = scheduler.shutdown(elio::test::scaled_ms(5000));
        for (auto& wait : waits) {
            if (!wait) continue;
            try { wait->await_resume(); } catch (...) {}
        }
        // Preserve the first launch failure even if bounded scheduler drain
        // reports false; every retained frame has already been destroyed.
        if (!launch_cleanup_drained) std::rethrow_exception(launch_failure);
        std::rethrow_exception(launch_failure);
    }
    const bool published = observe([&] { return signal.waiter_count_for_test() == waits.size(); });
    if (published) {
        for (size_t i = 0; i < probes.size(); ++i) retained[i] = probes[i].suspended->snapshot();
    }
    bool allocation_failed = false;
    dispatch_hook_guard hook(fail_call);
    if (!published) hook.release();
    try { signal.set(); }
    catch (const std::bad_alloc&) { allocation_failed = true; }
    hook.release();
    const bool signaled_after_attempt = signal.is_set();
    const auto queued_after_attempt = signal.waiter_count_for_test();
    bool cancellation_completed = true;
    if (allocation_failed && cancellable) {
        stop.cancel();
        cancellation_completed = observe([&] {
            for (const auto& wait : waits) if (!wait->is_ready()) return false;
            return true;
        });
    }
    // Retry removes all remaining nodes before rescuing selected old-code
    // wakes. schedule_selected() claims each wake once, rejecting duplicates.
    std::exception_ptr retry_failure;
    try {
        signal.set();
    } catch (...) {
        retry_failure = std::current_exception();
        signal.release_waiters_for_test();
    }
    size_t rescued = 0;
    for (const auto& wake : retained) if (wake && wake->schedule_selected()) ++rescued;
    for (auto& wait : waits) wait->wait_destroyed();
    const bool drained = scheduler.shutdown(elio::test::scaled_ms(5000));
    if (retry_failure) {
        for (auto& wait : waits) {
            try { wait->await_resume(); } catch (...) {}
        }
        std::rethrow_exception(retry_failure);
    }
    for (auto& wait : waits) wait->await_resume();
    REQUIRE(drained);
    CHECK(published);
    CHECK(allocation_failed == (published && fail_call == 0));
    if (allocation_failed) {
        CHECK_FALSE(signaled_after_attempt);
        CHECK(queued_after_attempt == waits.size());
        CHECK(cancellation_completed);
    }
    CHECK(rescued == 0);
    for (const auto& probe : probes) {
        CHECK(probe.result == (allocation_failed && cancellable
            ? elio::coro::cancel_result::cancelled : elio::coro::cancel_result::completed));
    }
}
