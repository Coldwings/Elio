#include <catch2/catch_test_macros.hpp>

#include <elio/http/detail/owned_prefix_stream.hpp>
#include <elio/http/detail/route_connection.hpp>
#include <elio/sync/event.hpp>

#include <array>
#include <atomic>
#include <cstring>
#include <fcntl.h>
#include <memory>
#include <string>
#include <vector>

using elio::coro::task;
using elio::http::detail::owned_prefix_stream;

namespace {

template<typename T>
T immediate(task<T> operation) {
    auto handle = elio::coro::detail::task_access::handle(operation);
    handle.resume();
    REQUIRE(handle.done());
    return operation.await_resume();
}

struct observation {
    std::string input = "tail";
    std::string output;
    size_t input_offset = 0;
    size_t reads = 0;
    size_t writes = 0;
    size_t finishes = 0;
    size_t aborts = 0;
    bool zero_write = false;
    bool pause = false;
    bool throw_abort = false;
    std::atomic<bool> sealed{false};
    elio::sync::event read_entered;
    elio::sync::event write_entered;
    elio::sync::event proceed;
};

class scripted_lower {
public:
    using byte_stream_contract = elio::net::publishing_byte_stream_contract;

    explicit scripted_lower(std::shared_ptr<observation> value) : observed_(std::move(value)) {}
    scripted_lower(scripted_lower&&) noexcept = default;
    scripted_lower& operator=(scripted_lower&&) noexcept = default;
    scripted_lower(const scripted_lower&) = delete;
    scripted_lower& operator=(const scripted_lower&) = delete;

    task<elio::io::io_result> read(void* data, size_t size, elio::coro::cancel_token token) {
        ++observed_->reads;
        observed_->read_entered.set();
        if (observed_->pause && co_await observed_->proceed.wait(token) ==
                elio::coro::cancel_result::cancelled)
            co_return elio::io::io_result{-ECANCELED, 0};
        const auto count = std::min(size, observed_->input.size() - observed_->input_offset);
        std::memcpy(data, observed_->input.data() + observed_->input_offset, count);
        observed_->input_offset += count;
        co_return elio::io::io_result{static_cast<int>(count), 0};
    }
    task<elio::io::io_result> write(const void* data, size_t size,
                                   elio::coro::cancel_token token) {
        ++observed_->writes;
        observed_->write_entered.set();
        if (observed_->pause && co_await observed_->proceed.wait(token) ==
                elio::coro::cancel_result::cancelled)
            co_return elio::io::io_result{-ECANCELED, 0};
        const auto count = observed_->zero_write ? size_t{0} : std::min(size, size_t{2});
        observed_->output.append(static_cast<const char*>(data), count);
        co_return elio::io::io_result{static_cast<int>(count), 0};
    }
    task<elio::net::write_finish_result> finish_write(elio::coro::cancel_token,
            std::chrono::milliseconds) {
        ++observed_->finishes;
        co_return elio::net::write_finish_result{elio::net::close_scope::whole_session, 0,
                                                true, true};
    }
    elio::net::close_scope read_end_scope() const noexcept {
        return elio::net::close_scope::whole_session;
    }
    void shutdown_socket() noexcept { observed_->sealed.store(true); }
    task<void> abort_and_settle() {
        ++observed_->aborts;
        observed_->sealed.store(true);
        if (observed_->throw_abort) throw std::runtime_error("scripted abort failure");
        co_return;
    }

private:
    std::shared_ptr<observation> observed_;
};

using channel = owned_prefix_stream<scripted_lower>;
static_assert(elio::net::publishing_byte_stream<channel>);

} // namespace

TEST_CASE("CONNECT channel consumes owned read-ahead once before lower input and EOF",
          "[http][proxy][prefix][issue-1249]") {
    auto observed = std::make_shared<observation>();
    std::vector<char> prefix{'l', 'e', 'a', 'd'};
    channel stream(scripted_lower(observed), prefix, 4);
    prefix.assign(4, 'x');
    std::array<char, 8> bytes{};
    auto first = immediate(stream.read(bytes.data(), 2, {}));
    REQUIRE(first.result == 2);
    REQUIRE(std::string_view(bytes.data(), 2) == "le");
    REQUIRE(observed->reads == 0);
    auto second = immediate(stream.read(bytes.data(), bytes.size(), {}));
    REQUIRE(second.result == 2);
    REQUIRE(std::string_view(bytes.data(), 2) == "ad");
    REQUIRE(observed->reads == 0);
    auto third = immediate(stream.read(bytes.data(), bytes.size(), {}));
    REQUIRE(third.result == 4);
    REQUIRE(std::string_view(bytes.data(), 4) == "tail");
    auto end = immediate(stream.read(bytes.data(), bytes.size(), {}));
    REQUIRE(end.result == 0);
    REQUIRE(stream.read_end_scope() == elio::net::close_scope::whole_session);
    REQUIRE(observed->reads == 2);
    REQUIRE_THROWS_AS(channel(scripted_lower(observed), {'a', 'b'}, 1),
                      std::invalid_argument);
}

TEST_CASE("CONNECT channel preserves short publication and layer-local finish",
          "[http][proxy][prefix][issue-1249]") {
    auto observed = std::make_shared<observation>();
    channel stream(scripted_lower(observed), {}, 0);
    auto write = immediate(stream.write("body", 4, {}));
    REQUIRE(write.result == 2);
    REQUIRE(observed->output == "bo");
    observed->zero_write = true;
    auto zero = immediate(stream.write("dy", 2, {}));
    REQUIRE(zero.result == -EIO);
    auto empty = immediate(stream.write("", 0, {}));
    REQUIRE(empty.result == 0);
    auto finish = immediate(stream.finish_write({}, std::chrono::milliseconds{1}));
    REQUIRE(finish.scope == elio::net::close_scope::whole_session);
    REQUIRE(finish.error == 0);
    REQUIRE(observed->finishes == 1);
    REQUIRE(observed->aborts == 0);
}

TEST_CASE("CONNECT channel cancellation preserves unconsumed prefix and move ownership",
          "[http][proxy][prefix][issue-1249]") {
    auto observed = std::make_shared<observation>();
    channel original(scripted_lower(observed), {'p'}, 1);
    elio::coro::cancel_source stop;
    stop.cancel();
    std::array<char, 1> bytes{};
    REQUIRE(immediate(original.read(bytes.data(), bytes.size(), stop.get_token())).result ==
            -ECANCELED);
    channel moved(std::move(original));
    REQUIRE(immediate(moved.read(bytes.data(), bytes.size(), {})).result == 1);
    REQUIRE(bytes[0] == 'p');
    REQUIRE(observed->reads == 0);
    REQUIRE(immediate(original.write("x", 1, {})).result == -ENOTCONN);
}

TEST_CASE("CONNECT channel abort settles overlapping sides even when lower abort throws",
          "[http][proxy][prefix][issue-1249]") {
    for (const bool throw_abort : {false, true}) {
        auto observed = std::make_shared<observation>();
        observed->pause = true;
        observed->throw_abort = throw_abort;
        channel stream(scripted_lower(observed), {}, 0);
        elio::runtime::scheduler scheduler(1);
        scheduler.start();
        auto operation = scheduler.go_joinable([&]() -> task<void> {
            std::array<char, 4> bytes{};
            auto reader = scheduler.go_joinable([&]() -> task<elio::io::io_result> {
                co_return co_await stream.read(bytes.data(), bytes.size(), {});
            });
            auto writer = scheduler.go_joinable([&]() -> task<elio::io::io_result> {
                co_return co_await stream.write("data", 4, {});
            });
            co_await observed->read_entered.wait();
            co_await observed->write_entered.wait();
            bool caught = false;
            try { co_await stream.abort_and_settle(); }
            catch (const std::runtime_error&) { caught = true; }
            REQUIRE(caught == throw_abort);
            REQUIRE((co_await reader).result == -ECANCELED);
            REQUIRE((co_await writer).result == -ECANCELED);
            co_await reader.wait_destroyed_async();
            co_await writer.wait_destroyed_async();
            REQUIRE(observed->sealed.load());
            REQUIRE(observed->aborts == 1);
            REQUIRE((co_await stream.read(bytes.data(), bytes.size(), {})).result == -ECANCELED);
            REQUIRE((co_await stream.write("data", 4, {})).result == -ECANCELED);
        });
        operation.wait_destroyed();
        scheduler.shutdown();
        operation.await_resume();
    }
}

TEST_CASE("HTTP private route connection preserves direct facade ownership and I/O",
          "[http][proxy][channel][issue-1249]") {
    using elio::http::detail::route_connection;
    route_connection absent;
    REQUIRE_FALSE(absent.is_connected());
    REQUIRE(absent.fd() == -1);
    REQUIRE(immediate(absent.write("x", 1)).result == -ENOTCONN);
    std::array<int, 2> descriptors{};
    REQUIRE(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC,
                         0, descriptors.data()) == 0);
    route_connection original(elio::net::stream(elio::net::tcp_stream{descriptors[0]}));
    elio::net::tcp_stream peer{descriptors[1]};
    route_connection moved(std::move(original));
    REQUIRE(moved.fd() == descriptors[0]);
    REQUIRE(original.fd() == -1);
    elio::runtime::scheduler scheduler(1);
    scheduler.start();
    auto operation = scheduler.go_joinable([&]() -> task<void> {
        REQUIRE((co_await moved.write_all("hello")).result == 5);
        std::array<char, 5> input{};
        REQUIRE((co_await peer.read_exactly(input.data(), input.size())).result == 5);
        REQUIRE(std::string_view(input.data(), input.size()) == "hello");
        REQUIRE((co_await peer.write_exactly("reply")).result == 5);
        REQUIRE((co_await moved.read(input.data(), input.size())).result == 5);
        REQUIRE(std::string_view(input.data(), input.size()) == "reply");
    });
    operation.wait_destroyed();
    scheduler.shutdown();
    operation.await_resume();
    auto legacy = moved.take_legacy();
    REQUIRE(legacy);
    REQUIRE(legacy->fd() == descriptors[0]);
    REQUIRE_FALSE(moved.is_connected());
    legacy->disconnect();
    REQUIRE(::fcntl(descriptors[0], F_GETFD) == -1);
}

TEST_CASE("HTTP private CONNECT TLS connection abort owns the root without exposing its fd",
          "[http][proxy][channel][issue-1249]") {
    using namespace elio::http::detail;
    std::array<int, 2> descriptors{};
    REQUIRE(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC,
                         0, descriptors.data()) == 0);
    elio::net::tcp_stream peer{descriptors[1]};
    elio::tls::tls_context context(elio::tls::tls_mode::client);
    connect_tls_stream inner(connect_channel(elio::net::tcp_stream{descriptors[0]}, {}, 0),
                             context);
    route_connection original(std::move(inner));
    route_connection moved(std::move(original));
    REQUIRE(moved.is_connected());
    REQUIRE(moved.fd() == -1);
    REQUIRE_FALSE(moved.take_legacy());
    moved.shutdown_socket();
    moved.disconnect();
    REQUIRE_FALSE(moved.is_connected());
    REQUIRE(::fcntl(descriptors[0], F_GETFD) == -1);
    std::array<char, 1> input{};
    REQUIRE(immediate(moved.read(input.data(), input.size())).result == -ENOTCONN);
}
