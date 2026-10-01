#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers.hpp>
#include <elio/coro/join_wait.hpp>
#include <elio/net/detail/resolve_job.hpp>
#include <elio/net/resolve_wait.hpp>
#include <elio/sync/event.hpp>
#if defined(ELIO_HAS_HTTP) && ELIO_HAS_HTTP
#include <elio/http/http_client.hpp>
#include <elio/http/websocket.hpp>
#include <elio/http/sse_client.hpp>
#if defined(ELIO_HAS_HTTP2) && ELIO_HAS_HTTP2
#include <elio/http/http2_client.hpp>
#endif
#endif
#include "../test_main.cpp"

#include <atomic>
#include <cerrno>
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
    int lookup_error = 0;
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
    if (control->lookup_error) return {{}, control->lookup_error};
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

TEST_CASE("public DNS fast paths bypass admission and retain owned outcomes",
          "[dns][resolve_wait][public][cache][contract]") {
    scheduler sched(1);
    lookup_control lookup;
    elio::net::resolve_cache cache;
    lookup_guard guard(lookup, [&] { sched.shutdown(); });
    auto domain = std::make_shared<elio::net::resolve_domain>(0);
    elio::net::resolve_wait_options options;
    options.domain = domain;
    options.lookup.use_cache = true;
    options.lookup.cache = &cache;
    cache.store({"cached.example", 80},
        {elio::net::socket_address(elio::net::ipv4_address("127.0.0.1", 80))},
        std::chrono::seconds(60));
    cache.store({"negative.example", 80}, {}, std::chrono::seconds(60), ENETUNREACH);
    sched.start();
    for (const std::string host : {"127.0.0.1", "::1", "", "cached.example", "negative.example", "miss.example"}) {
        auto observer = sched.go_joinable([host, options]() -> task<elio::net::resolve_result> {
            co_return co_await elio::net::resolve_all(host, 80, options, {});
        });
        REQUIRE(wait_for([&] { return observer.await_ready(); }));
        auto result = observer.await_resume();
        observer.wait_destroyed();
        if (host == "negative.example" || host == "miss.example") {
            REQUIRE_FALSE(result);
            REQUIRE(result.status == elio::net::resolve_status::failed);
            REQUIRE(result.error == (host == "negative.example" ? ENETUNREACH : EAGAIN));
        } else {
            REQUIRE(result);
            REQUIRE(result.error == 0);
            REQUIRE(result.addresses.size() == 1);
            REQUIRE(result.addresses.front().port() == 80);
        }
        REQUIRE(domain->outstanding() == 0);
    }
    REQUIRE(lookup.calls.load() == 0);
}

TEST_CASE("public DNS precancellation and expired deadlines do not touch cache or libc",
          "[dns][resolve_wait][public][cancellation][deadline][contract]") {
    const bool cancel = GENERATE(false, true);
    const std::string host = GENERATE("127.0.0.1", "preflight.example");
    scheduler sched(1);
    lookup_control lookup;
    elio::net::resolve_cache cache;
    lookup_guard guard(lookup, [&] { sched.shutdown(); });
    elio::net::resolve_wait_options options;
    options.lookup.use_cache = true;
    options.lookup.cache = &cache;
    options.domain = std::make_shared<elio::net::resolve_domain>(1);
    cancel_source source;
    if (cancel) source.cancel();
    else options.deadline = std::chrono::steady_clock::now() - std::chrono::seconds(1);
    sched.start();
    auto observer = sched.go_joinable([host, options, token = source.get_token()]() -> task<elio::net::resolve_result> {
        co_return co_await elio::net::resolve_all(host, 80, options, token);
    });
    REQUIRE(wait_for([&] { return observer.await_ready(); }));
    auto result = observer.await_resume();
    observer.wait_destroyed();
    REQUIRE_FALSE(result);
    REQUIRE(result.status == (cancel ? elio::net::resolve_status::cancelled : elio::net::resolve_status::timed_out));
    REQUIRE(result.error == (cancel ? ECANCELED : ETIMEDOUT));
    REQUIRE(options.domain->outstanding() == 0);
    REQUIRE(lookup.calls.load() == 0);
    REQUIRE(cache.stats().cache_misses == 0);
}

TEST_CASE("public DNS departure releases the observer and its borrowed cache before late work",
          "[dns][resolve_wait][public][running][lifetime][regression]") {
    const bool deadline = GENERATE(false, true);
    const bool throw_failure = GENERATE(false, true);
    scheduler sched(2);
    lookup_control lookup;
    lookup.throw_failure = throw_failure;
    auto cache = std::make_unique<elio::net::resolve_cache>();
    lookup_guard guard(lookup, [&] { sched.shutdown(); }, deadline);
    auto domain = std::make_shared<elio::net::resolve_domain>(1);
    elio::net::resolve_wait_options options;
    options.domain = domain;
    options.lookup.use_cache = true;
    options.lookup.cache = cache.get();
    if (deadline) options.deadline = std::chrono::steady_clock::now() + std::chrono::hours(1);
    cancel_source source;
    std::string host(256, 'h');
    auto operation = elio::net::resolve_all(host, 8080, options, source.get_token());
    host.assign(256, 'x');
    sched.start();
    const auto previous_installs = elio::coro::detail::join_observer_installed_for_test.load(std::memory_order_acquire);
    auto observer = sched.go_joinable(std::move(operation));
    REQUIRE(wait_for([&] { return lookup.entered.load(std::memory_order_acquire); }));
    REQUIRE(lookup.observed_host == std::string(256, 'h'));
    REQUIRE(wait_for([&] {
        return elio::coro::detail::join_observer_installed_for_test.load(std::memory_order_acquire) > previous_installs;
    }));
    if (deadline) {
        REQUIRE(wait_for([&] { return lookup.timer_entered.load(std::memory_order_acquire); }));
        lookup.expire.set();
    } else source.cancel();
    REQUIRE(wait_for([&] { return observer.await_ready(); }));
    auto result = observer.await_resume();
    observer.wait_destroyed();
    REQUIRE_FALSE(result);
    REQUIRE(result.status == (deadline ? elio::net::resolve_status::timed_out : elio::net::resolve_status::cancelled));
    REQUIRE(result.error == (deadline ? ETIMEDOUT : ECANCELED));
    REQUIRE(domain->outstanding() == 1);
    REQUIRE_FALSE(lookup.release.load(std::memory_order_acquire));
    cache.reset();
    auto overloaded = sched.go_joinable([domain]() -> task<elio::net::resolve_result> {
        elio::net::resolve_wait_options next;
        next.domain = domain;
        co_return co_await elio::net::resolve_all("overload.example", 80, next, {});
    });
    REQUIRE(wait_for([&] { return overloaded.await_ready(); }));
    REQUIRE(overloaded.await_resume().error == EAGAIN);
    overloaded.wait_destroyed();
    release_flag(lookup.release);
    sched.shutdown();
    REQUIRE(domain->outstanding() == 0);
    REQUIRE(lookup.calls.load() == 1);
}

TEST_CASE("public DNS live completion alone publishes positive and negative cache entries",
          "[dns][resolve_wait][public][cache][completion][regression]") {
    const int error = GENERATE(0, ENETUNREACH);
    scheduler sched(1);
    lookup_control lookup;
    lookup.lookup_error = error;
    release_flag(lookup.release);
    elio::net::resolve_cache cache;
    lookup_guard guard(lookup, [&] { sched.shutdown(); });
    auto domain = std::make_shared<elio::net::resolve_domain>(1);
    elio::net::resolve_wait_options options;
    options.lookup.use_cache = true;
    options.lookup.cache = &cache;
    options.domain = domain;
    sched.start();
    for (int attempt = 0; attempt < 2; ++attempt) {
        auto observer = sched.go_joinable([options]() -> task<elio::net::resolve_result> {
            co_return co_await elio::net::resolve_all("published.example", 443, options, {});
        });
        REQUIRE(wait_for([&] { return observer.await_ready(); }));
        auto result = observer.await_resume();
        observer.wait_destroyed();
        REQUIRE(static_cast<bool>(result) == (error == 0));
        REQUIRE(result.error == error);
        errno = ENOSPC;
        REQUIRE(result.error == error);
    }
    REQUIRE(lookup.calls.load() == 1);
    REQUIRE(cache.stats().cache_stores == 1);
    REQUIRE(cache.stats().cache_hits == 1);
}

TEST_CASE("public queued DNS cancellation stays bounded and skips lookup on dequeue",
          "[dns][resolve_wait][public][queued][admission][regression]") {
    scheduler sched(1, elio::runtime::wait_strategy::blocking(), 1);
    lookup_control lookup;
    std::atomic<bool> blocker_entered{false};
    std::atomic<bool> blocker_release{false};
    lookup_guard guard(lookup, [&] {
        release_flag(blocker_release);
        sched.shutdown();
    });
    auto* pool = sched.get_blocking_pool();
    std::function<void()> blocker = [&] {
        release_flag(blocker_entered);
        hold_until_released(blocker_release);
    };
    REQUIRE(pool->submit_bounded(std::move(blocker), 1));
    REQUIRE(wait_for([&] { return blocker_entered.load(std::memory_order_acquire); }));
    auto domain = std::make_shared<elio::net::resolve_domain>(1);
    elio::net::resolve_wait_options options;
    options.domain = domain;
    cancel_source source;
    sched.start();
    auto observer = sched.go_joinable([options, token = source.get_token()]() -> task<elio::net::resolve_result> {
        co_return co_await elio::net::resolve_all("queued.example", 80, options, token);
    });
    REQUIRE(wait_for([&] { return pool->queued_count_for_test() == 1; }));
    source.cancel();
    REQUIRE(wait_for([&] { return observer.await_ready(); }));
    REQUIRE(observer.await_resume().status == elio::net::resolve_status::cancelled);
    observer.wait_destroyed();
    REQUIRE(domain->outstanding() == 1);
    for (int attempt = 0; attempt < 64; ++attempt) {
        auto rejected = sched.go_joinable([options]() -> task<elio::net::resolve_result> {
            co_return co_await elio::net::resolve_all("rejected.example", 80, options, {});
        });
        REQUIRE(wait_for([&] { return rejected.await_ready(); }));
        REQUIRE(rejected.await_resume().error == EAGAIN);
        rejected.wait_destroyed();
        REQUIRE(domain->outstanding() == 1);
        REQUIRE(pool->queued_count_for_test() == 1);
    }
    release_flag(blocker_release);
    REQUIRE(wait_for([&] { return domain->outstanding() == 0; }));
    REQUIRE(lookup.calls.load() == 0);
}

TEST_CASE("public DNS stopped-pool rejection releases admission and never caches overload",
          "[dns][resolve_wait][public][shutdown][cache][regression]") {
    scheduler sched(1);
    lookup_control lookup;
    elio::net::resolve_cache cache;
    lookup_guard guard(lookup, [&] { sched.shutdown(); });
    auto domain = std::make_shared<elio::net::resolve_domain>(1);
    elio::net::resolve_wait_options options;
    options.lookup.use_cache = true;
    options.lookup.cache = &cache;
    options.domain = domain;
    sched.get_blocking_pool()->shutdown();
    sched.start();
    auto observer = sched.go_joinable([options]() -> task<elio::net::resolve_result> {
        co_return co_await elio::net::resolve_all("stopped.example", 80, options, {});
    });
    REQUIRE(wait_for([&] { return observer.await_ready(); }));
    REQUIRE(observer.await_resume().error == EAGAIN);
    observer.wait_destroyed();
    REQUIRE(domain->outstanding() == 0);
    REQUIRE(lookup.calls.load() == 0);
    REQUIRE(cache.stats().cache_stores == 0);
}

TEST_CASE("public DNS worker-side pool teardown rejects inline libc dispatch",
          "[dns][resolve_wait][public][shutdown][worker][regression]") {
    scheduler sched(2, elio::runtime::wait_strategy::blocking(), 1);
    lookup_control lookup;
    elio::net::resolve_cache cache;
    std::atomic<bool> blocker_entered{false};
    std::atomic<bool> blocker_release{false};
    lookup_guard guard(lookup, [&] {
        release_flag(blocker_release);
        sched.shutdown();
    });
    auto* pool = sched.get_blocking_pool();
    std::function<void()> blocker = [&] {
        release_flag(blocker_entered);
        hold_until_released(blocker_release);
    };
    REQUIRE(pool->submit_bounded(std::move(blocker), 1));
    REQUIRE(wait_for([&] { return blocker_entered.load(std::memory_order_acquire); }));
    auto domain = std::make_shared<elio::net::resolve_domain>(1);
    elio::net::resolve_wait_options options;
    options.domain = domain;
    options.lookup.use_cache = true;
    options.lookup.cache = &cache;
    sched.start();
    auto observer = sched.go_joinable([options]() -> task<elio::net::resolve_result> {
        co_return co_await elio::net::resolve_all("inline.example", 80, options, {});
    });
    REQUIRE(wait_for([&] { return pool->queued_count_for_test() == 1; }));
    auto teardown = sched.go_joinable([pool]() -> task<void> {
        pool->shutdown();
        co_return;
    });
    REQUIRE(wait_for([&] { return pool->stopped_for_test(); }));
    release_flag(blocker_release);
    REQUIRE(wait_for([&] { return observer.await_ready() && teardown.await_ready(); }));
    REQUIRE(observer.await_resume().error == EAGAIN);
    teardown.await_resume();
    observer.wait_destroyed();
    teardown.wait_destroyed();
    REQUIRE(domain->outstanding() == 0);
    REQUIRE(lookup.calls.load() == 0);
    REQUIRE(cache.stats().cache_stores == 0);
}

#if defined(ELIO_HAS_HTTP) && ELIO_HAS_HTTP
TEST_CASE("HTTP connection pool DNS override snapshots survive direct co_await calls",
          "[dns][resolve_wait][public][client][http][lifetime][regression]") {
    STATIC_REQUIRE(!std::is_aggregate_v<elio::http::connection_pool::dns_options>);
    scheduler sched(1);
    lookup_control lookup;
    lookup_guard guard(lookup, [&] { sched.shutdown(); });
    auto domain = std::make_shared<elio::net::resolve_domain>(0);
    sched.start();
    auto observer = sched.go_joinable([domain]() -> task<long> {
        elio::http::client_config config;
        config.resolve_options.use_cache = false;
        elio::http::connection_pool pool(config);
        auto result = co_await pool.acquire_result("direct-snapshot.example", 80, false,
            nullptr, std::chrono::nanoseconds::zero(), {},
            elio::http::connection_pool::dns_options{
                std::chrono::nanoseconds::zero(), domain});
        const auto* error = std::get_if<elio::http::client_error>(&result);
        if (!error || error->stage != elio::http::client_stage::resolve ||
            error->code.value() != EAGAIN) {
            co_return -1;
        }
        co_return domain.use_count();
    });
    REQUIRE(wait_for([&] { return observer.await_ready(); }));
    REQUIRE(observer.await_resume() == 2);
    observer.wait_destroyed();
    REQUIRE(domain.use_count() == 1);
    REQUIRE(domain->outstanding() == 0);
    REQUIRE(lookup.calls.load() == 0);
}

TEST_CASE("HTTP connection pool direct DNS override branches keep constructor and null-domain semantics",
          "[dns][resolve_wait][public][client][http][configuration][regression]") {
    const int method = GENERATE(0, 1);
    const int override_mode = GENERATE(0, 1);
    scheduler sched(2);
    lookup_control lookup;
    lookup.lookup_error = ENETUNREACH;
    lookup_guard guard(lookup, [&] { sched.shutdown(); }, true);
    auto domain = std::make_shared<elio::net::resolve_domain>(1);
    if (override_mode == 1) release_flag(lookup.release);
    sched.start();
    auto observer = sched.go_joinable([domain, method, override_mode]() -> task<int> {
        elio::http::client_config config;
        config.resolve_options.use_cache = false;
        config.dns_domain = domain;
        config.dns_timeout = std::chrono::hours(1);
        elio::http::connection_pool pool(config);
        if (method == 0) {
            if (override_mode == 0) {
                auto result = co_await pool.acquire_result("direct-override.example", 80, false);
                const auto* error = std::get_if<elio::http::client_error>(&result);
                if (!error || error->stage != elio::http::client_stage::resolve) co_return 0;
                co_return error->code.value();
            }
            auto result = co_await pool.acquire_result("direct-override.example", 80, false,
                nullptr, std::chrono::nanoseconds::zero(), {},
                elio::http::connection_pool::dns_options{});
            const auto* error = std::get_if<elio::http::client_error>(&result);
            if (!error || error->stage != elio::http::client_stage::resolve) co_return 0;
            co_return error->code.value();
        }
        errno = 0;
        std::optional<elio::http::connection> conn;
        if (override_mode == 0) {
            conn = co_await pool.acquire("direct-override.example", 80, false);
        } else {
            conn = co_await pool.acquire("direct-override.example", 80, false,
                nullptr, std::chrono::nanoseconds::zero(), {},
                elio::http::connection_pool::dns_options{});
        }
        if (conn) co_return 0;
        co_return errno;
    });
    REQUIRE(wait_for([&] { return lookup.entered.load(std::memory_order_acquire); }));
    if (override_mode == 0) {
        REQUIRE(wait_for([&] { return lookup.timer_entered.load(std::memory_order_acquire); }));
        lookup.expire.set();
    }
    REQUIRE(wait_for([&] { return observer.await_ready(); }));
    REQUIRE(observer.await_resume() == (override_mode == 0 ? ETIMEDOUT : ENETUNREACH));
    observer.wait_destroyed();
    REQUIRE(lookup.calls.load() == 1);
    REQUIRE(lookup.timer_entered.load(std::memory_order_acquire) == (override_mode == 0));
    REQUIRE(domain->outstanding() == (override_mode == 0 ? 1 : 0));
    release_flag(lookup.release);
    sched.shutdown();
    REQUIRE(domain->outstanding() == 0);
}

TEST_CASE("HTTP DNS admission is frozen in the owning transport",
          "[dns][resolve_wait][public][client][http][configuration][regression]") {
    const int update = GENERATE(0, 1, 2);
    scheduler sched(1);
    lookup_control lookup;
    lookup.lookup_error = ENETUNREACH;
    release_flag(lookup.release);
    lookup_guard guard(lookup, [&] { sched.shutdown(); });
    sched.start();
    auto observer = sched.go_joinable([update]() -> task<int> {
        elio::http::client_config config;
        config.resolve_options.use_cache = false;
        config.dns_domain = std::make_shared<elio::net::resolve_domain>(update == 0 ? 1 : 0);
        elio::http::client client(config);
        if (update == 2) client.config().dns_domain.reset();
        else client.config().dns_domain = std::make_shared<elio::net::resolve_domain>(update == 0 ? 0 : 1);
        auto result = co_await client.get_result("http://updated-domain.example/");
        const auto* error = std::get_if<elio::http::client_error>(&result);
        if (!error || error->stage != elio::http::client_stage::resolve) co_return 0;
        co_return error->code.value();
    });
    REQUIRE(wait_for([&] { return observer.await_ready(); }));
    REQUIRE(observer.await_resume() == (update == 0 ? ENETUNREACH : EAGAIN));
    observer.wait_destroyed();
    REQUIRE(lookup.calls.load() == (update == 0 ? 1 : 0));
}

TEST_CASE("HTTP WebSocket and SSE DNS waiting uses an independent disabled-by-default budget",
          "[dns][resolve_wait][public][client][http][regression]") {
    const int client_kind = GENERATE(0, 1, 2);
    const bool deadline = GENERATE(false, true);
    scheduler sched(2);
    lookup_control lookup;
    lookup_guard guard(lookup, [&] { sched.shutdown(); }, deadline);
    auto domain = std::make_shared<elio::net::resolve_domain>(1);
    cancel_source source;
    sched.start();
    auto observer = sched.go_joinable([domain, client_kind, deadline, token = source.get_token()]() -> task<int> {
        auto configure = [&](auto& config) {
            config.dns_domain = domain;
            config.resolve_options.use_cache = false;
            config.connect_timeout = std::chrono::seconds(1);
            if (deadline) config.dns_timeout = std::chrono::hours(1);
        };
        if (client_kind == 0) {
            elio::http::client_config config;
            configure(config);
            elio::http::client client(config);
            auto result = co_await client.get_result("http://client-dns.example/", token);
            const auto* error = std::get_if<elio::http::client_error>(&result);
            if (!error || error->stage != elio::http::client_stage::resolve) co_return 0;
            co_return error->code.value();
        }
        if (client_kind == 1) {
            elio::http::websocket::client_config config;
            configure(config);
            elio::http::websocket::ws_client client(config);
            if (co_await client.connect("ws://client-dns.example/", token)) co_return 0;
            co_return errno;
        }
        elio::http::sse::client_config config;
        configure(config);
        elio::http::sse::sse_client client(config);
        if (co_await client.connect("http://client-dns.example/", token)) co_return 0;
        co_return errno;
    });
    REQUIRE(wait_for([&] { return lookup.entered.load(std::memory_order_acquire); }));
    if (deadline) {
        REQUIRE(wait_for([&] { return lookup.timer_entered.load(std::memory_order_acquire); }));
        lookup.expire.set();
    } else {
        REQUIRE_FALSE(lookup.timer_entered.load(std::memory_order_acquire));
        source.cancel();
    }
    REQUIRE(wait_for([&] { return observer.await_ready(); }));
    REQUIRE(observer.await_resume() == (deadline ? ETIMEDOUT : ECANCELED));
    observer.wait_destroyed();
    REQUIRE(domain->outstanding() == 1);
    REQUIRE_FALSE(lookup.release.load(std::memory_order_acquire));
    release_flag(lookup.release);
    sched.shutdown();
    REQUIRE(domain->outstanding() == 0);
}
#if defined(ELIO_HAS_HTTP2) && ELIO_HAS_HTTP2
TEST_CASE("HTTP2 DNS budget expires before TCP TLS setup without changing connect timeout",
          "[dns][resolve_wait][public][client][http2][regression]") {
    scheduler sched(2);
    lookup_control lookup;
    lookup_guard guard(lookup, [&] { sched.shutdown(); }, true);
    auto domain = std::make_shared<elio::net::resolve_domain>(1);
    sched.start();
    auto observer = sched.go_joinable([domain]() -> task<int> {
        elio::http::h2_client_config config;
        config.dns_domain = domain;
        config.dns_timeout = std::chrono::hours(1);
        config.connect_timeout = std::chrono::seconds(1);
        config.resolve_options.use_cache = false;
        elio::http::h2_client client(config);
        if (co_await client.get("https://client-dns.example/")) co_return 0;
        co_return errno;
    });
    REQUIRE(wait_for([&] { return lookup.entered.load(std::memory_order_acquire); }));
    REQUIRE(wait_for([&] { return lookup.timer_entered.load(std::memory_order_acquire); }));
    lookup.expire.set();
    REQUIRE(wait_for([&] { return observer.await_ready(); }));
    REQUIRE(observer.await_resume() == ETIMEDOUT);
    observer.wait_destroyed();
    REQUIRE(domain->outstanding() == 1);
    REQUIRE_FALSE(lookup.release.load(std::memory_order_acquire));
    release_flag(lookup.release);
    sched.shutdown();
    REQUIRE(domain->outstanding() == 0);
}
#endif
#endif

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
