#include <catch2/catch_test_macros.hpp>
#include <elio/http/http_tunnel_relay.hpp>
#include <elio/sync/event.hpp>
#include "../test_main.cpp"
#include <atomic>
#include <future>
#include <poll.h>
#include <thread>

namespace {
using namespace elio;
using namespace elio::http;
using backend_type = io::io_context::backend_type;

struct backend_guard {
    backend_type previous;
    explicit backend_guard(backend_type type)
        : previous(runtime::detail::worker_io_backend_for_test.exchange(type)) {}
    ~backend_guard() { runtime::detail::worker_io_backend_for_test.store(previous); }
};

std::pair<net::tcp_stream, net::tcp_stream> tunnel_pair() {
    net::tcp_stream listener(::socket(AF_INET, SOCK_STREAM, 0));
    REQUIRE(listener.fd() >= 0);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    REQUIRE(::bind(listener.fd(), reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0);
    REQUIRE(::listen(listener.fd(), 1) == 0);
    socklen_t size = sizeof(address);
    REQUIRE(::getsockname(listener.fd(), reinterpret_cast<sockaddr*>(&address), &size) == 0);
    const int fd = ::socket(AF_INET, SOCK_STREAM, 0);
    REQUIRE(fd >= 0);
    const int connected = ::connect(fd, reinterpret_cast<sockaddr*>(&address), size);
    net::tcp_stream first(fd);
    REQUIRE(connected == 0);
    net::tcp_stream second(::accept(listener.fd(), nullptr, nullptr));
    REQUIRE(second.fd() >= 0);
    const int small = 4096;
    for (const int socket : {first.fd(), second.fd()}) {
        REQUIRE(::setsockopt(socket, SOL_SOCKET, SO_SNDBUF, &small, sizeof(small)) == 0);
        REQUIRE(::setsockopt(socket, SOL_SOCKET, SO_RCVBUF, &small, sizeof(small)) == 0);
    }
    return {std::move(first), std::move(second)};
}

// Observe real transport calls without changing their completion semantics.
struct observed_tcp {
    net::tcp_stream& stream;
    std::atomic<bool> writing{false};
    coro::task<io::io_result> read(void* data, size_t size, coro::cancel_token token) {
        co_return co_await stream.read(data, size, token);
    }
    coro::task<io::io_result> write(const void* data, size_t size, coro::cancel_token token) {
        writing.store(true);
        struct reset { std::atomic<bool>& flag; ~reset() { flag.store(false); } } guard{writing};
        co_return co_await stream.write(data, size, token);
    }
    coro::task<net::write_finish_result> finish_write(coro::cancel_token token) {
        co_return co_await stream.finish_write(token);
    }
    net::close_scope read_end_scope() const noexcept { return stream.read_end_scope(); }
};

struct exchange_state {
    net::tcp_stream& left_peer;
    net::tcp_stream& right_peer;
    observed_tcp& left;
    observed_tcp& right;
    coro::cancel_source stop;
    sync::event readers, first_eof;
    std::string left_payload, right_payload;
    std::string received_left, received_right;
    size_t left_count = 0, right_count = 0;
    bool left_eof = false, right_eof = false, failed = false;
    bool reverse = false;
    tunnel_result result;
    std::promise<void> done;
    exchange_state(net::tcp_stream& left_peer_value, net::tcp_stream& right_peer_value,
                   observed_tcp& left_value, observed_tcp& right_value)
        : left_peer(left_peer_value), right_peer(right_peer_value),
          left(left_value), right(right_value) {}
};

coro::task<void> peer_writer(exchange_state* state, bool left) {
    auto& peer = left ? state->left_peer : state->right_peer;
    const auto& payload = left ? state->left_payload : state->right_payload;
    const bool first = left != state->reverse;
    const size_t initial = payload.size() - (first ? 0 : 7);
    auto sent = co_await peer.write_exactly(payload.data(), initial, state->stop.get_token());
    if (sent.result < 0) co_return;
    if (!first) {
        const auto released = co_await state->first_eof.wait(state->stop.get_token());
        if (released == coro::cancel_result::cancelled) co_return;
        sent = co_await peer.write_exactly(payload.data() + initial, 7, state->stop.get_token());
        if (sent.result < 0) co_return;
    }
    co_await peer.finish_write(state->stop.get_token());
}

coro::task<void> peer_reader(exchange_state* state, bool left) {
    const auto released = co_await state->readers.wait(state->stop.get_token());
    if (released == coro::cancel_result::cancelled) co_return;
    auto& peer = left ? state->left_peer : state->right_peer;
    auto& received = left ? state->received_left : state->received_right;
    auto& count = left ? state->left_count : state->right_count;
    auto& eof = left ? state->left_eof : state->right_eof;
    char buffer[4096];
    for (;;) {
        const auto read = co_await peer.read(buffer, sizeof(buffer), state->stop.get_token());
        if (read.result < 0) co_return;
        if (!read.result) {
            eof = true;
            // Reader opposite the first writer permits the final reverse tail.
            if (left == state->reverse) state->first_eof.set();
            co_return;
        }
        const auto size = static_cast<size_t>(read.result);
        if (size > received.size() - count) {
            state->stop.cancel();
            co_return;
        }
        std::memcpy(received.data() + count, buffer, size);
        count += size;
    }
}

coro::task<void> run_relay(exchange_state* state) {
    auto client = http::detail::tunnel_stream_access::create(state->left);
    state->result = co_await http::detail::relay_with_stream(
        client, state->right, relay_options{4096}, state->stop.get_token());
}

coro::task<void> run_exchange(exchange_state* state) {
    coro::task_group tasks;
    try {
        tasks.spawn(run_relay, state);
        tasks.spawn(peer_writer, state, true);
        tasks.spawn(peer_writer, state, false);
        tasks.spawn(peer_reader, state, true);
        tasks.spawn(peer_reader, state, false);
    } catch (...) { state->failed = true; state->stop.cancel(); }
    try { co_await tasks.join(); }
    catch (...) { state->failed = true; }
    state->done.set_value();
}

bool unwritable(observed_tcp& stream) {
    pollfd fd{stream.stream.fd(), POLLOUT, 0};
    return stream.writing.load() && ::poll(&fd, 1, 0) == 0;
}

void exercise(backend_type backend, bool cancel, bool reverse) {
    backend_guard selected(backend);
    auto [left_peer, left_socket] = tunnel_pair();
    auto [right_peer, right_socket] = tunnel_pair();
    observed_tcp left{left_socket}, right{right_socket};
    exchange_state state{left_peer, right_peer, left, right};
    state.reverse = reverse;
    constexpr size_t bytes = 256 * 1024;
    state.left_payload.resize(bytes);
    state.right_payload.resize(bytes + 7);
    for (size_t i = 0; i < state.left_payload.size(); ++i) state.left_payload[i] = static_cast<char>(i * 17);
    for (size_t i = 0; i < state.right_payload.size(); ++i) state.right_payload[i] = static_cast<char>(i * 31 + 5);
    state.received_left.resize(state.right_payload.size());
    state.received_right.resize(state.left_payload.size());
    auto done = state.done.get_future();
    runtime::scheduler scheduler(1);
    scheduler.start();
    const bool actual_backend = scheduler.get_worker(0)->io_context().get_backend_type() == backend;
    scheduler.go(run_exchange(&state));
    const auto deadline = std::chrono::steady_clock::now() + test::scaled_ms(5000);
    bool backpressured = false;
    while (std::chrono::steady_clock::now() < deadline) {
        if (unwritable(left) && unwritable(right)) { backpressured = true; break; }
        if (done.wait_for(std::chrono::milliseconds(0)) == std::future_status::ready) break;
        std::this_thread::yield();
    }
    if (cancel || !backpressured) state.stop.cancel();
    else state.readers.set();
    bool joined = done.wait_for(test::scaled_ms(15000)) == std::future_status::ready;
    const bool completed_in_time = joined;
    if (!joined) {
        state.stop.cancel();
        for (auto* stream : {&left_peer, &right_peer, &left_socket, &right_socket}) stream->shutdown_socket();
        joined = done.wait_for(test::scaled_ms(5000)) == std::future_status::ready;
    }
    if (!joined || !scheduler.shutdown(test::scaled_ms(5000))) std::terminate();
    CHECK(actual_backend);
    CHECK(completed_in_time);
    REQUIRE(backpressured);
    CHECK_FALSE(state.failed);
    CHECK_FALSE(left.writing.load());
    CHECK_FALSE(right.writing.load());
    CHECK(state.result.client_to_upstream.accepted_bytes <= state.result.client_to_upstream.source_bytes);
    CHECK(state.result.upstream_to_client.accepted_bytes <= state.result.upstream_to_client.source_bytes);
    CHECK(state.result.client_to_upstream.source_bytes <= state.left_payload.size());
    CHECK(state.result.upstream_to_client.source_bytes <= state.right_payload.size());
    if (cancel) {
        CHECK(state.result.end == tunnel_end::cancelled);
        CHECK(state.result.error == ECANCELED);
    } else {
        CHECK(state.result.end == tunnel_end::completed);
        CHECK(state.left_eof);
        CHECK(state.right_eof);
        CHECK(state.received_left == state.right_payload);
        CHECK(state.received_right == state.left_payload);
        CHECK(state.left_count == state.right_payload.size());
        CHECK(state.right_count == state.left_payload.size());
        CHECK(state.result.client_to_upstream.accepted_bytes == state.left_payload.size());
        CHECK(state.result.upstream_to_client.accepted_bytes == state.right_payload.size());
    }
}

void cases(bool cancel) {
    SECTION("epoll") { exercise(backend_type::epoll, cancel, false); if (!cancel) exercise(backend_type::epoll, false, true); }
    SECTION("io_uring") {
#if ELIO_HAS_IO_URING
        if (!io::io_uring_backend::is_available()) SKIP("io_uring unavailable on this host");
        exercise(backend_type::io_uring, cancel, false);
        if (!cancel) exercise(backend_type::io_uring, false, true);
#else
        SKIP("io_uring support is not compiled");
#endif
    }
}
}

TEST_CASE("TCP tunnel relay drains both backpressured directions and EOF orders", "[http][tunnel][transport]") {
    cases(false);
}
TEST_CASE("TCP tunnel relay cancellation joins both backpressured directions", "[http][tunnel][transport][cancel]") {
    cases(true);
}
