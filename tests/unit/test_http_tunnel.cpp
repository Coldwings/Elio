#include <catch2/catch_test_macros.hpp>
#include <elio/http/http_tunnel_response.hpp>
#include <elio/sync/event.hpp>
#include <algorithm>
#include <array>
#include <cstring>
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

#ifdef ELIO_RUNTIME_TEST_HOOKS
namespace {
struct allocation_reader : tunnel_fake {
    sync::event never;
    bool entered = false, cleaned = false;
    coro::task<io::io_result> read(void*, size_t, coro::cancel_token token) {
        entered = true;
        const auto result = co_await never.wait(token);
        cleaned = true;
        co_return io::io_result{result == coro::cancel_result::cancelled ? -ECANCELED : 0, 0};
    }
};
void fail_tunnel_frame(void* context, http::detail::tunnel_frame frame) {
    if (frame == *static_cast<http::detail::tunnel_frame*>(context)) throw std::bad_alloc();
}
}

TEST_CASE("Tunnel task construction failure terminalizes and cancels an overlapping reader", "[http][tunnel][allocation]") {
    using frame = http::detail::tunnel_frame;
    for (auto site : {frame::read, frame::single_write, frame::vector_write, frame::finish}) {
        allocation_reader fake;
        auto stream = http::detail::tunnel_stream_access::create(fake);
        char byte{};
        auto reader = stream.read(&byte, 1);
        auto handle = coro::detail::task_access::handle(reader);
        handle.resume();
        const bool parked = fake.entered && !handle.done();
        stream.set_frame_test_hook(&site, fail_tunnel_frame);
        bool threw = false;
        try {
            if (site == frame::read) (void)stream.read(&byte, 1);
            else if (site == frame::single_write) (void)stream.write("x");
            else if (site == frame::vector_write) {
                iovec part{&byte, 1};
                (void)stream.writev(std::span<const iovec>(&part, 1));
            } else (void)stream.finish_output();
        } catch (const std::bad_alloc&) { threw = true; }
        // Standalone event cancellation resumes inline. Release the fake gate
        // on regression so assertion failure never unwinds a live reader.
        const bool cancelled_reader = handle.done();
        if (!cancelled_reader) fake.never.set();
        if (!handle.done()) std::terminate();
        const auto read = reader.await_resume();
        CHECK(parked);
        CHECK(threw);
        CHECK(cancelled_reader);
        CHECK(stream.error() == ENOMEM);
        CHECK(fake.cleaned);
        CHECK(read.result == -ENOMEM);
        CHECK(fake.wire.empty());
    }
}

TEST_CASE("Tunnel scalar write reports nested operation frame allocation failure", "[http][tunnel][allocation]") {
    allocation_reader fake;
    auto stream = http::detail::tunnel_stream_access::create(fake);
    char byte{};
    auto reader = stream.read(&byte, 1);
    auto handle = coro::detail::task_access::handle(reader);
    handle.resume();
    auto site = http::detail::tunnel_frame::vector_write;
    stream.set_frame_test_hook(&site, fail_tunnel_frame);
    const auto result = immediate(stream.write("x"));
    const bool cancelled_reader = handle.done();
    if (!cancelled_reader) fake.never.set();
    if (!handle.done()) std::terminate();
    const auto read = reader.await_resume();
    CHECK(result.error == ENOMEM);
    CHECK(cancelled_reader);
    CHECK(result.accepted_bytes == 0);
    CHECK_FALSE(result.uncertain_attempt);
    CHECK(stream.error() == ENOMEM);
    CHECK(fake.cleaned);
    CHECK(read.result == -ENOMEM);
    CHECK(fake.wire.empty());
}
#endif
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
