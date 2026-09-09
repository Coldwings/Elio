#include <catch2/catch_test_macros.hpp>
#include <elio/http/http_tunnel_response.hpp>
#include <algorithm>
#include <array>
#include <deque>
#include <memory>
#include <stdexcept>
#include <type_traits>
#include <vector>

namespace {
using namespace elio;
using namespace elio::http;
struct tunnel_fake {
    std::string input = "tail", wire;
    size_t offset = 0, reads = 0;
    std::deque<int> results;
    std::vector<const void*> pointers;
    coro::task<io::io_result> read(void* p, size_t n, coro::cancel_token) {
        ++reads;
        n = std::min(n, input.size() - offset);
        std::memcpy(p, input.data() + offset, n);
        offset += n;
        co_return io::io_result{static_cast<int32_t>(n), 0};
    }
    coro::task<io::io_result> write(const void* p, size_t n, coro::cancel_token) {
        pointers.push_back(p);
        int result = static_cast<int>(std::min<size_t>(n, 2));
        if (!results.empty()) { result = results.front(); results.pop_front(); }
        if (result > 0) wire.append(static_cast<const char*>(p), static_cast<size_t>(result));
        co_return io::io_result{result, 0};
    }
    coro::task<net::write_finish_result> finish_write(coro::cancel_token) {
        co_return net::write_finish_result{net::close_scope::write_direction, 0, true, false};
    }
    net::close_scope read_end_scope() const { return net::close_scope::write_direction; }
};
template<typename T> T immediate(coro::task<T> task) {
    auto handle = coro::detail::task_access::handle(task);
    handle.resume();
    // Every fake await is synchronous; an unexpected suspension is unsafe to
    // destroy, so stop rather than letting an assertion unwind its frame.
    if (!handle.done()) std::terminate();
    return task.await_resume();
}
}

TEST_CASE("Tunnel read-ahead is binary and consumed once before transport reads", "[http][tunnel]") {
    tunnel_fake fake;
    auto stream = http::detail::tunnel_stream_access::create(fake, std::string("a\0b", 3));
    std::array<char, 8> bytes{};
    CHECK(immediate(stream.read(bytes.data(), 0)).result == 0);
    CHECK(immediate(stream.read(bytes.data(), 2)).result == 2);
    CHECK(std::string(bytes.data(), 2) == std::string("a\0", 2));
    CHECK(fake.reads == 0);
    CHECK(immediate(stream.read(bytes.data(), bytes.size())).result == 1);
    CHECK(bytes[0] == 'b');
    CHECK(immediate(stream.read(bytes.data(), bytes.size())).result == 4);
    CHECK(std::string(bytes.data(), 4) == "tail");
    CHECK(immediate(stream.read(bytes.data(), bytes.size())).result == 0);
    CHECK(fake.reads == 2);
}

TEST_CASE("Tunnel full vector writes borrow slices and retain confirmed progress on failure", "[http][tunnel]") {
    tunnel_fake fake;
    auto stream = http::detail::tunnel_stream_access::create(fake);
    std::string first = "abc", second = "def";
    const std::array<iovec, 3> parts{{{first.data(), first.size()}, {nullptr, 0}, {second.data(), second.size()}}};
    SECTION("full borrowed progress") {
        auto result = immediate(stream.writev(parts));
        CHECK(result.success());
        CHECK(result.accepted_bytes == 6);
        CHECK_FALSE(result.uncertain_attempt);
        CHECK(fake.wire == "abcdef");
        REQUIRE(fake.pointers.size() == 4);
        CHECK(fake.pointers[0] == first.data());
        CHECK(fake.pointers[1] == first.data() + 2);
        CHECK(fake.pointers[2] == second.data());
        CHECK(fake.pointers[3] == second.data() + 2);
    }
    SECTION("failed attempted suffix is uncertain, not known lost") {
        fake.results = {2, -EPIPE};
        auto result = immediate(stream.writev(parts));
        CHECK(result.error == EPIPE);
        CHECK(result.accepted_bytes == 2);
        CHECK(result.uncertain_attempt);
        CHECK(immediate(stream.write("later")).error == EPIPE);
        CHECK(fake.pointers.size() == 2);
    }
    SECTION("pre-cancel does not claim an attempted write") {
        coro::cancel_source cancel;
        cancel.cancel();
        auto result = immediate(stream.writev(parts, cancel.get_token()));
        CHECK(result.error == ECANCELED);
        CHECK_FALSE(result.uncertain_attempt);
        CHECK(fake.pointers.empty());
    }
}

TEST_CASE("Tunnel response owns a move-only session and invokes it at most once", "[http][tunnel]") {
    static_assert(!std::is_move_constructible_v<tunnel_stream>);
    static_assert(!std::is_copy_constructible_v<tunnel_response>);
    tunnel_fake fake;
    auto stream = http::detail::tunnel_stream_access::create(fake);
    int calls = 0;
    tunnel_response response([owned = std::make_unique<int>(7), &calls](tunnel_stream&, coro::cancel_token)
        -> coro::task<tunnel_result> { calls += *owned; co_return tunnel_result{}; });
    tunnel_response moved(std::move(response));
    CHECK_FALSE(http::detail::tunnel_response_access::can_run(response));
    CHECK(immediate(http::detail::tunnel_response_access::run(moved, stream, {})).success());
    CHECK(immediate(http::detail::tunnel_response_access::run(moved, stream, {})).end == tunnel_end::invalid_state);
    CHECK(calls == 7);
    tunnel_response throwing([](tunnel_stream&, coro::cancel_token) -> coro::task<tunnel_result> {
        throw std::runtime_error("session"); co_return tunnel_result{};
    });
    CHECK(immediate(http::detail::tunnel_response_access::run(throwing, stream, {})).end == tunnel_end::callback_error);
}
