#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <elio/coro/join_wait.hpp>
#include <elio/net/detail/resolve_job.hpp>
#include <elio/sync/event.hpp>
#include "../test_main.cpp"

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <stdexcept>
#include <string>
#include <thread>
#include <type_traits>
#include <vector>

namespace {

using elio::coro::cancel_source;
using elio::coro::cancel_token;
using elio::coro::join_wait_outcome;
using elio::coro::task;
using elio::net::detail::dns_admission_lease;
using elio::net::detail::dns_admission_state;
using elio::net::detail::dns_job_state;
using elio::net::detail::dns_lookup_result;
using elio::net::detail::try_make_owned_dns_job;
using elio::runtime::scheduler;

bool wait_for(const std::function<bool()>& predicate) {
    const auto deadline = std::chrono::steady_clock::now() + elio::test::scaled_ms(5000);
    while (!predicate()) {
        if (std::chrono::steady_clock::now() >= deadline) return false;
        std::this_thread::yield();
    }
    return true;
}

void release_flag(std::atomic<bool>& flag) noexcept {
    flag.store(true, std::memory_order_release);
    flag.notify_all();
}

void hold_until_released(std::atomic<bool>& flag) noexcept {
    while (!flag.load(std::memory_order_acquire)) {
        flag.wait(false, std::memory_order_acquire);
    }
}

struct thread_drain_guard {
    std::vector<std::thread>& threads;
    std::atomic<bool>& start;
    std::atomic<bool>& release;

    void drain() noexcept {
        release_flag(start);
        release_flag(release);
        for (auto& thread : threads) {
            if (thread.joinable()) thread.join();
        }
    }
    ~thread_drain_guard() { drain(); }
};

struct lookup_control {
    std::atomic<bool> entered{false};
    std::atomic<bool> release{false};
    std::atomic<size_t> calls{0};
    std::atomic<bool> timer_entered{false};
    elio::sync::event expire;
    std::string observed_host;
    uint16_t observed_port = 0;
    bool throw_failure = false;
};

std::atomic<lookup_control*> current_lookup_control{nullptr};

dns_lookup_result controlled_lookup(std::string_view host, uint16_t port) {
    auto* control = current_lookup_control.load(std::memory_order_acquire);
    if (!control) throw std::logic_error("missing controlled lookup");
    control->calls.fetch_add(1, std::memory_order_relaxed);
    control->observed_host = host;
    control->observed_port = port;
    release_flag(control->entered);
    hold_until_released(control->release);
    if (control->throw_failure) throw std::runtime_error("controlled DNS failure");
    dns_lookup_result result;
    result.addresses.emplace_back(elio::net::ipv4_address("127.0.0.1", port));
    return result;
}

task<elio::coro::cancel_result> controlled_deadline(
        std::chrono::steady_clock::time_point, cancel_token token) {
    auto* control = current_lookup_control.load(std::memory_order_acquire);
    if (!control) throw std::logic_error("missing controlled deadline");
    release_flag(control->timer_entered);
    co_return co_await control->expire.wait(std::move(token));
}

class lookup_guard final {
public:
    lookup_guard(lookup_control& control, std::function<void()> drain,
                 bool fake_deadline = false)
        : control_(control), drain_(std::move(drain)), fake_deadline_(fake_deadline) {
        current_lookup_control.store(&control_, std::memory_order_release);
        elio::net::detail::owned_dns_lookup_for_test.store(
            controlled_lookup, std::memory_order_release);
        if (fake_deadline_) {
            elio::coro::detail::join_timer_wait_for_test.store(
                controlled_deadline, std::memory_order_release);
        }
    }

    ~lookup_guard() {
        release_flag(control_.release);
        drain_();
        if (fake_deadline_) {
            elio::coro::detail::join_timer_wait_for_test.store(nullptr, std::memory_order_release);
        }
        elio::net::detail::owned_dns_lookup_for_test.store(nullptr, std::memory_order_release);
        current_lookup_control.store(nullptr, std::memory_order_release);
    }

private:
    lookup_control& control_;
    std::function<void()> drain_;
    bool fake_deadline_;
};

struct dns_departure_guard {
    std::shared_ptr<dns_job_state> state;
    ~dns_departure_guard() { state->depart(); }
};

task<join_wait_outcome> observe_job(std::shared_ptr<dns_job_state> state, cancel_token token,
        std::optional<std::chrono::steady_clock::time_point> deadline = std::nullopt) {
    dns_departure_guard departure{state};
    co_return co_await elio::coro::detail::observe_join_result(
        state->result_state(), deadline, std::move(token));
}

} // namespace

TEST_CASE("DNS admission leases bound all simultaneous reservations",
          "[dns][resolve_wait][admission][contract]") {
    STATIC_REQUIRE(!std::is_copy_constructible_v<dns_admission_lease>);
    STATIC_REQUIRE(std::is_nothrow_move_constructible_v<dns_admission_lease>);
    auto capacity = std::make_shared<dns_admission_state>(2);
    std::atomic<bool> start{false};
    std::atomic<bool> release{false};
    std::atomic<size_t> attempted{0};
    std::atomic<size_t> accepted{0};
    std::vector<std::thread> threads;
    thread_drain_guard guard{threads, start, release};
    for (size_t index = 0; index < 8; ++index) {
        threads.emplace_back([capacity, &start, &release, &attempted, &accepted] {
            hold_until_released(start);
            auto lease = dns_admission_lease::try_acquire(capacity);
            if (lease) accepted.fetch_add(1, std::memory_order_relaxed);
            attempted.fetch_add(1, std::memory_order_release);
            if (lease) hold_until_released(release);
        });
    }
    release_flag(start);
    REQUIRE(wait_for([&] { return attempted.load(std::memory_order_acquire) == 8; }));
    REQUIRE(accepted.load() == 2);
    REQUIRE(capacity->outstanding() == 2);
    guard.drain();
    REQUIRE(capacity->outstanding() == 0);
    auto first = dns_admission_lease::try_acquire(capacity);
    auto second = dns_admission_lease::try_acquire(capacity);
    REQUIRE(first);
    REQUIRE(second);
    REQUIRE_FALSE(dns_admission_lease::try_acquire(capacity));
}

TEST_CASE("DNS admission state survives its configuration owner",
          "[dns][resolve_wait][admission][lifetime]") {
    auto capacity = std::make_shared<dns_admission_state>(1);
    std::weak_ptr<dns_admission_state> weak = capacity;
    {
        auto lease = dns_admission_lease::try_acquire(capacity);
        REQUIRE(lease);
        auto moved = std::move(lease);
        REQUIRE(moved);
        REQUIRE_FALSE(lease);
        capacity.reset();
        REQUIRE_FALSE(weak.expired());
        REQUIRE(weak.lock()->outstanding() == 1);
    }
    REQUIRE(weak.expired());
    REQUIRE_FALSE(try_make_owned_dns_job("unused", 80,
        std::make_shared<dns_admission_state>(0)));
}

TEST_CASE("DNS lookup admission and queued departure have one phase winner",
          "[dns][resolve_wait][queued][race][regression]") {
    SECTION("lookup wins before departure") {
        dns_job_state state;
        REQUIRE(state.begin_lookup());
        state.depart();
        REQUIRE(state.phase_for_test() == dns_job_state::phase::running);
        REQUIRE_FALSE(state.begin_lookup());
    }
    SECTION("departure wins before lookup") {
        dns_job_state state;
        state.depart();
        REQUIRE_FALSE(state.begin_lookup());
        REQUIRE(state.phase_for_test() == dns_job_state::phase::discarded);
    }
    SECTION("simultaneous contenders preserve either legal winner") {
        for (size_t attempt = 0; attempt < 64; ++attempt) {
            dns_job_state state;
            std::atomic<bool> start{false};
            std::atomic<bool> release{false};
            bool admitted = false;
            std::vector<std::thread> threads;
            thread_drain_guard guard{threads, start, release};
            threads.emplace_back([&] {
                hold_until_released(start);
                admitted = state.begin_lookup();
            });
            threads.emplace_back([&] {
                hold_until_released(start);
                state.depart();
            });
            guard.drain();
            REQUIRE(state.phase_for_test() == (admitted
                ? dns_job_state::phase::running : dns_job_state::phase::discarded));
            REQUIRE_FALSE(state.begin_lookup());
            state.depart();
            REQUIRE(state.phase_for_test() == (admitted
                ? dns_job_state::phase::running : dns_job_state::phase::discarded));
            state.retire();
            state.depart();
            REQUIRE_FALSE(state.begin_lookup());
            REQUIRE(state.phase_for_test() == dns_job_state::phase::retired);
        }
    }
}

TEST_CASE("rejected DNS submission releases all caller-owned work and capacity",
          "[dns][resolve_wait][admission][overload][lifetime][regression]") {
    enum class rejection { no_workers, stopped, full_queue };
    const auto reason = GENERATE(rejection::no_workers, rejection::stopped,
                                rejection::full_queue);
    elio::runtime::blocking_pool pool(reason == rejection::no_workers ? 0 : 1);
    lookup_control lookup;
    std::atomic<bool> blocker_entered{false};
    std::atomic<bool> blocker_release{false};
    lookup_guard guard(lookup, [&] {
        release_flag(blocker_release);
        pool.shutdown();
    });
    if (reason == rejection::stopped) {
        pool.shutdown();
    } else if (reason == rejection::full_queue) {
        std::function<void()> blocker = [&] {
            release_flag(blocker_entered);
            hold_until_released(blocker_release);
        };
        REQUIRE(pool.submit_bounded(std::move(blocker), 1));
        REQUIRE(wait_for([&] { return blocker_entered.load(std::memory_order_acquire); }));
        std::function<void()> filler = [] {};
        REQUIRE(pool.submit_bounded(std::move(filler), 1));
        filler = nullptr;
        REQUIRE(pool.queued_count_for_test() == 1);
    }

    auto capacity = std::make_shared<dns_admission_state>(1);
    auto job = try_make_owned_dns_job("rejected.example", 80, capacity);
    REQUIRE(job);
    auto state = job->state();
    std::weak_ptr<elio::net::detail::owned_dns_job> weak_job = job;
    std::weak_ptr<dns_job_state> weak_state = state;
    std::weak_ptr<elio::coro::detail::join_state<dns_lookup_result>> weak_result =
        state->result_state();
    std::function<void()> work = [job = std::move(job)] { job->run(); };
    REQUIRE_FALSE(pool.submit_bounded(std::move(work), 1));
    REQUIRE(work);
    REQUIRE_FALSE(weak_job.expired());
    REQUIRE(capacity->outstanding() == 1);
    REQUIRE(state->phase_for_test() == dns_job_state::phase::queued);
    REQUIRE_FALSE(state->result_state()->is_completed());
    state->depart();
    work = nullptr;
    REQUIRE(weak_job.expired());
    REQUIRE(capacity->outstanding() == 0);
    state.reset();
    REQUIRE(weak_state.expired());
    REQUIRE(weak_result.expired());
    REQUIRE(lookup.calls.load() == 0);
    {
        auto replacement = try_make_owned_dns_job("replacement.example", 80, capacity);
        REQUIRE(replacement);
        REQUIRE(capacity->outstanding() == 1);
    }
    REQUIRE(capacity->outstanding() == 0);
}

TEST_CASE("repeated queued DNS departure cannot evade outstanding admission limits",
          "[dns][resolve_wait][queued][admission][stress][regression]") {
    elio::runtime::blocking_pool pool(1);
    lookup_control lookup;
    std::atomic<bool> blocker_entered{false};
    std::atomic<bool> blocker_release{false};
    lookup_guard guard(lookup, [&] {
        release_flag(blocker_release);
        pool.shutdown();
    });
    release_flag(lookup.release);
    auto capacity = std::make_shared<dns_admission_state>(3);
    for (size_t round = 0; round < 4; ++round) {
        blocker_entered.store(false, std::memory_order_release);
        blocker_release.store(false, std::memory_order_release);
        std::function<void()> blocker = [&] {
            release_flag(blocker_entered);
            hold_until_released(blocker_release);
        };
        REQUIRE(pool.submit_bounded(std::move(blocker), 3));
        REQUIRE(wait_for([&] { return blocker_entered.load(std::memory_order_acquire); }));
        std::vector<std::weak_ptr<elio::net::detail::owned_dns_job>> weak_jobs;
        std::vector<std::weak_ptr<dns_job_state>> weak_states;
        std::vector<std::weak_ptr<elio::coro::detail::join_state<dns_lookup_result>>> weak_results;
        for (size_t index = 0; index < 3; ++index) {
            auto job = try_make_owned_dns_job("departed.queued.example", 80, capacity);
            REQUIRE(job);
            auto state = job->state();
            weak_jobs.push_back(job);
            weak_states.push_back(state);
            weak_results.push_back(state->result_state());
            std::function<void()> work = [job = std::move(job)] { job->run(); };
            REQUIRE(pool.submit_bounded(std::move(work), 3));
            work = nullptr;
            state->depart();
            REQUIRE(state->phase_for_test() == dns_job_state::phase::discarded);
        }
        for (size_t attempt = 0; attempt < 64; ++attempt) {
            REQUIRE_FALSE(try_make_owned_dns_job("overloaded.example", 80, capacity));
            REQUIRE(capacity->outstanding() == 3);
            REQUIRE(pool.queued_count_for_test() == 3);
        }
        REQUIRE(lookup.calls.load() == 0);
        release_flag(blocker_release);
        REQUIRE(wait_for([&] { return capacity->outstanding() == 0; }));
        REQUIRE(pool.queued_count_for_test() == 0);
        REQUIRE(lookup.calls.load() == 0);
        for (size_t index = 0; index < weak_jobs.size(); ++index) {
            REQUIRE(weak_jobs[index].expired());
            REQUIRE(weak_states[index].expired());
            REQUIRE(weak_results[index].expired());
        }
    }
}

TEST_CASE("queued DNS departure retains capacity and skips libc work",
          "[dns][resolve_wait][queued][cancellation][regression]") {
    elio::runtime::blocking_pool pool(1);
    lookup_control lookup;
    std::atomic<bool> blocker_entered{false};
    std::atomic<bool> blocker_release{false};
    lookup_guard guard(lookup, [&] {
        release_flag(blocker_release);
        pool.shutdown();
    });
    std::function<void()> blocker = [&] {
        release_flag(blocker_entered);
        hold_until_released(blocker_release);
    };
    REQUIRE(pool.submit_bounded(std::move(blocker), 1));
    REQUIRE(wait_for([&] { return blocker_entered.load(std::memory_order_acquire); }));

    auto capacity = std::make_shared<dns_admission_state>(1);
    auto job = try_make_owned_dns_job("owned.queued.example", 80, capacity);
    REQUIRE(job);
    auto state = job->state();
    std::weak_ptr<elio::net::detail::owned_dns_job> weak_job = job;
    std::function<void()> work = [job = std::move(job)] { job->run(); };
    REQUIRE(pool.submit_bounded(std::move(work), 1));
    work = nullptr;
    REQUIRE(pool.queued_count_for_test() == 1);
    state->depart();
    REQUIRE(state->phase_for_test() == dns_job_state::phase::discarded);
    REQUIRE(capacity->outstanding() == 1);
    REQUIRE_FALSE(try_make_owned_dns_job("overloaded.example", 80, capacity));

    release_flag(lookup.release);
    release_flag(blocker_release);
    pool.shutdown();
    REQUIRE(lookup.calls.load() == 0);
    REQUIRE(state->phase_for_test() == dns_job_state::phase::retired);
    REQUIRE_FALSE(state->result_state()->is_completed());
    REQUIRE(weak_job.expired());
    REQUIRE(capacity->outstanding() == 0);
}

TEST_CASE("running DNS outlives a departed observer without retaining its input frame",
          "[dns][resolve_wait][running][cancellation][lifetime][regression]") {
    const bool throw_failure = GENERATE(false, true);
    const bool use_deadline = GENERATE(false, true);
    scheduler sched(2);
    lookup_control lookup;
    lookup.throw_failure = throw_failure;
    lookup_guard guard(lookup, [&] { sched.shutdown(); }, use_deadline);
    sched.start();
    auto capacity = std::make_shared<dns_admission_state>(1);
    auto job = [&] {
        std::string host(256, 'h');
        return try_make_owned_dns_job(host, 8080, capacity);
    }();
    REQUIRE(job);
    auto state = job->state();
    std::weak_ptr<dns_job_state> weak_state = state;
    std::weak_ptr<elio::coro::detail::join_state<dns_lookup_result>> weak_result =
        state->result_state();
    std::weak_ptr<elio::net::detail::owned_dns_job> weak_job = job;
    std::function<void()> work = [job = std::move(job)] { job->run(); };
    REQUIRE(sched.get_blocking_pool()->submit_bounded(std::move(work), 1));
    work = nullptr;
    REQUIRE(wait_for([&] { return lookup.entered.load(std::memory_order_acquire); }));
    REQUIRE(lookup.observed_host == std::string(256, 'h'));
    REQUIRE(lookup.observed_port == 8080);
    REQUIRE(state->phase_for_test() == dns_job_state::phase::running);

    cancel_source source;
    const auto previous_installs =
        elio::coro::detail::join_observer_installed_for_test.load(std::memory_order_acquire);
    const auto deadline = use_deadline
        ? std::optional(std::chrono::steady_clock::now() + std::chrono::hours(1))
        : std::nullopt;
    auto observer = sched.go_joinable([state, token = source.get_token(), deadline]() -> task<join_wait_outcome> {
        co_return co_await observe_job(state, token, deadline);
    });
    REQUIRE(wait_for([&] {
        return elio::coro::detail::join_observer_installed_for_test.load(
            std::memory_order_acquire) > previous_installs;
    }));
    if (use_deadline) {
        REQUIRE(wait_for([&] { return lookup.timer_entered.load(std::memory_order_acquire); }));
        lookup.expire.set();
    } else {
        source.cancel();
    }
    REQUIRE(wait_for([&] { return observer.await_ready(); }));
    REQUIRE(observer.await_resume() == (use_deadline
        ? join_wait_outcome::timed_out : join_wait_outcome::cancelled));
    observer.wait_destroyed();
    REQUIRE_FALSE(lookup.release.load(std::memory_order_acquire));
    REQUIRE(capacity->outstanding() == 1);
    REQUIRE_FALSE(try_make_owned_dns_job("overloaded.example", 80, capacity));
    state.reset();
    REQUIRE_FALSE(weak_state.expired());
    REQUIRE_FALSE(weak_result.expired());

    release_flag(lookup.release);
    sched.shutdown();
    REQUIRE(lookup.calls.load() == 1);
    REQUIRE(weak_job.expired());
    REQUIRE(weak_state.expired());
    REQUIRE(weak_result.expired());
    REQUIRE(capacity->outstanding() == 0);
}

TEST_CASE("owned DNS completion publishes a value or exception only once",
          "[dns][resolve_wait][completion][exception][contract]") {
    const bool throw_failure = GENERATE(false, true);
    scheduler sched(1);
    lookup_control lookup;
    lookup.throw_failure = throw_failure;
    lookup_guard guard(lookup, [&] { sched.shutdown(); });
    sched.start();
    auto capacity = std::make_shared<dns_admission_state>(1);
    auto job = try_make_owned_dns_job("owned.completed.example", 443, capacity);
    REQUIRE(job);
    auto state = job->state();
    std::function<void()> work = [job = std::move(job)] { job->run(); };
    REQUIRE(sched.get_blocking_pool()->submit_bounded(std::move(work), 1));
    work = nullptr;
    REQUIRE(wait_for([&] { return lookup.entered.load(std::memory_order_acquire); }));
    release_flag(lookup.release);
    REQUIRE(wait_for([&] { return state->result_state()->is_completed(); }));

    cancel_source source;
    source.cancel();
    auto observer = sched.go_joinable([state, token = source.get_token()]() -> task<join_wait_outcome> {
        co_return co_await observe_job(state, token);
    });
    REQUIRE(wait_for([&] { return observer.await_ready(); }));
    REQUIRE(observer.await_resume() == join_wait_outcome::completed);
    observer.wait_destroyed();
    sched.shutdown();
    REQUIRE(lookup.calls.load() == 1);
    REQUIRE(capacity->outstanding() == 0);
    if (throw_failure) {
        REQUIRE_THROWS_WITH(state->result_state()->get_value(), "controlled DNS failure");
    } else {
        auto result = state->result_state()->get_value();
        REQUIRE(result.error == 0);
        REQUIRE(result.addresses.size() == 1);
        REQUIRE(result.addresses.front().port() == 443);
    }
}

TEST_CASE("scheduler shutdown drains a still-blocked departed DNS producer",
          "[dns][resolve_wait][running][shutdown][lifetime][regression]") {
    const bool throw_failure = GENERATE(false, true);
    scheduler sched(2);
    lookup_control lookup;
    lookup.throw_failure = throw_failure;
    std::vector<std::thread> shutdown_threads;
    std::atomic<bool> shutdown_returned{false};
    bool drained = false;
    lookup_guard guard(lookup, [&] {
        for (auto& thread : shutdown_threads) {
            if (thread.joinable()) thread.join();
        }
        sched.shutdown();
    });
    sched.start();
    auto capacity = std::make_shared<dns_admission_state>(1);
    auto job = try_make_owned_dns_job("departed.shutdown.example", 80, capacity);
    REQUIRE(job);
    auto state = job->state();
    std::weak_ptr<elio::net::detail::owned_dns_job> weak_job = job;
    std::weak_ptr<dns_job_state> weak_state = state;
    std::weak_ptr<elio::coro::detail::join_state<dns_lookup_result>> weak_result =
        state->result_state();
    auto* pool = sched.get_blocking_pool();
    REQUIRE(pool);
    std::function<void()> work = [job = std::move(job)] { job->run(); };
    REQUIRE(pool->submit_bounded(std::move(work), 1));
    work = nullptr;
    REQUIRE(wait_for([&] { return lookup.entered.load(std::memory_order_acquire); }));

    cancel_source source;
    auto observer = sched.go_joinable([state, token = source.get_token()]() -> task<join_wait_outcome> {
        co_return co_await observe_job(state, token);
    });
    source.cancel();
    REQUIRE(wait_for([&] { return observer.await_ready(); }));
    REQUIRE(observer.await_resume() == join_wait_outcome::cancelled);
    observer.wait_destroyed();
    state.reset();
    REQUIRE(sched.active_tasks() == 0);

    shutdown_threads.emplace_back([&] {
        drained = sched.shutdown();
        release_flag(shutdown_returned);
    });
    REQUIRE(wait_for([&] { return pool->stopped_for_test(); }));
    REQUIRE_FALSE(shutdown_returned.load(std::memory_order_acquire));
    REQUIRE_FALSE(lookup.release.load(std::memory_order_acquire));
    REQUIRE(sched.is_running());
    REQUIRE(capacity->outstanding() == 1);
    REQUIRE_FALSE(try_make_owned_dns_job("overloaded.example", 80, capacity));
    REQUIRE_FALSE(weak_job.expired());
    REQUIRE_FALSE(weak_state.expired());
    REQUIRE_FALSE(weak_result.expired());

    release_flag(lookup.release);
    shutdown_threads.front().join();
    REQUIRE(shutdown_returned.load(std::memory_order_acquire));
    REQUIRE(drained);
    REQUIRE_FALSE(sched.is_running());
    REQUIRE(lookup.calls.load() == 1);
    REQUIRE(capacity->outstanding() == 0);
    REQUIRE(weak_job.expired());
    REQUIRE(weak_state.expired());
    REQUIRE(weak_result.expired());
}
