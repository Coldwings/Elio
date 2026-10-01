#include <catch2/catch_test_macros.hpp>

#if defined(ELIO_HAS_TLS) && ELIO_HAS_TLS && defined(ELIO_RUNTIME_TEST_HOOKS)
#include <elio/tls/detail/tls_transport.hpp>
#include <elio/tls/tls_stream.hpp>
#include <elio/net/byte_stream.hpp>
#include <elio/runtime/scheduler.hpp>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <deque>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {
using elio::tls::detail::tls_transport;
using elio::tls::detail::basic_tls_transport;
using elio::coro::detail::task_access;

std::shared_ptr<tls_transport> notification_transport() {
    return std::make_shared<tls_transport>(elio::net::tcp_stream{-1}, 32);
}

std::vector<std::byte> bytes_of(std::string_view text) {
    std::vector<std::byte> bytes(text.size());
    std::memcpy(bytes.data(), text.data(), text.size());
    return bytes;
}

std::string text_of(const std::vector<std::byte>& bytes) {
    std::string text(bytes.size(), '\0');
    std::memcpy(text.data(), bytes.data(), bytes.size());
    return text;
}

struct scripted_lower_state {
    std::deque<elio::io::io_result> reads;
    std::deque<elio::io::io_result> writes;
    std::vector<std::byte> read_bytes;
    size_t read_offset = 0;
    std::vector<std::byte> written_bytes;
    void* callback_context = nullptr;
    void (*after_positive_read)(void*) = nullptr;
    void (*after_positive_write)(void*) = nullptr;
    unsigned read_calls = 0;
    unsigned write_calls = 0;
    unsigned aborts = 0;
    unsigned finishes = 0;
    unsigned shutdowns = 0;
    bool abort_throws = false;
    bool shutdown_throws = false;
};

class scripted_lower_stream {
public:
    using byte_stream_contract = elio::net::publishing_byte_stream_contract;

    explicit scripted_lower_stream(std::shared_ptr<scripted_lower_state> state)
        : state_(std::move(state)) {}
    scripted_lower_stream(scripted_lower_stream&&) noexcept = default;
    scripted_lower_stream& operator=(scripted_lower_stream&&) noexcept = default;
    scripted_lower_stream(const scripted_lower_stream&) = delete;
    scripted_lower_stream& operator=(const scripted_lower_stream&) = delete;

    elio::coro::task<elio::io::io_result> read(
        void* buffer, size_t length, elio::coro::cancel_token token) {
        ++state_->read_calls;
        if (state_->reads.empty()) co_return elio::io::io_result{-EAGAIN, 0};
        auto result = state_->reads.front();
        state_->reads.pop_front();
        if (result.result > 0) {
            const auto count = static_cast<size_t>(result.result);
            if (count > length ||
                count > state_->read_bytes.size() - state_->read_offset) {
                co_return elio::io::io_result{-EOVERFLOW, 0};
            }
            std::memcpy(buffer, state_->read_bytes.data() + state_->read_offset, count);
            state_->read_offset += count;
            if (state_->after_positive_read) {
                state_->after_positive_read(state_->callback_context);
            }
            co_return result;
        }
        if (token.is_cancelled() && result.result >= 0)
            co_return elio::io::io_result{-ECANCELED, 0};
        co_return result;
    }

    elio::coro::task<elio::io::io_result> write(
        const void* buffer, size_t length, elio::coro::cancel_token token) {
        ++state_->write_calls;
        elio::io::io_result result{static_cast<int32_t>(length), 0};
        if (!state_->writes.empty()) {
            result = state_->writes.front();
            state_->writes.pop_front();
        }
        if (result.result > 0) {
            const auto count = static_cast<size_t>(result.result);
            if (count > length) co_return elio::io::io_result{-EOVERFLOW, 0};
            const auto* first = static_cast<const std::byte*>(buffer);
            state_->written_bytes.insert(state_->written_bytes.end(), first, first + count);
            if (state_->after_positive_write) {
                state_->after_positive_write(state_->callback_context);
            }
            co_return result;
        }
        if (token.is_cancelled() && result.result >= 0)
            co_return elio::io::io_result{-ECANCELED, 0};
        co_return result;
    }

    elio::coro::task<elio::net::write_finish_result> finish_write(
        elio::coro::cancel_token token, std::chrono::milliseconds) {
        ++state_->finishes;
        if (token.is_cancelled()) {
            co_return elio::net::write_finish_result{
                elio::net::close_scope::write_direction, ECANCELED};
        }
        co_return elio::net::write_finish_result{
            elio::net::close_scope::write_direction, 0, true, false};
    }

    elio::net::close_scope read_end_scope() const noexcept {
        return elio::net::close_scope::write_direction;
    }

    elio::coro::task<void> abort_and_settle() {
        ++state_->aborts;
        if (state_->abort_throws) throw std::runtime_error("scripted abort");
        co_return;
    }

    void shutdown_socket() {
        ++state_->shutdowns;
        if (state_->shutdown_throws) throw std::runtime_error("scripted shutdown");
    }

private:
    std::shared_ptr<scripted_lower_state> state_;
};

static_assert(elio::net::publishing_byte_stream<scripted_lower_stream>);

using scripted_transport = basic_tls_transport<scripted_lower_stream>;

std::shared_ptr<scripted_transport>
make_scripted_transport(const std::shared_ptr<scripted_lower_state>& state) {
    return std::make_shared<scripted_transport>(scripted_lower_stream{state}, 64);
}

void cancel_source_callback(void* context) {
    static_cast<elio::coro::cancel_source*>(context)->cancel();
}

struct counted_cancel {
    elio::coro::cancel_source* source = nullptr;
    unsigned target = 0;
    unsigned calls = 0;
};

void counted_cancel_callback(void* context) {
    auto& state = *static_cast<counted_cancel*>(context);
    if (++state.calls == state.target) state.source->cancel();
}

template<typename Function>
void run_transport_on_scheduler(Function work) {
    elio::runtime::scheduler scheduler(1);
    scheduler.start();
    std::atomic<bool> done{false};
    bool threw = false;
    scheduler.go([&]() -> elio::coro::task<void> {
        try {
            co_await work();
        } catch (...) {
            threw = true;
        }
        done.store(true, std::memory_order_release);
    });
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (!done.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::yield();
    }
    REQUIRE(done.load(std::memory_order_acquire));
    REQUIRE(scheduler.shutdown(std::chrono::seconds(5)));
    REQUIRE_FALSE(threw);
}
}

TEST_CASE("TLS progress before waiter publication is retained", "[tls][transport][issue-1215]") {
    auto transport = notification_transport();
    auto wait = transport->wait_change(transport->generation, {});
    transport->notify_progress();
    auto handle = task_access::handle(wait);
    handle.resume();
    REQUIRE(handle.done());
    CHECK(wait.await_resume().result == 0);
}

TEST_CASE("TLS progress wakes all published waiters exactly once", "[tls][transport][issue-1215]") {
    auto transport = notification_transport();
    auto first = transport->wait_change(0, {});
    auto second = transport->wait_change(0, {});
    auto first_handle = task_access::handle(first);
    auto second_handle = task_access::handle(second);
    first_handle.resume();
    second_handle.resume();
    const bool both_parked = !first_handle.done() && !second_handle.done();
    transport->notify_progress();
    transport->notify_progress();
    REQUIRE(first_handle.done());
    REQUIRE(second_handle.done());
    CHECK(both_parked);
    CHECK(first.await_resume().result == 0);
    CHECK(second.await_resume().result == 0);
}

TEST_CASE("TLS cancelled progress waiter does not consume sibling notification", "[tls][transport][issue-1215]") {
    auto transport = notification_transport();
    elio::coro::cancel_source cancel;
    auto first = transport->wait_change(0, cancel.get_token());
    auto second = transport->wait_change(0, {});
    auto first_handle = task_access::handle(first);
    auto second_handle = task_access::handle(second);
    first_handle.resume();
    second_handle.resume();
    cancel.cancel();
    const bool second_parked = !second_handle.done();
    transport->notify_progress();
    REQUIRE(first_handle.done());
    REQUIRE(second_handle.done());
    CHECK(second_parked);
    CHECK(first.await_resume().result == -ECANCELED);
    CHECK(second.await_resume().result == 0);
}

TEST_CASE("TLS read poll rechecks progress at source publication", "[tls][transport][issue-1215]") {
    auto transport = notification_transport();
    transport->read_publish_context = transport.get();
    transport->before_read_publish = [](void* context) {
        static_cast<tls_transport*>(context)->notify_progress();
    };
    auto wait = transport->wait_read(0, {});
    auto handle = task_access::handle(wait);
    // No scheduler or socket is available: success must come from the epoch
    // recheck, never a backend poll after the notification was already lost.
    handle.resume();
    REQUIRE(handle.done());
    CHECK(wait.await_resume().result == 0);
    CHECK(transport->generation == 1);
}

TEST_CASE("TLS transport publishes short lower reads before racing cancellation",
          "[tls][transport][generic][issue-1244]") {
    auto state = std::make_shared<scripted_lower_state>();
    state->reads.push_back(elio::io::io_result{5, 0});
    state->read_bytes = bytes_of("hello");
    elio::coro::cancel_source cancel;
    state->callback_context = &cancel;
    state->after_positive_read = cancel_source_callback;
    auto transport = make_scripted_transport(state);
    std::unique_ptr<BIO, decltype(&BIO_free)> input(BIO_new(BIO_s_mem()), BIO_free);
    REQUIRE(input);
    transport->input = input.get();

    auto wait = transport->wait_read(transport->generation, cancel.get_token());
    auto handle = task_access::handle(wait);
    handle.resume();

    REQUIRE(handle.done());
    CHECK(wait.await_resume().result == 0);
    CHECK(cancel.is_cancelled());
    CHECK(state->read_calls == 1);
    std::array<char, 5> received{};
    REQUIRE(BIO_read(input.get(), received.data(), static_cast<int>(received.size())) == 5);
    CHECK(std::string_view(received.data(), received.size()) == "hello");
    CHECK(transport->output.error() == 0);
}

TEST_CASE("TLS transport drains short lower writes before racing cancellation",
          "[tls][transport][generic][issue-1244]") {
    auto state = std::make_shared<scripted_lower_state>();
    state->writes.push_back(elio::io::io_result{2, 0});
    elio::coro::cancel_source cancel;
    counted_cancel race{&cancel, 2};
    state->callback_context = &race;
    state->after_positive_write = counted_cancel_callback;
    auto transport = make_scripted_transport(state);
    std::unique_ptr<BIO, decltype(&BIO_free)> output(transport->output.make_bio(), BIO_free);
    REQUIRE(output);
    REQUIRE(BIO_write(output.get(), "abcdef", 6) == 6);

    elio::io::io_result flushed{};
    run_transport_on_scheduler([&]() -> elio::coro::task<void> {
        flushed = co_await transport->flush_to(6, cancel.get_token());
    });

    CHECK(flushed.result == 0);
    CHECK(cancel.is_cancelled());
    CHECK(race.calls == 2);
    CHECK(state->write_calls == 2);
    CHECK(text_of(state->written_bytes) == "abcdef");
    CHECK(transport->output.error() == 0);
    CHECK(transport->output.drained_bytes() == 6);
}

TEST_CASE("TLS transport treats lower zero writes as terminal no-progress failure",
          "[tls][transport][generic][issue-1244]") {
    auto state = std::make_shared<scripted_lower_state>();
    state->writes.push_back(elio::io::io_result{0, 0});
    auto transport = make_scripted_transport(state);
    std::unique_ptr<BIO, decltype(&BIO_free)> output(transport->output.make_bio(), BIO_free);
    REQUIRE(output);
    REQUIRE(BIO_write(output.get(), "cipher", 6) == 6);

    elio::io::io_result flushed{};
    run_transport_on_scheduler([&]() -> elio::coro::task<void> {
        flushed = co_await transport->flush_to(6, {});
    });

    CHECK(flushed.result == -EPIPE);
    CHECK(state->write_calls == 1);
    CHECK(state->written_bytes.empty());
    CHECK(transport->output.error() == EPIPE);
}

TEST_CASE("TLS transport ignores throwing optional shutdown hooks from noexcept failure",
          "[tls][transport][generic][issue-1244]") {
    auto state = std::make_shared<scripted_lower_state>();
    state->shutdown_throws = true;
    auto transport = make_scripted_transport(state);

    REQUIRE_NOTHROW(transport->fail(ECANCELED));

    CHECK(state->shutdowns == 0);
    CHECK(transport->output.error() == ECANCELED);
}

TEST_CASE("TLS stream shutdown_socket falls back when lower shutdown hook may throw",
          "[tls][generic][issue-1244]") {
    elio::tls::tls_context context(elio::tls::tls_mode::client);
    auto state = std::make_shared<scripted_lower_state>();
    state->shutdown_throws = true;
    elio::tls::basic_tls_stream<scripted_lower_stream> stream(
        scripted_lower_stream{state}, context);

    REQUIRE_NOTHROW(stream.shutdown_socket());

    CHECK(state->shutdowns == 0);
    CHECK(stream.shutdown_state_for_test().transport_error == ECANCELED);
}

TEST_CASE("TLS abort settles local output before rethrowing lower abort failure",
          "[tls][generic][issue-1244]") {
    elio::tls::tls_context context(elio::tls::tls_mode::client);
    auto state = std::make_shared<scripted_lower_state>();
    state->abort_throws = true;
    elio::tls::basic_tls_stream<scripted_lower_stream> stream(
        scripted_lower_stream{state}, context);
    stream.set_output_active_for_test(true);

    auto abort = stream.abort_and_settle();
    auto handle = task_access::handle(abort);
    handle.resume();

    REQUIRE(state->aborts == 1);
    REQUIRE_FALSE(handle.done());
    CHECK(stream.shutdown_state_for_test().pump_active);

    stream.set_output_active_for_test(false);
    REQUIRE(handle.done());
    REQUIRE_THROWS_AS(abort.await_resume(), std::runtime_error);
    CHECK_FALSE(stream.shutdown_state_for_test().pump_active);
    CHECK(stream.shutdown_state_for_test().transport_error == ECANCELED);
}

TEST_CASE("TLS output launch failure releases pump ownership and wakes waiters", "[tls][transport][issue-1215]") {
    auto transport = notification_transport();
    transport->output.set_test_hooks({nullptr,
        [](void*, int, const void*, size_t, int) -> ssize_t { errno = EAGAIN; return -1; }, nullptr});
    std::unique_ptr<BIO, decltype(&BIO_free)> bio(transport->output.make_bio(), BIO_free);
    REQUIRE(bio);
    REQUIRE(BIO_write(bio.get(), "ciphertext", 10) == 10);
    auto wait = transport->wait_change(0, {});
    auto handle = task_access::handle(wait);
    handle.resume();
    const bool parked = !handle.done();
    // No scheduler is installed on this thread: failure cannot strand an
    // admitted queue or a waiter behind a pump that will never start.
    transport->start_output();
    REQUIRE(handle.done());
    CHECK(parked);
    CHECK(wait.await_resume().result == 0);
    CHECK(transport->output.error() == EIO);
    CHECK_FALSE(transport->output_active_for_test());
    auto settled = transport->settle_output();
    REQUIRE(settled.await_ready());
    settled.await_resume();
}

TEST_CASE("TLS terminal output settlement reserves both waiters without allocation", "[tls][transport][issue-1215]") {
    auto transport = notification_transport();
    // Hold the pump-active boundary, not a fake kernel completion. This test
    // isolates cleanup publication from the independently tested native pump.
    transport->set_output_active_for_test(true);
    transport->fail(ENOMEM);
    auto settle = [&]() -> elio::coro::task<void> {
        co_await transport->settle_output();
    };
    auto first = settle();
    auto second = settle();
    auto first_handle = task_access::handle(first);
    auto second_handle = task_access::handle(second);
    const auto allocations = elio::sync::detail::wake_state_allocations_for_test.load();
    elio::sync::detail::fail_next_wake_state_allocation_for_test.store(true);
    first_handle.resume();
    second_handle.resume();
    const bool both_parked = !first_handle.done() && !second_handle.done();
    transport->set_output_active_for_test(false);
    transport->notify_progress();
    const bool allocation_unused =
        elio::sync::detail::fail_next_wake_state_allocation_for_test.exchange(false);
    REQUIRE(first_handle.done());
    REQUIRE(second_handle.done());
    first.await_resume();
    second.await_resume();
    CHECK(both_parked);
    CHECK(allocation_unused);
    CHECK(elio::sync::detail::wake_state_allocations_for_test.load() == allocations);
    CHECK(transport->output.error() == ENOMEM);
}

TEST_CASE("TLS moved-from transport queries and socket shutdown remain inert", "[tls][transport][move][issue-1215]") {
    int sockets[2];
    REQUIRE(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, sockets) == 0);
    elio::net::tcp_stream source_tcp(sockets[0]);
    elio::net::tcp_stream peer(sockets[1]);
    elio::tls::tls_context context(elio::tls::tls_mode::client);
    elio::tls::tls_stream source(std::move(source_tcp), context);
    const int descriptor = source.fd();
    elio::tls::tls_stream target(std::move(source));
    CHECK(source.fd() == -1);
    CHECK(source.tcp().fd() == -1);
    CHECK_FALSE(source.is_handshake_complete());
    source.shutdown_socket();
    auto close = source.shutdown();
    auto handle = task_access::handle(close);
    handle.resume();
    REQUIRE(handle.done());
    close.await_resume();
    CHECK(target.fd() == descriptor);
    source = std::move(target);
    CHECK(target.fd() == -1);
    CHECK(target.tcp().fd() == -1);
    target.shutdown_socket();
    CHECK(source.fd() == descriptor);
}
#endif
