#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <elio/http/http_client.hpp>

#include <atomic>
#include <algorithm>
#include <array>
#include <latch>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

using namespace elio::http;
using elio::coro::task;

namespace {

template<typename T>
T immediate(task<T> operation) {
    auto handle = elio::coro::detail::task_access::handle(operation);
    handle.resume();
    REQUIRE(handle.done());
    return operation.await_resume();
}

struct observation {
    size_t open = 0;
    size_t peak = 0;
    size_t closed = 0;
};

class probe_stream {
public:
    explicit probe_stream(std::shared_ptr<observation> value)
        : owner_(std::move(value)) {
        owner_->peak = std::max(owner_->peak, ++owner_->open);
    }
    probe_stream(probe_stream&& other) noexcept = default;
    probe_stream& operator=(probe_stream&& other) noexcept {
        if (this != &other) {
            close();
            owner_ = std::move(other.owner_);
            used_ = other.used_;
        }
        return *this;
    }
    ~probe_stream() { close(); }
    auto last_use() const noexcept { return used_; }
    void touch() noexcept { used_ = std::chrono::steady_clock::now(); }
private:
    void close() noexcept {
        if (auto owner = std::move(owner_)) {
            --owner->open;
            ++owner->closed;
        }
    }
    std::shared_ptr<observation> owner_;
    std::chrono::steady_clock::time_point used_ = std::chrono::steady_clock::now();
};

using pool_type = detail::bounded_pool<probe_stream>;
using grant = pool_type::grant;

detail::connection_key route(std::string host = "one.invalid") {
    detail::connection_key result;
    result.target = detail::route_endpoint::from(std::move(host), 80);
    return result;
}

grant acquire(pool_type& pool, const detail::connection_key& key = route()) {
    auto result = immediate(pool.acquire(key, {}, {}));
    REQUIRE(std::holds_alternative<grant>(result));
    return std::move(std::get<grant>(result));
}

void rejected(pool_type& pool, int expected, const detail::connection_key& key = route()) {
    auto result = immediate(pool.acquire(key, {}, {}));
    REQUIRE(std::holds_alternative<client_error>(result));
    CHECK(std::get<client_error>(result).code.value() == expected);
    CHECK(std::get<client_error>(result).stage == client_stage::acquire);
}

struct queue_observation {
    elio::sync::event queued;
};
std::atomic<queue_observation*> observing_queue{nullptr};
void queue_entered() { observing_queue.load()->queued.set(); }
struct queue_guard {
    explicit queue_guard(queue_observation& value) {
        observing_queue.store(&value);
        detail::pool_waiter_queued_for_test.store(queue_entered);
    }
    ~queue_guard() {
        detail::pool_waiter_queued_for_test.store(nullptr);
        observing_queue.store(nullptr);
    }
};

struct timer_observation {
    elio::sync::event entered;
    elio::sync::event expire;
    std::chrono::steady_clock::time_point deadline;
};
std::atomic<timer_observation*> observing_timer{nullptr};
task<elio::coro::cancel_result> controlled_timer(std::chrono::steady_clock::time_point deadline,
        elio::coro::cancel_token stop) {
    auto& observed = *observing_timer.load();
    observed.deadline = deadline;
    observed.entered.set();
    co_return co_await observed.expire.wait(stop);
}
struct timer_guard {
    explicit timer_guard(timer_observation& value) {
        observing_timer.store(&value);
        elio::coro::detail::join_timer_wait_for_test.store(controlled_timer);
    }
    ~timer_guard() {
        elio::coro::detail::join_timer_wait_for_test.store(nullptr);
        observing_timer.store(nullptr);
    }
};

struct dns_observation {
    elio::sync::event entered;
    std::latch proceed{1};
};
std::atomic<dns_observation*> observing_dns{nullptr};
elio::net::detail::dns_lookup_result paused_dns(std::string_view, uint16_t) {
    auto& observed = *observing_dns.load();
    observed.entered.set();
    observed.proceed.wait();
    return {{}, EHOSTUNREACH, false};
}
struct dns_guard {
    explicit dns_guard(dns_observation& value) {
        observing_dns.store(&value);
        elio::net::detail::owned_dns_lookup_for_test.store(paused_dns);
    }
    ~dns_guard() {
        elio::net::detail::owned_dns_lookup_for_test.store(nullptr);
        observing_dns.store(nullptr);
    }
};

struct transport_dials {
    std::vector<connection> ready;
    size_t calls = 0;
    bool throw_exception = false;
    std::optional<std::chrono::steady_clock::time_point> deadline;
};
std::atomic<transport_dials*> observing_transport_dials{nullptr};
task<client_result<connection>> fixture_dial(const detail::route_plan&,
        std::chrono::nanoseconds, elio::coro::cancel_token) {
    auto& observed = *observing_transport_dials.load();
    if (observed.throw_exception) throw std::runtime_error("scripted dial exception");
    const auto index = observed.calls++;
    if (index >= observed.ready.size())
        co_return detail::make_client_error(ECONNREFUSED, client_stage::connect);
    co_return std::move(observed.ready[index]);
}
void observe_deadline(std::optional<std::chrono::steady_clock::time_point> value) {
    observing_transport_dials.load()->deadline = value;
}
struct transport_dial_guard {
    explicit transport_dial_guard(transport_dials& value) {
        observing_transport_dials.store(&value);
        detail::route_connect_for_test.store(fixture_dial);
        detail::route_deadline_for_test.store(observe_deadline);
    }
    ~transport_dial_guard() {
        detail::route_deadline_for_test.store(nullptr);
        detail::route_connect_for_test.store(nullptr);
        observing_transport_dials.store(nullptr);
    }
};

struct socket_pair {
    elio::net::tcp_stream peer{-1};
    explicit socket_pair(transport_dials& dials) {
        std::array<int, 2> descriptors{};
        if (::socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC,
                         0, descriptors.data()) != 0)
            throw std::runtime_error("socketpair failed");
        dials.ready.emplace_back(elio::net::tcp_stream(descriptors[0]));
        peer = elio::net::tcp_stream(descriptors[1]);
    }
};

task<void> serve_twice(elio::net::tcp_stream& peer) {
    for (size_t count = 0; count < 2; ++count) {
        std::string headers;
        std::array<char, 512> buffer{};
        while (headers.find("\r\n\r\n") == std::string::npos) {
            const auto read = co_await peer.read(buffer.data(), buffer.size());
            if (read.result <= 0) throw std::runtime_error("request headers missing");
            headers.append(buffer.data(), static_cast<size_t>(read.result));
        }
        const auto sent = co_await peer.write_exactly(
            "HTTP/1.1 200 OK\r\nContent-Length: 2\r\nConnection: keep-alive\r\n\r\nok");
        if (sent.result <= 0) throw std::runtime_error("fixture response send failed");
    }
}

struct backend_guard {
    elio::io::io_context::backend_type old;
    explicit backend_guard(elio::io::io_context::backend_type value)
        : old(elio::runtime::detail::worker_io_backend_for_test.exchange(value)) {}
    ~backend_guard() { elio::runtime::detail::worker_io_backend_for_test.store(old); }
};

template<typename F>
void run(F operation) {
    elio::runtime::scheduler scheduler(1);
    scheduler.start();
    auto parent = scheduler.go_joinable(std::move(operation));
    parent.wait_destroyed();
    scheduler.shutdown();
    parent.await_resume();
}

} // namespace

TEST_CASE("HTTP finite admission is opt-in and each zero limit denies its resource",
          "[http][pool][issue-1248]") {
    transport_config config;
    CHECK_FALSE(config.limits);
    CHECK(config.acquisition_timeout == std::chrono::nanoseconds::zero());
    pool_limits limits;
    CHECK(limits.max_idle_per_route == 6);
    CHECK(limits.max_idle_total == 64);
    CHECK(limits.max_live_per_route == 12);
    CHECK(limits.max_live_total == 128);
    CHECK(limits.max_dials_total == 16);
    CHECK(limits.max_waiters_total == 256);
    CHECK(limits.max_route_buckets == 128);
    const auto selected = GENERATE(0, 1, 2, 3);
    if (selected == 0) limits.max_live_per_route = 0;
    if (selected == 1) limits.max_live_total = 0;
    if (selected == 2) limits.max_dials_total = 0;
    if (selected == 3) limits.max_route_buckets = 0;
    pool_type pool(limits, std::chrono::seconds(60));
    rejected(pool, EAGAIN);
    CHECK(pool.counters_for_test().live == 0);
    CHECK(pool.counters_for_test().routes == 0);
}

TEST_CASE("HTTP reservations cap live connections and dials before establishment",
          "[http][pool][issue-1248]") {
    pool_limits limits;
    limits.max_live_per_route = 1;
    limits.max_live_total = 2;
    limits.max_dials_total = 1;
    limits.max_waiters_total = 0;
    pool_type pool(limits, std::chrono::seconds(60));
    auto one = acquire(pool);
    CHECK(pool.counters_for_test().live == 1);
    CHECK(pool.counters_for_test().dialing == 1);
    rejected(pool, EAGAIN);
    rejected(pool, EAGAIN, route("two.invalid"));
    one.capacity.dial_complete();
    one.capacity.dial_complete();
    auto two = acquire(pool, route("two.invalid"));
    CHECK(pool.counters_for_test().live == 2);
    CHECK(pool.counters_for_test().dialing == 1);
    rejected(pool, EAGAIN, route("three.invalid"));
    pool_type::permit moved(std::move(two.capacity));
    two.capacity.reset();
    moved.reset();
    moved.reset();
    CHECK(pool.counters_for_test().live == 1);
    CHECK(pool.counters_for_test().dialing == 0);
    one.capacity.reset();
    CHECK(pool.counters_for_test().live == 0);
    CHECK(pool.counters_for_test().routes == 0);
}

TEST_CASE("HTTP bounded idle retention preserves live ownership until stream close",
          "[http][pool][issue-1248]") {
    pool_limits limits;
    limits.max_idle_per_route = 1;
    limits.max_idle_total = 1;
    pool_type pool(limits, std::chrono::seconds(60));
    auto observed = std::make_shared<observation>();
    auto one = acquire(pool);
    one.capacity.dial_complete();
    probe_stream stream(observed);
    { auto returned = pool.retain(one.capacity, stream); CHECK(returned.retained); }
    CHECK(pool.counters_for_test().live == 1);
    CHECK(pool.counters_for_test().idle == 1);
    auto reused = acquire(pool);
    REQUIRE(reused.idle);
    CHECK(pool.counters_for_test().dialing == 0);
    CHECK(pool.counters_for_test().live == 1);
    { auto returned = pool.retain(reused.capacity, *reused.idle); CHECK(returned.retained); }
    {
        auto retired = pool.clear();
        CHECK(pool.counters_for_test().idle == 0);
        CHECK(pool.counters_for_test().live == 1);
        CHECK(observed->open == 1);
    }
    CHECK(observed->closed == 1);
    CHECK(pool.counters_for_test().live == 0);
    CHECK(pool.counters_for_test().routes == 0);
}

TEST_CASE("HTTP route churn evicts idle metadata without exceeding physical live capacity",
          "[http][pool][issue-1248]") {
    pool_limits limits;
    limits.max_live_per_route = 1;
    limits.max_live_total = 1;
    limits.max_route_buckets = GENERATE(1u, 2u);
    pool_type pool(limits, std::chrono::seconds(60));
    auto observed = std::make_shared<observation>();
    for (size_t index = 0; index < 20; ++index) {
        auto reserved = acquire(pool, route(std::to_string(index) + ".invalid"));
        CHECK_FALSE(reserved.idle);
        reserved.capacity.dial_complete();
        probe_stream stream(observed);
        { auto returned = pool.retain(reserved.capacity, stream); CHECK(returned.retained); }
        CHECK(pool.counters_for_test().live == 1);
        CHECK(pool.counters_for_test().routes <= limits.max_route_buckets);
        CHECK(observed->peak == 1);
    }
    { auto retired = pool.clear(); }
    CHECK(observed->closed == 20);
    CHECK(pool.counters_for_test().routes == 0);
}

TEST_CASE("HTTP queued acquisition is cancellable and shutdown does not await queued permits",
          "[http][pool][issue-1248]") {
    const bool close = GENERATE(false, true);
    pool_limits limits;
    limits.max_live_per_route = 1;
    limits.max_live_total = 1;
    pool_type pool(limits, std::chrono::seconds(60));
    queue_observation observed;
    queue_guard guard(observed);
    run([&]() -> task<void> {
        auto first_result = co_await pool.acquire(route(), {}, {});
        auto first = std::move(std::get<grant>(first_result));
        elio::coro::cancel_source stop;
        auto waiting = elio::runtime::scheduler::current()->go_joinable([&]() -> task<client_result<grant>> {
            co_return co_await pool.acquire(route(), {}, stop.get_token());
        });
        co_await observed.queued.wait();
        CHECK(pool.counters_for_test().waiting == 1);
        if (close) { auto retired = pool.clear(true); }
        else stop.cancel();
        auto result = co_await waiting;
        co_await waiting.wait_destroyed_async();
        REQUIRE(std::holds_alternative<client_error>(result));
        CHECK(std::get<client_error>(result).code.value() == (close ? ESHUTDOWN : ECANCELED));
        CHECK(pool.counters_for_test().waiting == 0);
        CHECK(pool.counters_for_test().live == 1);
        first.capacity.reset();
        CHECK(pool.counters_for_test().live == 0);
        CHECK(pool.counters_for_test().routes == 0);
    });
}

TEST_CASE("HTTP FIFO grants cannot be bypassed and reclaim cancellation after grant",
          "[http][pool][issue-1248]") {
    const bool cancel_selected = GENERATE(false, true);
    pool_limits limits;
    limits.max_live_total = 1;
    limits.max_live_per_route = 1;
    limits.max_waiters_total = 2;
    pool_type pool(limits, std::chrono::seconds(60));
    queue_observation observed;
    queue_guard guard(observed);
    run([&]() -> task<void> {
        auto first_result = co_await pool.acquire(route(), {}, {});
        auto first = std::move(std::get<grant>(first_result));
        elio::coro::cancel_source stop;
        auto second = elio::runtime::scheduler::current()->go_joinable([&]() -> task<client_result<grant>> {
            co_return co_await pool.acquire(route(), {}, stop.get_token());
        });
        co_await observed.queued.wait();
        observed.queued.reset();
        auto third = elio::runtime::scheduler::current()->go_joinable([&]() -> task<client_result<grant>> {
            co_return co_await pool.acquire(route("two.invalid"), {}, {});
        });
        co_await observed.queued.wait();
        CHECK(pool.counters_for_test().waiting == 2);
        auto overloaded = co_await pool.acquire(route("three.invalid"), {}, {});
        REQUIRE(std::holds_alternative<client_error>(overloaded));
        CHECK(std::get<client_error>(overloaded).code.value() == EAGAIN);
        first.capacity.reset();
        if (cancel_selected) stop.cancel();
        auto second_result = co_await second;
        co_await second.wait_destroyed_async();
        // Completion-selected handoff may beat cancellation; either outcome
        // must retain/reclaim exactly one live reservation, never two or zero.
        if (auto* selected = std::get_if<grant>(&second_result)) {
            CHECK(pool.counters_for_test().waiting == 1);
            CHECK(pool.counters_for_test().live == 1);
            selected->capacity.reset();
        } else {
            CHECK(cancel_selected);
            CHECK(std::get<client_error>(second_result).code.value() == ECANCELED);
        }
        auto third_result = co_await third;
        co_await third.wait_destroyed_async();
        REQUIRE(std::holds_alternative<grant>(third_result));
        CHECK(pool.counters_for_test().waiting == 0);
        CHECK(pool.counters_for_test().live == 1);
        std::get<grant>(third_result).capacity.reset();
        CHECK(pool.counters_for_test().live == 0);
        CHECK(pool.counters_for_test().routes == 0);
    });
}

TEST_CASE("HTTP queue expiry uses one absolute deadline and is distinct from overload",
          "[http][pool][issue-1248]") {
    pool_limits limits;
    limits.max_live_total = 1;
    pool_type pool(limits, std::chrono::seconds(60));
    timer_observation observed;
    timer_guard guard(observed);
    run([&]() -> task<void> {
        auto first_result = co_await pool.acquire(route(), {}, {});
        auto first = std::move(std::get<grant>(first_result));
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
        auto waiting = elio::runtime::scheduler::current()->go_joinable([&]() -> task<client_result<grant>> {
            co_return co_await pool.acquire(route(), deadline, {});
        });
        co_await observed.entered.wait();
        CHECK(observed.deadline == deadline);
        observed.expire.set();
        auto result = co_await waiting;
        co_await waiting.wait_destroyed_async();
        REQUIRE(std::holds_alternative<client_error>(result));
        CHECK(std::get<client_error>(result).code.value() == ETIMEDOUT);
        CHECK(std::get<client_error>(result).stage == client_stage::acquire);
        CHECK(pool.counters_for_test().waiting == 0);
        CHECK(pool.counters_for_test().live == 1);
        first.capacity.reset();
    });
}

TEST_CASE("HTTP acquisition deadline bounds DNS observer departure without destroying libc work",
          "[http][pool][dns][issue-1248]") {
    timer_observation timer;
    timer_guard clock(timer);
    dns_observation dns;
    dns_guard lookup(dns);
    auto domain = std::make_shared<elio::net::resolve_domain>(1);
    run([&]() -> task<void> {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
        elio::net::resolve_options options;
        options.use_cache = false;
        auto connecting = elio::runtime::scheduler::current()->go_joinable([&]() -> task<client_result<connection>> {
            co_return co_await client_connect_result("acquisition.invalid", 80, false, nullptr,
                options, false, std::chrono::seconds(60), {}, std::chrono::seconds(60),
                domain, deadline);
        });
        co_await timer.entered.wait();
        co_await dns.entered.wait();
        CHECK(timer.deadline == deadline);
        timer.expire.set();
        auto result = co_await connecting;
        co_await connecting.wait_destroyed_async();
        CHECK(domain->outstanding() == 1);
        dns.proceed.count_down();
        REQUIRE(std::holds_alternative<client_error>(result));
        CHECK(std::get<client_error>(result).code.value() == ETIMEDOUT);
        CHECK(std::get<client_error>(result).stage == client_stage::resolve);
    });
    CHECK(domain->outstanding() == 0);
}

TEST_CASE("HTTP expired acquisition budget rejects even literal DNS fast paths",
          "[http][pool][issue-1248]") {
    const auto deadline = std::chrono::steady_clock::now() - std::chrono::seconds(1);
    auto result = immediate(client_connect_result("127.0.0.1", 80, false, nullptr,
        {}, false, {}, {}, {}, {}, deadline));
    REQUIRE(std::holds_alternative<client_error>(result));
    CHECK(std::get<client_error>(result).code.value() == ETIMEDOUT);
    CHECK(std::get<client_error>(result).stage == client_stage::resolve);
}

TEST_CASE("HTTP Transport carries the queue deadline into establishment without restarting it",
          "[http][pool][issue-1248]") {
    transport_config config;
    config.limits = pool_limits{};
    config.limits->max_live_total = 1;
    config.acquisition_timeout = std::chrono::seconds(60);
    transport owner(config);
    transport_dials dials;
    socket_pair one(dials);
    socket_pair two(dials);
    transport_dial_guard connector(dials);
    timer_observation timer;
    timer_guard clock(timer);
    run([&]() -> task<void> {
        const auto target = *url::parse("http://one.invalid/");
        auto first_result = co_await owner.acquire_lease_for_test(target);
        REQUIRE(std::holds_alternative<transport::connection_lease_for_test>(first_result));
        auto waiting = elio::runtime::scheduler::current()->go_joinable([&]() {
            return owner.acquire_lease_for_test(target);
        });
        co_await timer.entered.wait();
        const auto original_deadline = timer.deadline;
        CHECK(owner.admission_counters_for_test().waiting == 1);
        std::get<transport::connection_lease_for_test>(first_result).retire();
        auto second_result = co_await waiting;
        co_await waiting.wait_destroyed_async();
        REQUIRE(std::holds_alternative<transport::connection_lease_for_test>(second_result));
        REQUIRE(dials.deadline);
        CHECK(*dials.deadline == original_deadline);
        CHECK(owner.admission_counters_for_test().live == 1);
        CHECK(owner.admission_counters_for_test().dialing == 0);
        std::get<transport::connection_lease_for_test>(second_result).retire();
        CHECK(owner.admission_counters_for_test().live == 0);
    });
}

TEST_CASE("HTTP streaming keeps finite capacity through the handler and unread body",
          "[http][pool][streaming][issue-1248]") {
    const auto backend = GENERATE(elio::io::io_context::backend_type::epoll,
                                 elio::io::io_context::backend_type::io_uring);
#if ELIO_HAS_IO_URING
    if (backend == elio::io::io_context::backend_type::io_uring &&
        !elio::io::io_uring_backend::is_available()) SKIP("io_uring unavailable");
#else
    if (backend == elio::io::io_context::backend_type::io_uring) SKIP("io_uring not compiled");
#endif
    backend_guard restore(backend);
    transport_config config;
    config.limits = pool_limits{};
    config.limits->max_live_total = 1;
    config.limits->max_dials_total = 1;
    auto owner = std::make_shared<transport>(config);
    client first_client(owner), second_client(owner);
    transport_dials dials;
    socket_pair pair(dials);
    transport_dial_guard connector(dials);
    queue_observation queue;
    queue_guard queued(queue);
    elio::sync::event handler_entered;
    elio::sync::event release_handler;
    run([&]() -> task<void> {
        auto* scheduler = elio::runtime::scheduler::current();
        auto server = scheduler->go_joinable([&]() { return serve_twice(pair.peer); });
        auto streaming = scheduler->go_joinable([&]() -> task<client_result<std::monostate>> {
            co_return co_await first_client.with_response(request(method::GET, "/"),
                *url::parse("http://one.invalid/"), {},
                [&](const response&, response_body_reader& reader,
                        const elio::coro::cancel_token& token) -> task<void> {
                    handler_entered.set();
                    co_await release_handler.wait();
                    std::array<char, 2> bytes{};
                    while (!reader.complete()) {
                        auto part = co_await reader.read_into(std::span<char>(bytes), token);
                        if (std::holds_alternative<client_error>(part))
                            throw std::runtime_error("streaming fixture read failed");
                    }
                });
        });
        co_await handler_entered.wait();
        CHECK(owner->admission_counters_for_test().live == 1);
        CHECK(owner->admission_counters_for_test().idle == 0);
        auto buffered = scheduler->go_joinable([&]() {
            return second_client.get_result("http://one.invalid/");
        });
        co_await queue.queued.wait();
        CHECK(owner->admission_counters_for_test().waiting == 1);
        CHECK(dials.calls == 1);
        release_handler.set();
        auto streamed_result = co_await streaming;
        co_await streaming.wait_destroyed_async();
        auto buffered_result = co_await buffered;
        co_await buffered.wait_destroyed_async();
        co_await server;
        co_await server.wait_destroyed_async();
        CHECK(std::holds_alternative<std::monostate>(streamed_result));
        REQUIRE(std::holds_alternative<response>(buffered_result));
        CHECK(std::get<response>(buffered_result).body() == "ok");
        CHECK(dials.calls == 1);
        CHECK(owner->admission_counters_for_test().idle == 1);
        CHECK(owner->admission_counters_for_test().live == 1);
        owner->clear();
        CHECK(owner->admission_counters_for_test().live == 0);
    });
}

TEST_CASE("HTTP finite idle limits do not reinterpret zero as unlimited or block dial admission",
          "[http][pool][issue-1248]") {
    const int scenario = GENERATE(0, 1, 2);
    pool_limits limits;
    limits.max_idle_per_route = scenario == 0 ? 0 : 1;
    limits.max_idle_total = scenario == 1 ? 0 : 1;
    pool_type pool(limits, std::chrono::seconds(60));
    auto one = acquire(pool);
    auto two = acquire(pool);
    one.capacity.dial_complete();
    two.capacity.dial_complete();
    auto observed = std::make_shared<observation>();
    std::optional<probe_stream> first(std::in_place, observed);
    std::optional<probe_stream> second(std::in_place, observed);
    bool retained = false;
    {
        auto result = pool.retain(one.capacity, *first);
        retained = result.retained;
        CHECK(retained == (scenario == 2));
    }
    if (!retained) { first.reset(); one.capacity.reset(); }
    {
        auto result = pool.retain(two.capacity, *second);
        CHECK_FALSE(result.retained);
    }
    second.reset();
    two.capacity.reset();
    CHECK(pool.counters_for_test().idle == (scenario == 2 ? 1 : 0));
    CHECK(pool.counters_for_test().live == (scenario == 2 ? 1 : 0));
    { auto cleared = pool.clear(); }
    CHECK(observed->open == 0);
    CHECK(observed->closed == 2);
    CHECK(pool.counters_for_test().routes == 0);
}

TEST_CASE("HTTP route metadata rejects churn while its only bucket is active",
          "[http][pool][issue-1248]") {
    pool_limits limits;
    limits.max_route_buckets = 1;
    pool_type pool(limits, std::chrono::seconds(60));
    auto one = acquire(pool);
    rejected(pool, EAGAIN, route("two.invalid"));
    CHECK(pool.counters_for_test().routes == 1);
    one.capacity.reset();
    auto two = acquire(pool, route("two.invalid"));
    CHECK(pool.counters_for_test().routes == 1);
    two.capacity.reset();
    CHECK(pool.counters_for_test().routes == 0);
}

TEST_CASE("HTTP admission separates conservative forward routes from global proxy pressure",
          "[http][pool][issue-1248]") {
    pool_limits limits;
    limits.max_live_per_route = 1;
    limits.max_live_total = 2;
    limits.max_waiters_total = 0;
    pool_type pool(limits, std::chrono::seconds(60));
    auto first_key = route();
    first_key.mode = detail::route_mode::forward_proxy;
    first_key.target_dns = detail::route_dns_mode::proxy;
    first_key.hops.push_back({detail::route_endpoint::from("proxy.invalid", 3128), 0, 7});
    auto second_key = first_key;
    second_key.target.host = "two.invalid";
    auto third_key = first_key;
    third_key.hops.front().auth_domain = 8;
    auto one = acquire(pool, first_key);
    one.capacity.dial_complete();
    rejected(pool, EAGAIN, first_key);
    auto two = acquire(pool, second_key);
    CHECK(pool.counters_for_test().live == 2);
    rejected(pool, EAGAIN, third_key);
    one.capacity.reset();
    two.capacity.reset();
    CHECK(pool.counters_for_test().routes == 0);
}

TEST_CASE("HTTP Transport dial failure and exception return reserved permits exactly once",
          "[http][pool][issue-1248]") {
    const bool exception = GENERATE(false, true);
    transport_config config;
    config.limits = pool_limits{};
    config.limits->max_live_total = 1;
    transport owner(config);
    transport_dials observed;
    observed.throw_exception = exception;
    transport_dial_guard connector(observed);
    bool threw = false;
    try {
        auto result = immediate(owner.acquire_lease_for_test(*url::parse("http://one.invalid/")));
        REQUIRE(std::holds_alternative<client_error>(result));
        CHECK(std::get<client_error>(result).code.value() == ECONNREFUSED);
    } catch (const std::runtime_error&) { threw = true; }
    CHECK(threw == exception);
    CHECK(owner.admission_counters_for_test().live == 0);
    CHECK(owner.admission_counters_for_test().dialing == 0);
    CHECK(owner.admission_counters_for_test().routes == 0);
    CHECK(immediate(owner.shutdown()) == elio::coro::cancel_result::completed);
}
