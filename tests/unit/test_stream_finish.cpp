#include <catch2/catch_test_macros.hpp>
#include <elio/net/stream.hpp>
#include "../test_main.cpp"
#include <chrono>
#include <exception>
#include <utility>

namespace {
using elio::net::tcp_stream;
using elio::net::close_scope;
using elio::coro::detail::task_access;

std::pair<tcp_stream, tcp_stream> connected_pair() {
    tcp_stream listener(::socket(AF_INET, SOCK_STREAM, 0));
    REQUIRE(listener.fd() >= 0);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    REQUIRE(::bind(listener.fd(), reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0);
    REQUIRE(::listen(listener.fd(), 1) == 0);
    socklen_t length = sizeof(address);
    REQUIRE(::getsockname(listener.fd(), reinterpret_cast<sockaddr*>(&address), &length) == 0);
    // Connect before wrapping: tcp_stream makes the descriptor nonblocking.
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    REQUIRE(fd >= 0);
    const int connected = ::connect(fd, reinterpret_cast<sockaddr*>(&address), length);
    tcp_stream first(fd);
    REQUIRE(connected == 0);
    tcp_stream second(::accept(listener.fd(), nullptr, nullptr));
    REQUIRE(second.fd() >= 0);
    return {std::move(first), std::move(second)};
}

template<class T>
T immediate(elio::coro::task<T> operation) {
    auto handle = task_access::handle(operation);
    handle.resume();
    const auto deadline = std::chrono::steady_clock::now() + elio::test::scaled_ms(2000);
    while (!handle.done() && std::chrono::steady_clock::now() < deadline)
        elio::io::current_io_context().poll(std::chrono::milliseconds(10));
    // A diagnostic timeout must not destroy a frame still registered for I/O.
    if (!handle.done()) std::terminate();
    return operation.await_resume();
}
}

TEST_CASE("TCP finish_write preserves reverse traffic in both EOF orders", "[net][finish][issue-1217]") {
    for (const bool reverse : {false, true}) {
        auto [left, right] = connected_pair();
        auto& first = reverse ? right : left;
        auto& second = reverse ? left : right;
        const auto ended = immediate(first.finish_write());
        CHECK(ended.scope == close_scope::write_direction);
        CHECK(ended.error == 0);
        CHECK(ended.local_end_flushed);
        CHECK_FALSE(ended.peer_end_observed);
        char byte = 0;
        CHECK(immediate(second.read(&byte, 1)).result == 0);
        REQUIRE(::send(second.fd(), "x", 1, MSG_NOSIGNAL) == 1);
        CHECK(immediate(first.read(&byte, 1)).result == 1);
        CHECK(byte == 'x');
        CHECK(immediate(second.finish_write()).error == 0);
        CHECK(immediate(first.read(&byte, 1)).result == 0);
    }
}

TEST_CASE("TCP finish_write leaves an already parked reader usable", "[net][finish][issue-1217]") {
    auto [local, peer] = connected_pair();
    char byte = 0;
    elio::coro::cancel_source cancel;
    auto read = local.read(&byte, 1, cancel.get_token());
    auto handle = task_access::handle(read);
    handle.resume();
    const bool parked = !handle.done();
    const auto ended = immediate(local.finish_write());
    const auto sent = ::send(peer.fd(), "r", 1, MSG_NOSIGNAL);
    auto& context = elio::io::current_io_context();
    auto deadline = std::chrono::steady_clock::now() + elio::test::scaled_ms(2000);
    while (!handle.done() && std::chrono::steady_clock::now() < deadline)
        context.poll(std::chrono::milliseconds(10));
    if (!handle.done()) {
        cancel.cancel();
        local.shutdown_socket();
        deadline = std::chrono::steady_clock::now() + elio::test::scaled_ms(2000);
        while (!handle.done() && std::chrono::steady_clock::now() < deadline)
            context.poll(std::chrono::milliseconds(10));
    }
    if (!handle.done()) std::terminate();
    CHECK(parked);
    CHECK(ended.error == 0);
    CHECK(sent == 1);
    CHECK(read.await_resume().result == 1);
    CHECK(byte == 'r');
}

TEST_CASE("finish_write cancellation and disconnected results are explicit", "[net][finish][issue-1217]") {
    auto [local, peer] = connected_pair();
    elio::coro::cancel_source cancel;
    cancel.cancel();
    const auto result = immediate(local.finish_write(cancel.get_token()));
    CHECK(result.error == ECANCELED);
    CHECK_FALSE(result.local_end_flushed);
    REQUIRE(::send(local.fd(), "x", 1, MSG_NOSIGNAL) == 1);
    char byte = 0;
    CHECK(immediate(peer.read(&byte, 1)).result == 1);
    tcp_stream disconnected(-1);
    CHECK(immediate(disconnected.finish_write()).error == EBADF);
    elio::net::stream empty;
    CHECK(immediate(empty.finish_write()).error == ENOTCONN);
    elio::net::stream wrapped(std::move(local));
    const auto wrapped_result = immediate(wrapped.finish_write({}, std::chrono::milliseconds(0)));
    CHECK(wrapped_result.scope == close_scope::write_direction);
    CHECK(wrapped_result.local_end_flushed);
    CHECK(wrapped_result.error == 0);
    CHECK(immediate(peer.read(&byte, 1)).result == 0);
}
