#include <catch2/catch_test_macros.hpp>
#include <elio/http/http_tunnel_relay.hpp>
#include <elio/sync/event.hpp>
#include <elio/runtime/scheduler.hpp>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstring>
#include <future>

namespace {
using namespace elio;
using namespace elio::http;

struct relay_fake {
    std::string input, wire;
    size_t offset = 0;
    bool held_read = false, whole = false, read_after_finish = false;
    sync::event read_entered, finish_entered, finish_release, close_done;
    std::atomic<bool> finished{false}, read_returned{false}, early_cancel{false};
    std::atomic<unsigned> reads{0};
    int read_error = 0, write_error = 0;

    coro::task<io::io_result> read(void* buffer, size_t count, coro::cancel_token token) {
        reads.fetch_add(1);
        if (read_error) co_return io::io_result{-read_error, 0};
        auto registration = token.on_cancel([this] {
            if (whole && !finished.load()) early_cancel.store(true);
        });
        if (held_read || read_after_finish) {
            read_entered.set();
            if (co_await close_done.wait(token) == coro::cancel_result::cancelled) {
                read_returned.store(true);
                co_return io::io_result{-ECANCELED, 0};
            }
        }
        count = std::min(count, input.size() - offset);
        std::memcpy(buffer, input.data() + offset, count);
        offset += count;
        read_returned.store(true);
        co_return io::io_result{static_cast<int32_t>(count), 0};
    }
    coro::task<io::io_result> write(const void* buffer, size_t count, coro::cancel_token token) {
        if (write_error) co_return io::io_result{-write_error, 0};
        if (whole) co_return io::io_result{-ESHUTDOWN, 0};
        if (token.is_cancelled()) co_return io::io_result{-ECANCELED, 0};
        count = std::min<size_t>(count, 1);
        wire.append(static_cast<const char*>(buffer), count);
        co_return io::io_result{static_cast<int32_t>(count), 0};
    }
    coro::task<net::write_finish_result> finish_write(coro::cancel_token token) {
        finish_entered.set();
        // A separate awaited local avoids GCC 12.2's conditional-await frame
        // layout bug (GCC PR106188), reproduced without Elio as well.
        if (whole) {
            const auto released = co_await finish_release.wait(token);
            if (released == coro::cancel_result::cancelled)
                co_return net::write_finish_result{net::close_scope::whole_session, ECANCELED};
        }
        finished.store(true);
        close_done.set();
        co_return net::write_finish_result{read_end_scope(), 0, true, whole};
    }
    net::close_scope read_end_scope() const {
        return whole ? net::close_scope::whole_session : net::close_scope::write_direction;
    }
};

coro::task<void> release_finish(relay_fake* stream, coro::cancel_token token) {
    // Exercise an existing reader, not a child cancelled before read entry.
    (void)co_await stream->read_entered.wait(token);
    (void)co_await stream->finish_entered.wait(token);
    stream->finish_release.set();
}

tunnel_result run_fake_relay(relay_fake& client, relay_fake& upstream,
                            bool release_close = false, bool inherited_cancel = false) {
    auto view = http::detail::tunnel_stream_access::create(client);
    runtime::scheduler scheduler(2);
    coro::cancel_source cancel;
    std::promise<tunnel_result> promise;
    auto future = promise.get_future();
    scheduler.start();
    if (release_close) scheduler.go(release_finish(&client, cancel.get_token()));
    scheduler.go([&]() -> coro::task<void> {
        try {
            if (inherited_cancel) {
                sync::event start;
                auto body = [&]() -> coro::task<tunnel_result> {
                    co_await start.wait();
                    co_return co_await http::detail::relay_with_stream(view, upstream, {2}, {});
                };
                auto child = elio::spawn(body());
                child.request_cancel();
                start.set();
                promise.set_value(co_await std::move(child));
            } else {
                promise.set_value(co_await http::detail::relay_with_stream(view, upstream, {2}, cancel.get_token()));
            }
        } catch (...) { promise.set_exception(std::current_exception()); }
    });
    const bool ready = future.wait_for(std::chrono::seconds(10)) == std::future_status::ready;
    if (!ready) {
        cancel.cancel();
        client.finish_release.set(); client.close_done.set();
        upstream.finish_release.set(); upstream.close_done.set();
    }
    const bool stopped = scheduler.shutdown(std::chrono::seconds(10));
    if (!stopped) std::terminate(); // Never unwind frames still borrowing fakes.
    REQUIRE(ready);
    return future.get();
}
}

TEST_CASE("Tunnel close fixture waits for reader entry before releasing finish", "[http][tunnel][relay]") {
    relay_fake stream;
    stream.finish_entered.set();
    auto release = release_finish(&stream, {});
    auto handle = coro::detail::task_access::handle(release);
    handle.resume();
    const bool waited_for_reader = !handle.done();
    const bool released_early = stream.finish_release.is_set();
    stream.read_entered.set();
    if (!handle.done()) std::terminate();
    release.await_resume();
    CHECK(waited_for_reader);
    CHECK_FALSE(released_early);
    CHECK(stream.finish_release.is_set());
}

TEST_CASE("Tunnel relay keeps a healthy reverse direction after directional EOF", "[http][tunnel][relay]") {
    relay_fake client, upstream;
    upstream.input = std::string("r\0everse", 8);
    upstream.read_after_finish = true;
    auto result = run_fake_relay(client, upstream);
    CHECK(result.end == tunnel_end::completed);
    CHECK(result.error == 0);
    CHECK(result.client_to_upstream.source_bytes == 0);
    CHECK(result.upstream_to_client.source_bytes == 8);
    CHECK(result.upstream_to_client.accepted_bytes == 8);
    CHECK_FALSE(result.upstream_to_client.uncertain_write);
    CHECK(client.wire == upstream.input);
    CHECK(client.finished.load());
    CHECK(upstream.finished.load());
}

TEST_CASE("Tunnel relay joins whole-session close before cancelling its reader", "[http][tunnel][relay]") {
    relay_fake client, upstream;
    client.whole = true;
    client.held_read = true;
    upstream.input = "reverse";
    auto result = run_fake_relay(client, upstream, true);
    CHECK(result.end == tunnel_end::session_closed);
    CHECK(result.error == 0);
    CHECK(client.finished.load());
    CHECK(client.reads.load() == 1);
    CHECK(client.read_returned.load());
    CHECK_FALSE(client.early_cancel.load());
    CHECK(result.upstream_to_client.accepted_bytes == 0);
    CHECK(result.upstream_to_client.uncertain_write);
}

TEST_CASE("Tunnel relay preserves write failure through inline reader cancellation", "[http][tunnel][relay]") {
    relay_fake client, upstream;
    client.held_read = true;
    client.write_error = EIO;
    upstream.input = "x";
    auto client_view = http::detail::tunnel_stream_access::create(client);
    auto upstream_view = http::detail::tunnel_stream_access::create(upstream);
    http::detail::tunnel_relay_state state;
    char outbound[2]{}, inbound[2]{};
    auto first = http::detail::relay_direction(&state, &client_view, &upstream_view,
        outbound, sizeof(outbound), &state.result.client_to_upstream);
    auto first_handle = coro::detail::task_access::handle(first);
    first_handle.resume();
    const bool parked = !first_handle.done();
    auto second = http::detail::relay_direction(&state, &upstream_view, &client_view,
        inbound, sizeof(inbound), &state.result.upstream_to_client);
    auto second_handle = coro::detail::task_access::handle(second);
    // Without a scheduler, the fake event resumes cancellation inline. The
    // reader therefore selects the relay result before the failed write returns.
    second_handle.resume();
    if (!first_handle.done() || !second_handle.done()) std::terminate();
    first.await_resume(); second.await_resume();
    CHECK(parked);
    CHECK(state.result.end == tunnel_end::transport_error);
    CHECK(state.result.error == EIO);
    CHECK(client.read_returned.load());
    CHECK(state.result.upstream_to_client.source_bytes == 1);
    CHECK(state.result.upstream_to_client.accepted_bytes == 0);
    CHECK(state.result.upstream_to_client.uncertain_write);
}

TEST_CASE("Tunnel relay preserves read failure through inline finish cancellation", "[http][tunnel][relay]") {
    relay_fake client, upstream;
    upstream.whole = true;
    upstream.read_error = EIO;
    auto client_view = http::detail::tunnel_stream_access::create(client);
    auto upstream_view = http::detail::tunnel_stream_access::create(upstream);
    http::detail::tunnel_relay_state state;
    char outbound[2]{}, inbound[2]{};
    auto first = http::detail::relay_direction(&state, &client_view, &upstream_view,
        outbound, sizeof(outbound), &state.result.client_to_upstream);
    auto first_handle = coro::detail::task_access::handle(first);
    first_handle.resume();
    const bool parked = !first_handle.done();
    auto second = http::detail::relay_direction(&state, &upstream_view, &client_view,
        inbound, sizeof(inbound), &state.result.upstream_to_client);
    auto second_handle = coro::detail::task_access::handle(second);
    second_handle.resume();
    if (!first_handle.done() || !second_handle.done()) std::terminate();
    first.await_resume(); second.await_resume();
    CHECK(parked);
    CHECK(state.result.end == tunnel_end::transport_error);
    CHECK(state.result.error == EIO);
    CHECK_FALSE(upstream.finished.load());
    CHECK(state.result.client_to_upstream.source_bytes == 0);
    CHECK(state.result.upstream_to_client.source_bytes == 0);
}

#ifdef ELIO_RUNTIME_TEST_HOOKS
TEST_CASE("Tunnel relay observes inherited cancellation before child entry", "[http][tunnel][relay]") {
    relay_fake client, upstream;
    const auto result = run_fake_relay(client, upstream, false, true);
    CHECK(result.end == tunnel_end::cancelled);
    CHECK(result.error == ECANCELED);
    CHECK(client.reads.load() == 0);
    CHECK(upstream.reads.load() == 0);
}

TEST_CASE("Tunnel relay retains child startup failure over group cleanup cancellation", "[http][tunnel][relay]") {
    relay_fake client, upstream;
    client.held_read = true;
    struct startup {
        relay_fake& first;
        std::atomic<unsigned> entries{0};
    } state{client};
    http::detail::tunnel_relay_test_hooks hooks;
    hooks.context = &state;
    hooks.before_second_launch = +[](void* context) -> coro::task<void> {
        co_await static_cast<startup*>(context)->first.read_entered.wait();
    };
    hooks.before_direction_start = +[](void* context) {
        if (static_cast<startup*>(context)->entries.fetch_add(1) == 1) throw std::bad_alloc();
    };
    struct reset_hook {
        ~reset_hook() { http::detail::relay_hooks_for_test = nullptr; }
    } reset;
    http::detail::relay_hooks_for_test = &hooks;
    const auto result = run_fake_relay(client, upstream);
    CHECK(state.entries.load() == 2);
    CHECK(result.end == tunnel_end::transport_error);
    CHECK(result.error == ENOMEM);
    CHECK(client.read_returned.load());
    CHECK(upstream.reads.load() == 0);
}

TEST_CASE("Tunnel relay joins an accepted child after second launch failure", "[http][tunnel][relay]") {
    relay_fake client, upstream;
    client.held_read = true;
    http::detail::tunnel_relay_test_hooks hooks;
    hooks.context = &client;
    hooks.before_second_launch = +[](void* context) -> coro::task<void> {
        auto& stream = *static_cast<relay_fake*>(context);
        co_await stream.read_entered.wait();
        throw std::bad_alloc();
    };
    struct reset_hook {
        ~reset_hook() { http::detail::relay_hooks_for_test = nullptr; }
    } reset;
    http::detail::relay_hooks_for_test = &hooks;
    auto result = run_fake_relay(client, upstream);
    CHECK(result.end == tunnel_end::transport_error);
    CHECK(result.error == ENOMEM);
    CHECK(client.read_returned.load());
    CHECK(upstream.reads.load() == 0);
}
#endif
