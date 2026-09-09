#include <catch2/catch_test_macros.hpp>
#include <elio/elio.hpp>
#include <elio/http/http_body_writer.hpp>

#include <deque>
#include <functional>
#include <future>
#include <stdexcept>
#include <string>
#include <vector>

using namespace elio;
using namespace elio::http;

namespace {
struct controlled_body_stream {
    std::string wire;
    size_t calls = 0;
    size_t max_progress = std::numeric_limits<int32_t>::max();
    std::deque<int32_t> results;
    std::function<void(iovec*, size_t)> inspect;
    std::function<void()> before_completion;
    bool hold = false;
    bool cancelled = false;
    bool cleaned = false;
    std::coroutine_handle<> pending;
    iovec* pending_vectors = nullptr;
    size_t pending_count = 0;

    struct gate {
        controlled_body_stream& stream;
        bool await_ready() const noexcept { return !stream.hold; }
        void await_suspend(std::coroutine_handle<> continuation) noexcept { stream.pending = continuation; }
        void await_resume() const noexcept {}
    };

    coro::task<io::io_result> writev(iovec* vectors, size_t count, coro::cancel_token token) {
        ++calls;
        cleaned = false;
        struct cleanup {
            bool& flag;
            ~cleanup() { flag = true; }
        } cleanup_on_return{cleaned};
        auto registration = token.on_cancel([this] { cancelled = true; });
        if (inspect) inspect(vectors, count);
        pending_vectors = vectors;
        pending_count = count;
        co_await gate{*this};
        if (before_completion) before_completion();
        int32_t progress = static_cast<int32_t>(max_progress);
        if (!results.empty()) { progress = results.front(); results.pop_front(); }
        if (progress < 0) co_return io::io_result{progress, 0};
        size_t available = 0;
        for (size_t i = 0; i < count; ++i) available += vectors[i].iov_len;
        size_t transferred = std::min<size_t>(progress, available);
        auto remaining = transferred;
        for (size_t i = 0; i < count && remaining; ++i) {
            const auto bytes = std::min(remaining, vectors[i].iov_len);
            wire.append(static_cast<const char*>(vectors[i].iov_base), bytes);
            remaining -= bytes;
        }
        co_return io::io_result{static_cast<int32_t>(transferred), 0};
    }
};

send_result run_body_operation(coro::task<send_result> operation) {
    auto handle = coro::detail::task_access::handle(operation);
    handle.resume();
    REQUIRE(handle.done());
    return operation.await_resume();
}

response_plan body_plan(std::optional<uint64_t> length = std::nullopt,
                        response_transfer transfer = response_transfer::automatic) {
    return prepare_response(response_head{}, {response_body_kind::streaming, length, transfer},
                            method::GET, "HTTP/1.1", true);
}

void open_writer(body_writer& writer, controlled_body_stream& stream) {
    REQUIRE(run_body_operation(http::detail::body_writer_access::send_headers(writer, "head\r\n\r\n")).success());
    REQUIRE(stream.wire == "head\r\n\r\n");
    stream.wire.clear();
    stream.calls = 0;
}
}

TEST_CASE("body writer borrows slices and advances short write cursors", "[http][body_writer]") {
    controlled_body_stream stream;
    auto writer = http::detail::body_writer_access::create(stream, body_plan(11));
    open_writer(writer, stream);
    const std::string first = "hello";
    const std::string second = " world";
    const body_buffer buffers[]{{nullptr, 0}, {first.data(), first.size()}, {second.data(), second.size()}};
    stream.max_progress = 2;
    bool borrowed_first = false;
    bool borrowed_second = false;
    stream.inspect = [&](iovec* vectors, size_t count) {
        for (size_t i = 0; i < count; ++i) {
            if (vectors[i].iov_base == first.data()) borrowed_first = true;
            if (vectors[i].iov_base == second.data()) borrowed_second = true;
        }
    };
    const auto result = run_body_operation(writer.writev(buffers));
    REQUIRE(result.success());
    REQUIRE(result.confirmed_body_bytes == 11);
    REQUIRE(stream.wire == "hello world");
    REQUIRE(stream.calls == 6);
    REQUIRE(borrowed_first);
    REQUIRE(borrowed_second);
    REQUIRE(buffers[1].data == first.data());
    REQUIRE(buffers[1].size == 5);
    REQUIRE(buffers[2].data == second.data());
    REQUIRE(buffers[2].size == 6);
    REQUIRE(stream.cleaned);
    REQUIRE(run_body_operation(http::detail::body_writer_access::finish(writer)).success());
    REQUIRE(stream.calls == 6);
    REQUIRE(run_body_operation(writer.write("")).error == send_errc::invalid_state);
}

TEST_CASE("body writer generates chunk framing without counting framing as payload", "[http][body_writer]") {
    controlled_body_stream stream;
    auto writer = http::detail::body_writer_access::create(stream, body_plan());
    open_writer(writer, stream);
    stream.max_progress = 1;
    REQUIRE(run_body_operation(writer.write("")).success());
    REQUIRE(stream.calls == 0);
    const auto sent = run_body_operation(writer.write("hello"));
    REQUIRE(sent.success());
    REQUIRE(sent.confirmed_body_bytes == 5);
    REQUIRE(stream.wire == "5\r\nhello\r\n");
    const auto finished = run_body_operation(http::detail::body_writer_access::finish(writer));
    REQUIRE(finished.success());
    REQUIRE(finished.confirmed_body_bytes == 5);
    REQUIRE(stream.wire == "5\r\nhello\r\n0\r\n\r\n");
    REQUIRE(run_body_operation(http::detail::body_writer_access::finish(writer)).error == send_errc::invalid_state);
}

TEST_CASE("body writer bounds descriptor scratch independently of caller count", "[http][body_writer]") {
    controlled_body_stream stream;
    auto writer = http::detail::body_writer_access::create(stream, body_plan());
    open_writer(writer, stream);
    const char byte = 'x';
    std::vector<body_buffer> buffers(200, body_buffer{&byte, 1});
    size_t maximum_vectors = 0;
    stream.inspect = [&](iovec*, size_t count) { maximum_vectors = std::max(maximum_vectors, count); };
    const auto result = run_body_operation(writer.writev(buffers));
    REQUIRE(result.success());
    REQUIRE(result.confirmed_body_bytes == 200);
    REQUIRE(maximum_vectors <= 64);
    REQUIRE(stream.calls == 4);
    REQUIRE(stream.wire == "3e\r\n" + std::string(62, 'x') + "\r\n3e\r\n" +
            std::string(62, 'x') + "\r\n3e\r\n" + std::string(62, 'x') +
            "\r\ne\r\n" + std::string(14, 'x') + "\r\n");
}

TEST_CASE("body writer enforces lengths and terminal state before more bytes", "[http][body_writer]") {
    SECTION("overrun is rejected without a partial write") {
        controlled_body_stream stream;
        auto writer = http::detail::body_writer_access::create(stream, body_plan(2));
        open_writer(writer, stream);
        REQUIRE(run_body_operation(writer.write("abc")).error == send_errc::length_mismatch);
        REQUIRE(stream.calls == 0);
        REQUIRE(run_body_operation(writer.write("ab")).error == send_errc::length_mismatch);
        REQUIRE(run_body_operation(http::detail::body_writer_access::finish(writer)).error == send_errc::length_mismatch);
    }
    SECTION("underproduction fails finalization") {
        controlled_body_stream stream;
        auto writer = http::detail::body_writer_access::create(stream, body_plan(2));
        open_writer(writer, stream);
        REQUIRE(run_body_operation(writer.write("a")).success());
        const auto result = run_body_operation(http::detail::body_writer_access::finish(writer));
        REQUIRE(result.error == send_errc::length_mismatch);
        REQUIRE(result.confirmed_body_bytes == 1);
        REQUIRE(stream.wire == "a");
    }
    SECTION("known zero still rejects accidental producer bytes") {
        controlled_body_stream stream;
        auto writer = http::detail::body_writer_access::create(stream, body_plan(0));
        open_writer(writer, stream);
        REQUIRE(run_body_operation(writer.write("")).success());
        REQUIRE(run_body_operation(writer.write("a")).error == send_errc::length_mismatch);
        REQUIRE(stream.calls == 0);
    }
    SECTION("body writes cannot precede headers") {
        controlled_body_stream stream;
        auto writer = http::detail::body_writer_access::create(stream, body_plan(0));
        REQUIRE(run_body_operation(writer.write("")).error == send_errc::invalid_state);
        REQUIRE(stream.calls == 0);
    }
}

TEST_CASE("body writer retries EINTR but preserves unrecoverable transport failures", "[http][body_writer]") {
    for (const auto error : {0, EPIPE, EAGAIN}) {
        controlled_body_stream stream;
        auto writer = http::detail::body_writer_access::create(stream, body_plan(4));
        open_writer(writer, stream);
        stream.results = {-EINTR, 2, error ? -error : 2};
        const auto result = run_body_operation(writer.write("abcd"));
        REQUIRE(stream.calls == 3);
        REQUIRE(result.success() == (error == 0));
        REQUIRE(result.confirmed_body_bytes == (error ? 2 : 4));
        if (error) {
            REQUIRE(result.error == send_errc::transport_error);
            REQUIRE(result.transport_error == error);
            REQUIRE(run_body_operation(writer.write("")).error == send_errc::transport_error);
            REQUIRE(stream.calls == 3);
        }
    }
    controlled_body_stream stream;
    auto writer = http::detail::body_writer_access::create(stream, body_plan(1));
    open_writer(writer, stream);
    stream.results = {0};
    const auto result = run_body_operation(writer.write("a"));
    REQUIRE(result.error == send_errc::transport_error);
    REQUIRE(result.transport_error == EPIPE);
}

TEST_CASE("body writer cancellation waits for borrowed I/O cleanup and keeps winner", "[http][body_writer]") {
    for (const bool session : {false, true}) {
        controlled_body_stream stream;
        coro::cancel_source source;
        auto writer = http::detail::body_writer_access::create(
            stream, body_plan(3), session ? source.get_token() : coro::cancel_token{});
        open_writer(writer, stream);
        stream.hold = true;
        const std::string bytes = "abc";
        auto operation = writer.write(bytes, session ? coro::cancel_token{} : source.get_token());
        auto handle = coro::detail::task_access::handle(operation);
        handle.resume();
        REQUIRE_FALSE(handle.done());
        REQUIRE(stream.pending);
        REQUIRE(stream.pending_vectors[0].iov_base == bytes.data());
        source.cancel();
        REQUIRE(stream.cancelled);
        REQUIRE_FALSE(handle.done());
        REQUIRE_FALSE(stream.cleaned);
        // A late positive completion reports side effects, not success. The
        // borrowed descriptor and payload are still valid after cancellation.
        REQUIRE(std::string_view(static_cast<const char*>(stream.pending_vectors[0].iov_base),
                                 stream.pending_vectors[0].iov_len) == bytes);
        stream.pending.resume();
        REQUIRE(handle.done());
        const auto result = operation.await_resume();
        REQUIRE(result.error == send_errc::cancelled);
        REQUIRE(result.confirmed_body_bytes == 3);
        REQUIRE(stream.cleaned);
        REQUIRE(stream.wire == bytes);
        REQUIRE(run_body_operation(http::detail::body_writer_access::finish(writer)).error == send_errc::cancelled);
    }
}

TEST_CASE("body writer completed write is not retroactively cancelled", "[http][body_writer]") {
    controlled_body_stream stream;
    coro::cancel_source source;
    auto writer = http::detail::body_writer_access::create(stream, body_plan(3));
    open_writer(writer, stream);
    const auto result = run_body_operation(writer.write("abc", source.get_token()));
    REQUIRE(result.success());
    source.cancel();
    REQUIRE(http::detail::body_writer_access::result(writer).success());
    REQUIRE(run_body_operation(http::detail::body_writer_access::finish(writer)).success());
}

#ifdef ELIO_RUNTIME_TEST_HOOKS
TEST_CASE("body writer completed write is not retroactively timed out", "[http][body_writer]") {
    controlled_body_stream stream;
    std::function<void()> expire;
    {
        auto writer = http::detail::body_writer_access::create(stream, body_plan(6));
        open_writer(writer, stream);
        stream.max_progress = 1;
        http::detail::capture_next_body_write_timeout_for_test(expire);
        const auto result = run_body_operation(writer.write("abc"));
        REQUIRE(result.success());
        REQUIRE(result.confirmed_body_bytes == 3);
        REQUIRE(stream.cleaned);
        REQUIRE(expire);
        REQUIRE(http::detail::next_body_write_timeout_for_test == nullptr);

        // The captured callback owns the operation arbitration state, not its
        // coroutine frame. Deliver it only after that frame has been destroyed.
        expire();
        REQUIRE_FALSE(stream.cancelled);
        REQUIRE(stream.calls == 3);
        REQUIRE(http::detail::body_writer_access::result(writer).success());
        REQUIRE(run_body_operation(writer.write("def")).success());
        REQUIRE(run_body_operation(http::detail::body_writer_access::finish(writer)).success());
        REQUIRE(http::detail::body_writer_access::result(writer).confirmed_body_bytes == 6);
        REQUIRE(stream.wire == "abcdef");
    }
    // A queued callback may also outlive the writer; its shared state must not
    // retain access to the writer, transport, or caller-owned body buffers.
    const auto calls = stream.calls;
    expire();
    REQUIRE_FALSE(stream.cancelled);
    REQUIRE(stream.calls == calls);
    REQUIRE(stream.wire == "abcdef");
    expire = {};
}

TEST_CASE("body writer setup allocation failures are sticky and suppress terminators", "[http][body_writer]") {
    using site = http::detail::body_write_allocation_site;
    for (const auto failure_site : {site::arbitration, site::session_registration, site::operation_registration}) {
        controlled_body_stream stream;
        coro::cancel_source session;
        coro::cancel_source operation;
        auto writer = http::detail::body_writer_access::create(stream, body_plan(), session.get_token());
        open_writer(writer, stream);
        struct reset_hook {
            ~reset_hook() { http::detail::fail_body_write_allocation_for_test = site::none; }
        } reset;
        http::detail::fail_body_write_allocation_for_test = failure_site;
        const auto result = run_body_operation(writer.write("abc", operation.get_token()));
        REQUIRE(result.error == send_errc::transport_error);
        REQUIRE(result.transport_error == ENOMEM);
        REQUIRE(stream.calls == 0);
        // Partially installed registrations have been detached before return.
        // An application ignoring the failed write cannot restore open state.
        operation.cancel();
        session.cancel();
        REQUIRE(run_body_operation(writer.write("ignored failure")).transport_error == ENOMEM);
        REQUIRE(run_body_operation(http::detail::body_writer_access::finish(writer)).transport_error == ENOMEM);
        REQUIRE(stream.wire.empty());
        REQUIRE(stream.calls == 0);
    }
}

TEST_CASE("body writer coroutine frame failures stay terminal even if producer catches", "[http][body_writer]") {
    using site = http::detail::body_write_allocation_site;
    for (const auto failure_site : {site::single_frame, site::operation_frame}) {
        for (const bool vectored : {false, true}) {
            if (vectored && failure_site == site::single_frame) continue;
            controlled_body_stream stream;
            auto writer = http::detail::body_writer_access::create(stream, body_plan());
            open_writer(writer, stream);
            struct reset_hook {
                ~reset_hook() { http::detail::fail_body_write_allocation_for_test = site::none; }
            } reset;
            http::detail::fail_body_write_allocation_for_test = failure_site;
            bool caught = false;
            try {
                if (vectored) {
                    const body_buffer buffer{"abc", 3};
                    (void)run_body_operation(writer.writev(std::span(&buffer, 1)));
                } else {
                    (void)run_body_operation(writer.write("abc"));
                }
            } catch (const std::bad_alloc&) {
                caught = true;
            }
            REQUIRE(caught);
            const auto sticky = http::detail::body_writer_access::result(writer);
            REQUIRE(sticky.error == send_errc::transport_error);
            REQUIRE(sticky.transport_error == ENOMEM);
            REQUIRE(run_body_operation(writer.write("")).transport_error == ENOMEM);
            REQUIRE(run_body_operation(http::detail::body_writer_access::finish(writer)).transport_error == ENOMEM);
            REQUIRE(stream.calls == 0);
            REQUIRE(stream.wire.empty());
        }
    }
}

TEST_CASE("body writer preserves winning internal error through transport cleanup",
          "[http][body_writer]") {
    for (const int mode : {0, 1, 2, 3, 4, 5}) {
        CAPTURE(mode);
        controlled_body_stream stream;
        auto writer = http::detail::body_writer_access::create(stream, body_plan(3));
        open_writer(writer, stream);
        stream.hold = true;
        const int cleanup_results[]{-ECANCELED, -EIO, 0, 3, 0, 0};
        stream.results = {cleanup_results[mode]};
        stream.before_completion = [mode] {
            if (mode == 4) throw std::bad_alloc();
            if (mode == 5) throw std::runtime_error("cleanup failure");
        };
        std::function<void()> fail;
        http::detail::capture_next_body_write_internal_error_for_test(fail);
        const std::string bytes = "abc";
        auto operation = writer.write(bytes);
        auto handle = coro::detail::task_access::handle(operation);
        handle.resume();
        REQUIRE_FALSE(handle.done());
        REQUIRE(stream.pending);
        REQUIRE(fail);
        fail();
        REQUIRE(stream.cancelled);
        REQUIRE_FALSE(handle.done());
        REQUIRE_FALSE(stream.cleaned);
        REQUIRE(stream.pending_vectors[0].iov_base == bytes.data());
        stream.pending.resume();
        REQUIRE(handle.done());
        const auto result = operation.await_resume();
        REQUIRE(result.error == send_errc::transport_error);
        REQUIRE(result.transport_error == ENOMEM);
        REQUIRE(result.confirmed_body_bytes == (mode == 3 ? 3 : 0));
        REQUIRE(stream.cleaned);
        REQUIRE(stream.calls == 1);
        REQUIRE(run_body_operation(
            http::detail::body_writer_access::finish(writer)).transport_error == ENOMEM);
        REQUIRE(stream.calls == 1);
    }
}

TEST_CASE("body writer timeout arbitration is deterministic and survives late progress", "[http][body_writer]") {
    for (const bool cancel_first : {false, true}) {
        controlled_body_stream stream;
        coro::cancel_source source;
        auto writer = http::detail::body_writer_access::create(stream, body_plan(3));
        open_writer(writer, stream);
        std::function<void()> expire;
        http::detail::capture_next_body_write_timeout_for_test(expire);
        stream.before_completion = [&] {
            REQUIRE(expire);
            if (cancel_first) { source.cancel(); expire(); }
            else { expire(); source.cancel(); }
        };
        const auto result = run_body_operation(writer.write("abc", source.get_token()));
        REQUIRE(result.error == (cancel_first ? send_errc::cancelled : send_errc::timed_out));
        REQUIRE(result.confirmed_body_bytes == 3);
        REQUIRE(stream.cleaned);
        REQUIRE(stream.cancelled);
        REQUIRE(stream.calls == 1);
    }
}
#endif

TEST_CASE("body writer rejects invalid buffers and already cancelled operations", "[http][body_writer]") {
    SECTION("null nonempty descriptor") {
        controlled_body_stream stream;
        auto writer = http::detail::body_writer_access::create(stream, body_plan());
        open_writer(writer, stream);
        const body_buffer buffer{nullptr, 1};
        const auto result = run_body_operation(writer.writev(std::span(&buffer, 1)));
        REQUIRE(result.error == send_errc::invalid_response);
        REQUIRE(result.transport_error == EFAULT);
        REQUIRE(stream.calls == 0);
    }
    SECTION("already cancelled empty write") {
        controlled_body_stream stream;
        auto writer = http::detail::body_writer_access::create(stream, body_plan(0));
        open_writer(writer, stream);
        coro::cancel_source source;
        source.cancel();
        REQUIRE(run_body_operation(writer.write("", source.get_token())).error == send_errc::cancelled);
        REQUIRE(stream.calls == 0);
    }
    SECTION("close delimiting adds no footer") {
        controlled_body_stream stream;
        auto writer = http::detail::body_writer_access::create(stream,
            body_plan(std::nullopt, response_transfer::close_delimited));
        open_writer(writer, stream);
        REQUIRE(run_body_operation(writer.write("abc")).success());
        REQUIRE(run_body_operation(http::detail::body_writer_access::finish(writer)).success());
        REQUIRE(stream.wire == "abc");
    }
}

TEST_CASE("body writer joins a cancelled real watchdog before returning", "[http][body_writer]") {
    controlled_body_stream stream;
    runtime::scheduler scheduler(1);
    std::promise<send_result> completed;
    auto future = completed.get_future();
    scheduler.start();
    scheduler.go([&]() -> coro::task<void> {
        try {
            auto writer = http::detail::body_writer_access::create(stream, body_plan(3), {}, std::chrono::hours(1));
            auto result = co_await http::detail::body_writer_access::send_headers(writer, "head\r\n\r\n");
            if (result.success()) result = co_await writer.write("abc");
            if (result.success()) result = co_await http::detail::body_writer_access::finish(writer);
            completed.set_value(result);
        } catch (...) {
            completed.set_exception(std::current_exception());
        }
    });
    const bool ready = future.wait_for(std::chrono::seconds(5)) == std::future_status::ready;
    const bool stopped = scheduler.shutdown(std::chrono::seconds(5));
    REQUIRE(ready);
    REQUIRE(stopped);
    REQUIRE(future.get().success());
    REQUIRE(stream.cleaned);
    REQUIRE(stream.wire == "head\r\n\r\nabc");
}
