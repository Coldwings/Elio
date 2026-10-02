#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <elio/http/http_client.hpp>
#include <elio/sync/event.hpp>

#include <array>
#include <atomic>
#include <cerrno>
#include <exception>
#include <fcntl.h>
#include <new>
#include <string>

using namespace elio::http;
using elio::coro::task;

namespace {
using backend = elio::io::io_context::backend_type;

struct close_observation {
    int fd = -1;
    bool returned = false;
    bool closed = false;
};
std::atomic<close_observation*> close_observed{nullptr};

void observe_close(int fd, bool returned) {
    auto& observed = *close_observed.load();
    observed.fd = fd;
    observed.returned = returned;
    if (!returned) observed.closed = ::fcntl(fd, F_GETFD) < 0 && errno == EBADF;
}

struct close_hooks {
    backend previous;
#if ELIO_HAS_IO_URING
    bool previous_submission;
#endif
    close_hooks(backend selected, close_observation& observed)
        : previous(elio::runtime::detail::worker_io_backend_for_test.exchange(selected))
#if ELIO_HAS_IO_URING
        , previous_submission(elio::io::detail::defer_destructor_close_submission_for_test.exchange(true))
#endif
    {
        close_observed.store(&observed);
        detail::lease_disposition_for_test.store(observe_close);
    }
    ~close_hooks() {
        detail::lease_disposition_for_test.store(nullptr);
        close_observed.store(nullptr);
        elio::runtime::detail::worker_io_backend_for_test.store(previous);
#if ELIO_HAS_IO_URING
        elio::io::detail::defer_destructor_close_submission_for_test.store(previous_submission);
#endif
    }
};

task<void> serve_close(elio::net::tcp_listener& listener, bool idle,
        bool& accepted, elio::coro::cancel_token token) {
    auto peer = co_await listener.accept(token);
    if (!peer) co_return;
    accepted = true;
    std::string request;
    std::array<char, 1024> bytes{};
    while (request.find("\r\n\r\n") == std::string::npos) {
        auto read = co_await peer->read(bytes.data(), bytes.size(), token);
        if (read.result <= 0) co_return;
        request.append(bytes.data(), static_cast<size_t>(read.result));
    }
    const std::string reply = "HTTP/1.1 200 OK\r\nContent-Length: 2\r\n" +
        std::string(idle ? "" : "Connection: close\r\n") + "\r\nok";
    (void)co_await peer->write_exactly(reply, token);
    (void)co_await peer->read(bytes.data(), bytes.size(), token);
}

std::atomic<bool> fail_before_prepare{false};

void observe_connect_prepare(int fd) {
    close_observed.load()->fd = fd;
    if (fail_before_prepare.load()) throw std::bad_alloc();
}

struct setup_failure_hooks {
    bool previous_prepare_failure;
    bool previous_linger_failure;
    void(*previous_prepare_hook)(int);
    explicit setup_failure_hooks(bool before_prepare)
        : previous_prepare_failure(fail_before_prepare.exchange(before_prepare))
        , previous_linger_failure(
            elio::net::detail::fail_root_linger_configuration_for_test.exchange(!before_prepare))
        , previous_prepare_hook(
            elio::net::detail::root_connect_before_prepare_for_test.exchange(observe_connect_prepare)) {}
    ~setup_failure_hooks() {
        elio::net::detail::root_connect_before_prepare_for_test.store(previous_prepare_hook);
        elio::net::detail::fail_root_linger_configuration_for_test.store(previous_linger_failure);
        fail_before_prepare.store(previous_prepare_failure);
    }
};
} // namespace

TEST_CASE("Plain Transport closes physical TCP roots before retiring capacity",
          "[http][lease][admission][root-close][issue-1276]") {
    const auto selected = GENERATE(backend::epoll, backend::io_uring);
    const auto finite = GENERATE(false, true);
    const auto action = GENERATE(0, 1, 2);
    CAPTURE(selected, finite, action);
#if ELIO_HAS_IO_URING
    if (selected == backend::io_uring && !elio::io::io_uring_backend::is_available())
        SKIP("io_uring unavailable on this host");
#else
    if (selected == backend::io_uring) SKIP("io_uring support is not compiled");
#endif
    close_observation observed;
    close_hooks hooks(selected, observed);
    auto listener = elio::net::tcp_listener::bind(elio::net::ipv4_address("127.0.0.1", 0));
    REQUIRE(listener);
    const auto target = "http://127.0.0.1:" + std::to_string(listener->local_address().port()) + "/";
    transport_config config;
    if (finite) config.limits = pool_limits{};
    auto owner = std::make_shared<transport>(config);
    client_config policy;
    policy.read_timeout = std::chrono::seconds(5);
    client agent(owner, policy);
    elio::coro::cancel_source stop;
    bool accepted = false;
    size_t operations = 1;
    size_t live = 1;
    elio::runtime::scheduler scheduler(1);
    scheduler.start();
    auto server = scheduler.go_joinable(serve_close(*listener, action != 0, accepted, stop.get_token()));
    auto controlled = scheduler.go_joinable([&]() -> task<client_result<response>> {
        auto result = co_await agent.get_result(target);
        if (action == 1) owner->clear();
        if (action == 2) (void)co_await owner->shutdown();
        if (action != 0) observed.closed = ::fcntl(observed.fd, F_GETFD) < 0 && errno == EBADF;
        operations = owner->active_operations_for_test();
        if (finite) live = owner->admission_counters_for_test().live;
        co_return result;
    });
    controlled.wait_destroyed();
    stop.cancel();
    server.wait_destroyed();
    REQUIRE(scheduler.shutdown(std::chrono::seconds(10)));
    const auto result = controlled.await_resume();
    server.await_resume();
    REQUIRE(accepted);
    if (const auto* error = std::get_if<client_error>(&result)) {
        CAPTURE(error->code.value(), error->stage);
        REQUIRE_FALSE(error);
    }
    REQUIRE(std::holds_alternative<response>(result));
    CHECK(std::get<response>(result).body() == "ok");
    CHECK(observed.fd >= 0);
    CHECK(observed.returned == (action != 0));
    CHECK(observed.closed);
    CHECK(operations == 0);
    if (finite) CHECK(live == 0);
}

TEST_CASE("Private TCP connect closes before reporting setup exceptions",
          "[http][root-close][issue-1276][setup-failure]") {
    const auto selected = GENERATE(backend::epoll, backend::io_uring);
    const auto before_prepare = GENERATE(false, true);
    CAPTURE(selected, before_prepare);
#if ELIO_HAS_IO_URING
    if (selected == backend::io_uring && !elio::io::io_uring_backend::is_available())
        SKIP("io_uring unavailable on this host");
#else
    if (selected == backend::io_uring) SKIP("io_uring support is not compiled");
#endif
    close_observation observed;
    close_hooks hooks(selected, observed);
    setup_failure_hooks failure_hooks(before_prepare);
    auto listener = elio::net::tcp_listener::bind(elio::net::ipv4_address("127.0.0.1", 0));
    REQUIRE(listener);
    bool expected_failure = false;
    elio::runtime::scheduler scheduler(1);
    scheduler.start();
    auto connected = scheduler.go_joinable([&]() -> task<void> {
        try {
            (void)co_await elio::net::detail::tcp_retirement_access::connect(
                listener->local_address(), {}, true);
        } catch (const std::bad_alloc&) {
            expected_failure = before_prepare;
        } catch (const std::system_error& error) {
            expected_failure = !before_prepare && error.code().value() == ENOMEM;
        }
        // Same-worker observation, before another poll can submit/drain close.
        observed.closed = ::fcntl(observed.fd, F_GETFD) < 0 && errno == EBADF;
    });
    connected.wait_destroyed();
    const auto stopped = scheduler.shutdown(std::chrono::seconds(10));
    connected.await_resume();
    REQUIRE(stopped);
    CHECK(observed.fd >= 0);
    CHECK(expected_failure);
    CHECK(observed.closed);
}

TEST_CASE("Transport setup exceptions release accounting and join the watchdog",
          "[http][root-close][issue-1276][setup-failure]") {
    const auto selected = GENERATE(backend::epoll, backend::io_uring);
    const auto before_prepare = GENERATE(false, true);
    const auto finite = GENERATE(false, true);
    const auto secure = GENERATE(false, true);
    CAPTURE(selected, before_prepare, finite, secure);
#if ELIO_HAS_IO_URING
    if (selected == backend::io_uring && !elio::io::io_uring_backend::is_available())
        SKIP("io_uring unavailable on this host");
#else
    if (selected == backend::io_uring) SKIP("io_uring support is not compiled");
#endif
    close_observation observed;
    close_hooks hooks(selected, observed);
    setup_failure_hooks failure_hooks(before_prepare);
    auto listener = elio::net::tcp_listener::bind(elio::net::ipv4_address("127.0.0.1", 0));
    REQUIRE(listener);
    const auto target = std::string(secure ? "https://" : "http://") +
        "127.0.0.1:" + std::to_string(listener->local_address().port()) + "/";
    transport_config config;
    if (finite) config.limits = pool_limits{};
    auto owner = std::make_shared<transport>(config);
    client_config policy;
    // An abandoned watchdog would outlive the bounded scheduler shutdown.
    policy.connect_timeout = std::chrono::hours(1);
    client agent(owner, policy);
    bool expected_failure = false;
    std::exception_ptr unexpected;
    size_t operations = 1;
    size_t live = 1;
    elio::runtime::scheduler scheduler(1);
    scheduler.start();
    auto controlled = scheduler.go_joinable([&]() -> task<void> {
        try {
            (void)co_await agent.get_result(target);
        } catch (const std::bad_alloc&) {
            expected_failure = before_prepare;
        } catch (const std::system_error& error) {
            expected_failure = !before_prepare && error.code().value() == ENOMEM;
        } catch (...) {
            unexpected = std::current_exception();
        }
        observed.closed = ::fcntl(observed.fd, F_GETFD) < 0 && errno == EBADF;
        operations = owner->active_operations_for_test();
        if (finite) live = owner->admission_counters_for_test().live;
    });
    controlled.wait_destroyed();
    const auto stopped = scheduler.shutdown(std::chrono::seconds(10));
    controlled.await_resume();
    if (unexpected) std::rethrow_exception(unexpected);
    REQUIRE(stopped);
    CHECK(observed.fd >= 0);
    CHECK(expected_failure);
    CHECK(observed.closed);
    CHECK(operations == 0);
    if (finite) CHECK(live == 0);
}

TEST_CASE("Settled TCP root moves retain the non-lingering close policy",
          "[http][root-close][issue-1276]") {
    const auto selected = GENERATE(backend::epoll, backend::io_uring);
#if ELIO_HAS_IO_URING
    if (selected == backend::io_uring && !elio::io::io_uring_backend::is_available())
        SKIP("io_uring unavailable on this host");
#else
    if (selected == backend::io_uring) SKIP("io_uring support is not compiled");
#endif
    close_observation observed;
    close_hooks hooks(selected, observed);
    bool created = false;
    bool linger_enabled = false;
    bool linger_disabled = false;
    bool moved_empty = false;
    elio::runtime::scheduler scheduler(1);
    scheduler.start();
    auto retired = scheduler.go_joinable([&]() -> task<void> {
        const int fd = ::socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
        if (fd < 0) co_return;
        created = true;
        observed.fd = fd;
        {
            elio::net::tcp_stream original(fd);
            const ::linger enabled{1, 5};
            linger_enabled = ::setsockopt(fd, SOL_SOCKET, SO_LINGER, &enabled, sizeof(enabled)) == 0;
            elio::net::detail::tcp_retirement_access::mark_settled_root(original);
            ::linger option{};
            socklen_t length = sizeof(option);
            linger_disabled = ::getsockopt(fd, SOL_SOCKET, SO_LINGER, &option, &length) == 0 &&
                option.l_onoff == 0;
            elio::net::tcp_stream moved(std::move(original));
            elio::net::tcp_stream assigned(-1);
            assigned = std::move(moved);
            moved_empty = original.fd() == -1 && moved.fd() == -1;
        }
        observed.closed = ::fcntl(fd, F_GETFD) < 0 && errno == EBADF;
    });
    retired.wait_destroyed();
    const auto stopped = scheduler.shutdown(std::chrono::seconds(10));
    retired.await_resume();
    REQUIRE(stopped);
    CHECK(created);
    CHECK(linger_enabled);
    CHECK(linger_disabled);
    CHECK(moved_empty);
    CHECK(observed.closed);
}

namespace {
struct watchdog_observation {
    elio::sync::event cancelled;
    bool saw_cancellation = false;
    bool fail_cancellation_callback = false;
};
std::atomic<watchdog_observation*> watchdog_observed{nullptr};

task<elio::coro::cancel_result> fail_watchdog_cleanup(
        std::chrono::steady_clock::time_point, elio::coro::cancel_token token) {
    auto& observed = *watchdog_observed.load();
    auto failure_registration = token.on_cancel([&observed]() {
        if (observed.fail_cancellation_callback)
            throw std::runtime_error("watchdog cancellation failure");
    });
    const auto result = co_await observed.cancelled.wait(token);
    observed.saw_cancellation = result == elio::coro::cancel_result::cancelled;
    throw std::runtime_error("watchdog cleanup failure");
}

struct watchdog_failure_hooks {
    detail::setup_watchdog_wait_hook previous_wait;
    watchdog_observation* previous_observation;
    explicit watchdog_failure_hooks(watchdog_observation& observed)
        : previous_wait(detail::setup_watchdog_wait_for_test.exchange(fail_watchdog_cleanup))
        , previous_observation(watchdog_observed.exchange(&observed)) {}
    ~watchdog_failure_hooks() {
        watchdog_observed.store(previous_observation);
        detail::setup_watchdog_wait_for_test.store(previous_wait);
    }
};
} // namespace

TEST_CASE("Connector preserves setup failure when watchdog cleanup also fails",
          "[http][root-close][issue-1276][watchdog-precedence]") {
    const auto selected = GENERATE(backend::epoll, backend::io_uring);
    const auto phase = GENERATE(0, 1, 2, 3, 4);
    CAPTURE(selected, phase);
#if ELIO_HAS_IO_URING
    if (selected == backend::io_uring && !elio::io::io_uring_backend::is_available())
        SKIP("io_uring unavailable on this host");
#else
    if (selected == backend::io_uring) SKIP("io_uring support is not compiled");
#endif
    close_observation observed;
    close_hooks hooks(selected, observed);
    const bool allocation_failure = phase == 0 || phase == 3;
    setup_failure_hooks failure_hooks(allocation_failure);
    if (phase == 2 || phase == 4)
        elio::net::detail::fail_root_linger_configuration_for_test.store(false);
    watchdog_observation watchdog;
    watchdog.fail_cancellation_callback = phase >= 3;
    watchdog_failure_hooks timer_hooks(watchdog);
    auto listener = elio::net::tcp_listener::bind(elio::net::ipv4_address("127.0.0.1", 0));
    REQUIRE(listener);
    transport_config config;
    config.limits = pool_limits{};
    auto owner = std::make_shared<transport>(config);
    client_config policy;
    policy.connect_timeout = std::chrono::hours(1);
    client agent(owner, policy);
    const auto target = "http://127.0.0.1:" + std::to_string(listener->local_address().port()) + "/";
    bool expected_failure = false;
    std::string cleanup_message;
    std::exception_ptr unexpected;
    elio::runtime::scheduler scheduler(1);
    scheduler.start();
    auto requested = scheduler.go_joinable([&]() -> task<void> {
        try {
            (void)co_await agent.get_result(target);
        } catch (const std::bad_alloc&) {
            expected_failure = allocation_failure;
        } catch (const std::system_error& error) {
            expected_failure = phase == 1 && error.code().value() == ENOMEM;
        } catch (const std::runtime_error& error) {
            cleanup_message = error.what();
            expected_failure = (phase == 2 && cleanup_message == "watchdog cleanup failure") ||
                (phase == 4 && cleanup_message == "watchdog cancellation failure");
        } catch (...) {
            unexpected = std::current_exception();
        }
        observed.closed = ::fcntl(observed.fd, F_GETFD) < 0 && errno == EBADF;
    });
    requested.wait_destroyed();
    const auto stopped = scheduler.shutdown(std::chrono::seconds(10));
    requested.await_resume();
    if (unexpected) std::rethrow_exception(unexpected);
    REQUIRE(stopped);
    CAPTURE(cleanup_message);
    CHECK(expected_failure);
    CHECK(watchdog.saw_cancellation);
    CHECK(observed.fd >= 0);
    CHECK(observed.closed);
    CHECK(owner->active_operations_for_test() == 0);
    CHECK(owner->admission_counters_for_test().live == 0);
}
