#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <elio/http/http_client.hpp>

#include <array>
#include <atomic>
#include <cerrno>
#include <fcntl.h>
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
