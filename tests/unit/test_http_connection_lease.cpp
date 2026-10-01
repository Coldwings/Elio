#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#include <elio/http/http_client.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <fcntl.h>
#include <latch>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
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

using lease = transport::connection_lease_for_test;

struct socket_pair {
    elio::net::tcp_stream client{-1};
    elio::net::tcp_stream peer{-1};
    socket_pair() {
        std::array<int, 2> descriptors{};
        if (::socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC,
                         0, descriptors.data()) != 0)
            throw std::runtime_error("socketpair failed");
        client = elio::net::tcp_stream(descriptors[0]);
        peer = elio::net::tcp_stream(descriptors[1]);
    }
};

struct dial_observation {
    std::vector<connection> connections;
    std::atomic<size_t> calls{0};
    bool pause_first = false;
    elio::sync::event entered;
    elio::sync::event proceed;
};

std::atomic<dial_observation*> observing_dial{nullptr};
std::atomic<bool> expire_response_clock{false};

task<elio::coro::cancel_result> controlled_response_clock(std::chrono::nanoseconds remaining,
        elio::coro::cancel_token token, client_stage) {
    if (expire_response_clock.load()) co_return elio::coro::cancel_result::completed;
    co_return co_await elio::time::sleep_for(remaining, std::move(token));
}

struct clock_guard {
    clock_guard() { detail::response_watchdog_wait_for_test.store(controlled_response_clock); }
    ~clock_guard() {
        detail::response_watchdog_wait_for_test.store(nullptr);
        expire_response_clock.store(false);
    }
};

task<client_result<connection>> controlled_dial(const detail::route_plan&,
        std::chrono::nanoseconds, elio::coro::cancel_token token) {
    auto& observation = *observing_dial.load();
    const auto index = observation.calls.fetch_add(1);
    if (index == 0 && observation.pause_first) {
        observation.entered.set();
        if (co_await observation.proceed.wait(token) == elio::coro::cancel_result::cancelled)
            co_return detail::make_client_error(ECANCELED, client_stage::acquire);
    }
    if (index >= observation.connections.size())
        co_return detail::make_client_error(EAGAIN, client_stage::connect);
    co_return std::move(observation.connections[index]);
}

struct dial_guard {
    explicit dial_guard(dial_observation& observation) {
        observing_dial.store(&observation);
        detail::route_connect_for_test.store(controlled_dial);
    }
    ~dial_guard() {
        detail::route_connect_for_test.store(nullptr);
        observing_dial.store(nullptr);
    }
};

struct disposition_observation {
    std::mutex mutex;
    std::vector<std::pair<int, bool>> events;
    std::latch returning{1};
    std::latch proceed{1};
};

std::atomic<disposition_observation*> observing_disposition{nullptr};

void record_disposition(int fd, bool returned) {
    auto& observation = *observing_disposition.load();
    std::lock_guard lock(observation.mutex);
    observation.events.emplace_back(fd, returned);
}

void block_return() {
    auto& observation = *observing_disposition.load();
    observation.returning.count_down();
    observation.proceed.wait();
}

struct disposition_guard {
    explicit disposition_guard(disposition_observation& observation) {
        observing_disposition.store(&observation);
        detail::lease_disposition_for_test.store(record_disposition);
    }
    ~disposition_guard() {
        detail::lease_before_return_for_test.store(nullptr);
        detail::lease_disposition_for_test.store(nullptr);
        observing_disposition.store(nullptr);
    }
};

task<bool> read_headers(elio::net::tcp_stream& peer, elio::coro::cancel_token token) {
    std::string result;
    std::array<char, 512> bytes{};
    while (result.find("\r\n\r\n") == std::string::npos && result.size() < 8192) {
        const auto read = co_await peer.read(bytes.data(), bytes.size(), token);
        if (read.result <= 0) co_return false;
        result.append(bytes.data(), static_cast<size_t>(read.result));
    }
    co_return result.find("\r\n\r\n") != std::string::npos;
}

struct backend_guard {
    elio::io::io_context::backend_type old;
    explicit backend_guard(elio::io::io_context::backend_type selected)
        : old(elio::runtime::detail::worker_io_backend_for_test.exchange(selected)) {}
    ~backend_guard() { elio::runtime::detail::worker_io_backend_for_test.store(old); }
};

struct clear_observation {
    transport& owner;
    std::optional<lease> acquired;
};
std::atomic<clear_observation*> observing_clear{nullptr};

void acquire_visible_generation() {
    auto& observation = *observing_clear.load();
    auto result = immediate(observation.owner.acquire_lease_for_test(
        *url::parse("http://origin.invalid/")));
    if (auto* acquired = std::get_if<lease>(&result))
        observation.acquired.emplace(std::move(*acquired));
}

struct clear_guard {
    explicit clear_guard(clear_observation& observation) {
        observing_clear.store(&observation);
        detail::transport_clear_visible_for_test.store(acquire_visible_generation);
    }
    ~clear_guard() {
        detail::transport_clear_visible_for_test.store(nullptr);
        observing_clear.store(nullptr);
    }
};

} // namespace

TEST_CASE("HTTP lease moves preserve the original owner and settle each disposition once",
          "[http][lease][issue-1247]") {
    socket_pair one;
    socket_pair two;
    const int one_fd = one.client.fd();
    const int two_fd = two.client.fd();
    dial_observation dials;
    dials.connections.emplace_back(std::move(one.client));
    dials.connections.emplace_back(std::move(two.client));
    dial_guard dial(dials);
    disposition_observation dispositions;
    disposition_guard observe(dispositions);
    auto first_owner = std::make_shared<transport>();
    auto second_owner = std::make_shared<transport>();
    const auto target = *url::parse("http://origin.invalid/");
    const auto original_key = first_owner->route_plan_for_test(target).key();
    {
        auto first_result = immediate(first_owner->acquire_lease_for_test(target));
        auto second_result = immediate(second_owner->acquire_lease_for_test(target));
        REQUIRE(std::holds_alternative<lease>(first_result));
        REQUIRE(std::holds_alternative<lease>(second_result));
        auto first = std::move(std::get<lease>(first_result));
        auto second = std::move(std::get<lease>(second_result));
        lease moved(std::move(first));
        first.retire();
        second = std::move(moved);
        moved.retire();
        REQUIRE(second.plan().key() == original_key);
        REQUIRE(second.stream().fd() == one_fd);
        REQUIRE(dispositions.events == std::vector<std::pair<int, bool>>{{two_fd, false}});
        transport::return_lease_for_test(second);
        transport::return_lease_for_test(second);
        second.retire();
        auto reused = immediate(first_owner->acquire_lease_for_test(target));
        REQUIRE(std::holds_alternative<lease>(reused));
        REQUIRE(std::get<lease>(reused).stream().fd() == one_fd);
        auto other = immediate(second_owner->acquire_lease_for_test(target));
        REQUIRE(std::holds_alternative<client_error>(other));
        REQUIRE(std::get<client_error>(other).code.value() == EAGAIN);
    }
    REQUIRE(dispositions.events == std::vector<std::pair<int, bool>>{
        {two_fd, false}, {one_fd, true}, {one_fd, false}});
    REQUIRE(::fcntl(one_fd, F_GETFD) == -1);
    REQUIRE(::fcntl(two_fd, F_GETFD) == -1);
}

TEST_CASE("HTTP default lease destruction aborts without draining and retains its state owner",
          "[http][lease][issue-1247]") {
    socket_pair pair;
    const int descriptor = pair.client.fd();
    dial_observation dials;
    dials.connections.emplace_back(std::move(pair.client));
    dial_guard dial(dials);
    disposition_observation dispositions;
    disposition_guard observe(dispositions);
    std::weak_ptr<void> retained;
    {
        auto owner = std::make_shared<transport>();
        retained = owner->state_owner_for_test();
        auto result = immediate(owner->acquire_lease_for_test(*url::parse("https://origin.invalid/")));
        REQUIRE(std::holds_alternative<lease>(result));
        auto active = std::move(std::get<lease>(result));
        owner.reset();
        REQUIRE_FALSE(retained.expired());
        REQUIRE(::fcntl(descriptor, F_GETFD) != -1);
        REQUIRE(dispositions.events.empty());
    }
    REQUIRE(retained.expired());
    REQUIRE(dispositions.events == std::vector<std::pair<int, bool>>{{descriptor, false}});
    REQUIRE(::fcntl(descriptor, F_GETFD) == -1);
    char byte{};
    REQUIRE(::recv(pair.peer.fd(), &byte, 1, MSG_DONTWAIT) == 0);
}

TEST_CASE("HTTP clear and shutdown linearize with a returning lease without early I/O release",
          "[http][lease][issue-1247][issue-1248]") {
    const bool shutdown = GENERATE(false, true);
    const bool bounded = GENERATE(false, true);
    socket_pair pair;
    const int descriptor = pair.client.fd();
    dial_observation dials;
    dials.connections.emplace_back(std::move(pair.client));
    dial_guard dial(dials);
    disposition_observation dispositions;
    disposition_guard observe(dispositions);
    transport_config policy;
    if (bounded) policy.limits = pool_limits{};
    auto owner = std::make_shared<transport>(policy);
    auto result = immediate(owner->acquire_lease_for_test(*url::parse("http://origin.invalid/")));
    REQUIRE(std::holds_alternative<lease>(result));
    auto active = std::move(std::get<lease>(result));
    detail::lease_before_return_for_test.store(block_return);
    std::thread returner([&] { transport::return_lease_for_test(active); });
    dispositions.returning.wait();
    std::optional<task<elio::coro::cancel_result>> waiting;
    bool stopped_early = false;
    if (shutdown) {
        waiting.emplace(owner->shutdown());
        auto handle = elio::coro::detail::task_access::handle(*waiting);
        handle.resume();
        stopped_early = handle.done();
    } else {
        owner->clear();
    }
    const bool descriptor_alive = ::fcntl(descriptor, F_GETFD) != -1;
    dispositions.proceed.count_down();
    returner.join();
    detail::lease_before_return_for_test.store(nullptr);
    REQUIRE_FALSE(stopped_early);
    REQUIRE(descriptor_alive);
    REQUIRE(dispositions.events == std::vector<std::pair<int, bool>>{{descriptor, false}});
    REQUIRE(::fcntl(descriptor, F_GETFD) == -1);
    if (bounded) REQUIRE(owner->admission_counters_for_test().live == 0);
    active.retire();
    if (waiting) {
        REQUIRE(elio::coro::detail::task_access::handle(*waiting).done());
        REQUIRE(waiting->await_resume() == elio::coro::cancel_result::completed);
    }
}

TEST_CASE("HTTP leases acquired across clear or shutdown retain admission until disposition",
          "[http][lease][issue-1247]") {
    const bool shutdown = GENERATE(false, true);
    socket_pair pair;
    const int descriptor = pair.client.fd();
    dial_observation dials;
    dials.connections.emplace_back(std::move(pair.client));
    dials.pause_first = true;
    dial_guard dial(dials);
    disposition_observation dispositions;
    disposition_guard observe(dispositions);
    auto owner = std::make_shared<transport>();
    auto acquisition = owner->acquire_lease_for_test(*url::parse("http://origin.invalid/"));
    auto handle = elio::coro::detail::task_access::handle(acquisition);
    handle.resume();
    REQUIRE(dials.entered.is_set());
    REQUIRE_FALSE(handle.done());
    std::optional<task<elio::coro::cancel_result>> waiting;
    if (shutdown) {
        waiting.emplace(owner->shutdown());
        auto shutdown_handle = elio::coro::detail::task_access::handle(*waiting);
        shutdown_handle.resume();
        REQUIRE_FALSE(shutdown_handle.done());
    } else {
        owner->clear();
    }
    REQUIRE(::fcntl(descriptor, F_GETFD) != -1);
    dials.proceed.set();
    REQUIRE(handle.done());
    auto acquired = acquisition.await_resume();
    REQUIRE(std::holds_alternative<lease>(acquired));
    auto active = std::move(std::get<lease>(acquired));
    if (waiting) REQUIRE_FALSE(elio::coro::detail::task_access::handle(*waiting).done());
    transport::return_lease_for_test(active);
    REQUIRE(dispositions.events == std::vector<std::pair<int, bool>>{{descriptor, false}});
    REQUIRE(::fcntl(descriptor, F_GETFD) == -1);
    if (waiting) {
        REQUIRE(elio::coro::detail::task_access::handle(*waiting).done());
        REQUIRE(waiting->await_resume() == elio::coro::cancel_result::completed);
    }
}

TEST_CASE("HTTP full or expired idle buckets cannot duplicate lease disposition",
          "[http][lease][issue-1247]") {
    const size_t limit = GENERATE(0, 1);
    const auto idle_timeout = GENERATE(std::chrono::seconds(0), std::chrono::seconds(60));
    socket_pair one;
    socket_pair two;
    const int one_fd = one.client.fd();
    const int two_fd = two.client.fd();
    dial_observation dials;
    dials.connections.emplace_back(std::move(one.client));
    dials.connections.emplace_back(std::move(two.client));
    dial_guard dial(dials);
    disposition_observation dispositions;
    disposition_guard observe(dispositions);
    transport_config policy;
    policy.max_connections_per_host = limit;
    policy.pool_idle_timeout = idle_timeout;
    auto owner = std::make_shared<transport>(policy);
    const auto target = *url::parse("http://origin.invalid/");
    auto first_result = immediate(owner->acquire_lease_for_test(target));
    auto second_result = immediate(owner->acquire_lease_for_test(target));
    REQUIRE(std::holds_alternative<lease>(first_result));
    REQUIRE(std::holds_alternative<lease>(second_result));
    auto first = std::move(std::get<lease>(first_result));
    auto second = std::move(std::get<lease>(second_result));
    transport::return_lease_for_test(first);
    transport::return_lease_for_test(second);
    first.retire();
    second.retire();
    REQUIRE(dispositions.events == std::vector<std::pair<int, bool>>{
        {one_fd, limit != 0}, {two_fd, false}});
    auto next = immediate(owner->acquire_lease_for_test(target));
    const bool reused = limit != 0 && idle_timeout.count() != 0;
    REQUIRE(std::holds_alternative<lease>(next) == reused);
    if (reused) {
        auto& active = std::get<lease>(next);
        REQUIRE(active.stream().fd() == one_fd);
        active.retire();
        REQUIRE(dispositions.events.size() == 3);
    } else {
        REQUIRE(std::get<client_error>(next).code.value() == EAGAIN);
        REQUIRE(dispositions.events.size() == 2);
    }
    REQUIRE(::fcntl(one_fd, F_GETFD) == -1);
    REQUIRE(::fcntl(two_fd, F_GETFD) == -1);
}

TEST_CASE("HTTP clear removes old idle entries before exposing a new generation",
          "[http][lease][clear-acquire][issue-1247]") {
    socket_pair old;
    socket_pair fresh;
    const int old_fd = old.client.fd();
    const int fresh_fd = fresh.client.fd();
    dial_observation dials;
    dials.connections.emplace_back(std::move(old.client));
    dials.connections.emplace_back(std::move(fresh.client));
    dial_guard dial(dials);
    disposition_observation dispositions;
    disposition_guard observe(dispositions);
    transport owner;
    auto warm = immediate(owner.acquire_lease_for_test(*url::parse("http://origin.invalid/")));
    REQUIRE(std::holds_alternative<lease>(warm));
    transport::return_lease_for_test(std::get<lease>(warm));
    clear_observation observation{owner, std::nullopt};
    {
        clear_guard at_publication(observation);
        owner.clear();
    }
    REQUIRE(observation.acquired);
    REQUIRE(observation.acquired->stream().fd() == fresh_fd);
    REQUIRE(dials.calls.load() == 2);
    REQUIRE(::fcntl(old_fd, F_GETFD) == -1);
    transport::return_lease_for_test(*observation.acquired);
    REQUIRE(dispositions.events == std::vector<std::pair<int, bool>>{
        {old_fd, true}, {fresh_fd, true}});
}

TEST_CASE("HTTP Transport clear rejects returns from pre-clear exchanges",
          "[http][lease][issue-1247]") {
    const auto backend = GENERATE(elio::io::io_context::backend_type::epoll,
                                 elio::io::io_context::backend_type::io_uring);
#if ELIO_HAS_IO_URING
    if (backend == elio::io::io_context::backend_type::io_uring &&
        !elio::io::io_uring_backend::is_available()) SKIP("io_uring unavailable");
#else
    if (backend == elio::io::io_context::backend_type::io_uring) SKIP("io_uring not compiled");
#endif
    backend_guard restore(backend);
    socket_pair old;
    socket_pair fresh;
    dial_observation observation;
    observation.connections.emplace_back(std::move(old.client));
    observation.connections.emplace_back(std::move(fresh.client));
    dial_guard dial(observation);
    auto owner = std::make_shared<transport>();
    client requestor(owner);
    elio::sync::event request_seen;
    elio::sync::event cleared;
    elio::coro::cancel_source stop;
    elio::runtime::scheduler scheduler(2);
    scheduler.start();
    std::atomic<bool> finished{false};
    std::array<int, 2> statuses{};
    const std::string reply = "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n"
        "Connection: keep-alive\r\n\r\nok";
    auto old_server = scheduler.go_joinable([&]() -> task<void> {
        if (!(co_await read_headers(old.peer, stop.get_token()))) co_return;
        request_seen.set();
        if (co_await cleared.wait(stop.get_token()) == elio::coro::cancel_result::cancelled)
            co_return;
        co_await old.peer.write_exactly(reply, stop.get_token());
        // Also answer a wrongly reused connection so the regression fails on
        // dial count instead of merely hanging until a timeout.
        if (co_await read_headers(old.peer, stop.get_token()))
            co_await old.peer.write_exactly(reply, stop.get_token());
    });
    auto fresh_server = scheduler.go_joinable([&]() -> task<void> {
        if (co_await read_headers(fresh.peer, stop.get_token()))
            co_await fresh.peer.write_exactly(reply, stop.get_token());
    });
    auto controller = scheduler.go_joinable([&]() -> task<void> {
        if (co_await request_seen.wait(stop.get_token()) == elio::coro::cancel_result::cancelled)
            co_return;
        owner->clear();
        cleared.set();
    });
    auto requests = scheduler.go_joinable([&]() -> task<void> {
        for (auto& status : statuses) {
            auto result = co_await requestor.get_result("http://origin.invalid/", stop.get_token());
            if (auto* response = std::get_if<elio::http::response>(&result))
                status = response->status_code();
        }
        finished.store(true, std::memory_order_release);
    });
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
    while (!finished.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < deadline)
        std::this_thread::yield();
    stop.cancel();
    cleared.set();
    requests.wait_destroyed();
    controller.wait_destroyed();
    old_server.wait_destroyed();
    fresh_server.wait_destroyed();
    scheduler.shutdown();
    requests.await_resume();
    controller.await_resume();
    old_server.await_resume();
    fresh_server.await_resume();
    REQUIRE(finished.load());
    REQUIRE(statuses == std::array<int, 2>{200, 200});
    REQUIRE(observation.calls.load() == 2);
}

TEST_CASE("HTTP exchange alone proves lease reuse and failures retire once",
          "[http][lease][issue-1247][issue-1248]") {
    const auto backend = GENERATE(elio::io::io_context::backend_type::epoll,
                                 elio::io::io_context::backend_type::io_uring);
#if ELIO_HAS_IO_URING
    if (backend == elio::io::io_context::backend_type::io_uring &&
        !elio::io::io_uring_backend::is_available()) SKIP("io_uring unavailable");
#else
    if (backend == elio::io::io_context::backend_type::io_uring) SKIP("io_uring not compiled");
#endif
    const int scenario = GENERATE(0, 1, 2, 3, 4, 5, 6, 7, 8);
    const bool bounded = GENERATE(false, true);
    INFO("scenario " << scenario << " backend " << static_cast<int>(backend));
    const std::string reply = "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n"
        "Connection: keep-alive\r\n\r\nok";
    std::string first_reply;
    bool close_write = false;
    bool first_success = true;
    int expected_error = 0;
    switch (scenario) {
    case 0: first_reply = reply; break;
    case 1:
        first_reply = "HTTP/1.1 200 OK\r\nContent-Length: 2\r\nContent-Length: 3\r\n\r\nok";
        first_success = false; expected_error = EBADMSG; break;
    case 2:
        first_reply = "HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nok";
        first_success = false; close_write = true; expected_error = EBADMSG; break;
    case 3:
        first_reply = "HTTP/1.1 200 OK\r\nConnection: close\r\n\r\nok";
        close_write = true; break;
    case 4: first_reply = reply + "unexpected suffix"; break;
    case 5:
        first_reply = "HTTP/1.1 200 OK\r\nContent-Length: 2\r\nConnection: close\r\n\r\nok";
        break;
    case 6: first_success = false; expected_error = ETIMEDOUT; break;
    case 7: first_success = false; expected_error = ECANCELED; break;
    case 8:
        first_reply = "HTTP/1.1 200 OK\r\nContent-Length: 3\r\n\r\nabc";
        first_success = false; expected_error = EMSGSIZE; break;
    }
    backend_guard restore(backend);
    socket_pair old;
    socket_pair fresh;
    const int old_fd = old.client.fd();
    const int fresh_fd = fresh.client.fd();
    dial_observation dials;
    dials.connections.emplace_back(std::move(old.client));
    dials.connections.emplace_back(std::move(fresh.client));
    dial_guard dial(dials);
    disposition_observation dispositions;
    disposition_guard observe(dispositions);
    clock_guard clock;
    expire_response_clock.store(scenario == 6);
    transport_config config;
    if (bounded) {
        config.limits = pool_limits{};
        config.limits->max_live_total = 1;
        config.limits->max_dials_total = 1;
    }
    auto owner = std::make_shared<transport>(config);
    client_config policy;
    if (scenario == 8) policy.max_response_size = 2;
    client requestor(owner, policy);
    elio::coro::cancel_source stop;
    elio::coro::cancel_source cancel_first;
    elio::runtime::scheduler scheduler(2);
    scheduler.start();
    std::atomic<bool> finished{false};
    bool observed_first_success = false;
    int first_error = 0;
    int second_status = 0;
    auto old_server = scheduler.go_joinable([&]() -> task<void> {
        if (!(co_await read_headers(old.peer, stop.get_token()))) co_return;
        if (scenario == 7) cancel_first.cancel();
        if (!first_reply.empty())
            co_await old.peer.write_exactly(first_reply, stop.get_token());
        if (close_write) ::shutdown(old.peer.fd(), SHUT_WR);
        if (co_await read_headers(old.peer, stop.get_token()))
            co_await old.peer.write_exactly(reply, stop.get_token());
    });
    auto fresh_server = scheduler.go_joinable([&]() -> task<void> {
        if (co_await read_headers(fresh.peer, stop.get_token()))
            co_await fresh.peer.write_exactly(reply, stop.get_token());
    });
    auto requests = scheduler.go_joinable([&]() -> task<void> {
        auto first = co_await requestor.get_result("http://origin.invalid/", cancel_first.get_token());
        observed_first_success = std::holds_alternative<response>(first);
        if (auto* error = std::get_if<client_error>(&first)) first_error = error->code.value();
        expire_response_clock.store(false);
        auto second = co_await requestor.get_result("http://origin.invalid/", stop.get_token());
        if (auto* response = std::get_if<elio::http::response>(&second))
            second_status = response->status_code();
        finished.store(true, std::memory_order_release);
    });
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
    while (!finished.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < deadline)
        std::this_thread::yield();
    cancel_first.cancel();
    stop.cancel();
    requests.wait_destroyed();
    old_server.wait_destroyed();
    fresh_server.wait_destroyed();
    scheduler.shutdown();
    requests.await_resume();
    old_server.await_resume();
    fresh_server.await_resume();
    REQUIRE(finished.load());
    REQUIRE(observed_first_success == first_success);
    REQUIRE(first_error == expected_error);
    REQUIRE(second_status == 200);
    REQUIRE(dials.calls.load() == (scenario == 0 ? 1 : 2));
    REQUIRE(dispositions.events == std::vector<std::pair<int, bool>>{
        {old_fd, scenario == 0}, {scenario == 0 ? old_fd : fresh_fd, true}});
    if (bounded) {
        REQUIRE(owner->admission_counters_for_test().live == 1);
        REQUIRE(owner->admission_counters_for_test().idle == 1);
        REQUIRE(owner->admission_counters_for_test().dialing == 0);
    }
}
