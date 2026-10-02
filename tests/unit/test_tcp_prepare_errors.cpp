#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <elio/net/tcp.hpp>
#include <elio/runtime/scheduler.hpp>

#include <atomic>
#include <cerrno>

namespace {
struct prepare_control {
    int error;
    std::atomic<size_t> rejections{0};
};

std::atomic<prepare_control*> active_prepare_control{nullptr};

class rejecting_tcp_backend : public elio::io::io_backend {
public:
    explicit rejecting_tcp_backend(prepare_control& control) : control_(control) {}
    bool prepare(const elio::io::io_request& request) override {
        if (request.op != elio::io::io_op::accept && request.op != elio::io::io_op::poll_write)
            return wake_.prepare(request);
        control_.rejections.fetch_add(1, std::memory_order_relaxed);
        elio::io::detail::set_last_completion_result({-control_.error, 0});
        return false;
    }
    int submit() override { return wake_.submit(); }
    int poll(std::chrono::milliseconds timeout) override { return wake_.poll(timeout); }
    bool has_pending() const noexcept override { return wake_.has_pending(); }
    size_t pending_count() const noexcept override { return wake_.pending_count(); }
    bool cancel(void* data) override { return wake_.cancel(data); }
    void notify() noexcept override { wake_.notify(); }
    void drain_notify() override { wake_.drain_notify(); }
private:
    prepare_control& control_;
    elio::io::epoll_backend wake_;
};

elio::io::io_backend* make_rejecting_tcp_backend(size_t) {
    return new rejecting_tcp_backend(*active_prepare_control.load(std::memory_order_acquire));
}

struct prepare_factory_guard {
    explicit prepare_factory_guard(prepare_control& control) {
        active_prepare_control.store(&control, std::memory_order_release);
        elio::io::detail::worker_backend_factory_for_test.store(
            make_rejecting_tcp_backend, std::memory_order_release);
    }
    ~prepare_factory_guard() {
        elio::io::detail::worker_backend_factory_for_test.store(nullptr, std::memory_order_release);
        active_prepare_control.store(nullptr, std::memory_order_release);
    }
};

elio::coro::task<int> rejected_accept(elio::net::tcp_listener& listener,
        bool cancellable, elio::coro::cancel_token token) {
    auto accepted = cancellable ? co_await listener.accept(token) : co_await listener.accept();
    co_return accepted ? 0 : errno;
}

elio::coro::task<int> rejected_connect(elio::net::socket_address address,
        bool cancellable, elio::coro::cancel_token token) {
    auto connected = cancellable ? co_await elio::net::tcp_connect(address, token)
                                 : co_await elio::net::tcp_connect(address);
    co_return connected ? 0 : errno;
}
} // namespace

TEST_CASE("TCP connect awaitable preserves braced public construction",
          "[net][tcp][compatibility][issue-1276]") {
    const elio::net::socket_address address(elio::net::ipv4_address("127.0.0.1", 80));
    // A private policy constructor must not steal this existing public call.
    elio::net::tcp_connect_awaitable operation(address, {}, {});
    CHECK_FALSE(operation.await_ready());
}

TEST_CASE("TCP listener preserves backend preparation errors and fallback",
          "[net][tcp][prepare][issue-1273]") {
    const auto error = GENERATE(EPERM, ENOSPC, ENOMEM, 0);
    const auto cancellable = GENERATE(false, true);
    CAPTURE(error, cancellable);
    auto listener = elio::net::tcp_listener::bind(elio::net::ipv4_address("127.0.0.1", 0));
    REQUIRE(listener);
    prepare_control control{error};
    prepare_factory_guard factory(control);
    elio::coro::cancel_source stop;
    elio::runtime::scheduler scheduler(1);
    scheduler.start();
    auto operation = scheduler.go_joinable(rejected_accept(*listener, cancellable, stop.get_token()));
    operation.wait_destroyed();
    stop.cancel();
    scheduler.shutdown();
    CHECK(operation.await_resume() == (error ? error : EAGAIN));
    CHECK(control.rejections.load() == 1);
}

TEST_CASE("TCP connect preserves backend preparation errors through socket cleanup",
          "[net][tcp][prepare][issue-1273]") {
    const auto error = GENERATE(EPERM, ENOSPC, ENOMEM, 0);
    const auto cancellable = GENERATE(false, true);
    CAPTURE(error, cancellable);
    auto listener = elio::net::tcp_listener::bind(elio::net::ipv4_address("127.0.0.1", 0));
    REQUIRE(listener);
    prepare_control control{error};
    prepare_factory_guard factory(control);
    elio::coro::cancel_source stop;
    elio::runtime::scheduler scheduler(1);
    scheduler.start();
    auto operation = scheduler.go_joinable(rejected_connect(
        listener->local_address(), cancellable, stop.get_token()));
    operation.wait_destroyed();
    stop.cancel();
    scheduler.shutdown();
    const auto result = operation.await_resume();
    if (control.rejections.load() == 0 && result == 0)
        SKIP("TCP connected immediately without asynchronous preparation");
    CHECK(result == (error ? error : EAGAIN));
    CHECK(control.rejections.load() == 1);
}
