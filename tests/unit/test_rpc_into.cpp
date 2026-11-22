#include <catch2/catch_test_macros.hpp>
#include <elio/rpc/rpc_client.hpp>

#include <deque>
#include <future>

namespace {
using namespace elio;
using namespace elio::rpc;
using namespace std::chrono_literals;

struct reply {
    std::vector<std::string> labels;
    buffer_ref data;
    std::optional<uint32_t> generation;
    buffer_ref extra;
    buffer_ref trailer;
    ELIO_RPC_FIELDS(reply, labels, data, generation, extra, trailer)
};
using method = method_descriptor<41, empty_request, reply>;

struct peer {
    std::deque<uint8_t> input;
    sync::event readable;
    sync::event request_seen;
    sync::event reading_target;
    sync::event allow_read;
    std::vector<std::pair<void*, size_t>> reads;
    void* pause_at = nullptr;
    bool valid = true;
    bool corrupt = false;
    size_t blob_size = 5;
    bool reorder = false;
    bool hold_response = false;
    bool send_error = false;
    bool trailing_field = false;
    std::vector<uint32_t> requests;

    void respond(uint32_t id) {
        std::array<uint8_t, 8> bytes{1, 2, 3, 4, 5, 6, 7, 8};
        reply r{{"a", "variable length"}, buffer_ref(bytes.data(), blob_size),
                id, buffer_ref(bytes.data() + 6, 2), buffer_ref(bytes.data(), 1)};
        auto [h, payload] = build_response(id, r, true);
        if (send_error) std::tie(h, payload) = build_error_response(id, rpc_error::method_not_found, "missing", true);
        if (trailing_field) {
            payload.write(uint8_t{42});
            h.payload_length = static_cast<uint32_t>(payload.size());
        }
        auto prefix = h.to_bytes();
        auto crc = crc32_update(prefix.data(), prefix.size(), 0xffffffff);
        crc = crc32_finalize(crc32_update(payload.data(), payload.size(), crc));
        std::array<uint8_t, 4> checksum{};
        endian::write_le(checksum.data(), corrupt ? ~crc : crc);
        input.insert(input.end(), prefix.begin(), prefix.end());
        input.insert(input.end(), payload.data(), payload.data() + payload.size());
        input.insert(input.end(), checksum.begin(), checksum.end());
        readable.set();
    }
};

// All stream methods run on one scheduler worker. Pause an actual destination
// read before touching memory, independently of cancellation or socket close.
struct stream {
    std::shared_ptr<peer> p;
    bool is_valid() const { return p->valid; }
    void shutdown_socket() { p->valid = false; p->readable.set(); }
    coro::task<io::io_result> read_exactly(void* to, size_t n) {
        if (p->pause_at == to) {
            p->reading_target.set();
            co_await p->allow_read.wait();
        }
        p->reads.emplace_back(to, n);
        while (p->valid && p->input.size() < n) {
            p->readable.reset();
            co_await p->readable.wait();
        }
        if (!p->valid) co_return io::io_result{-ECONNRESET, 0};
        for (size_t i = 0; i < n; ++i) {
            static_cast<uint8_t*>(to)[i] = p->input.front();
            p->input.pop_front();
        }
        co_return io::io_result{static_cast<int32_t>(n), 0};
    }
    coro::task<io::io_result> writev(iovec* v, size_t count) {
        auto h = frame_header::from_bytes(static_cast<uint8_t*>(v[0].iov_base));
        if (h.type == message_type::request) {
            p->requests.push_back(h.request_id);
            p->request_seen.set();
            if (!p->reorder && !p->hold_response) p->respond(h.request_id);
            else if (p->requests.size() == 2) {
                p->respond(h.request_id);
                p->respond(p->requests[0]);
            }
        }
        size_t n = 0;
        for (size_t i = 0; i < count; ++i) n += v[i].iov_len;
        co_return io::io_result{static_cast<int32_t>(n), 0};
    }
    coro::task<io::io_result> write_exactly(const void*, size_t n) {
        co_return io::io_result{static_cast<int32_t>(n), 0};
    }
    coro::task<io::io_result> poll_write() { co_return io::io_result{0, 0}; }
};
using client = rpc_client<stream>;

template<class F>
auto run(F fn) {
    using result_type = typename std::invoke_result_t<F&, runtime::scheduler&>::value_type;
    runtime::scheduler s(1);
    s.start();
    std::promise<void> completed;
    auto signal = completed.get_future();
    auto done = s.go_joinable([&]() -> coro::task<result_type> {
        struct notify { std::promise<void>& p; ~notify() { p.set_value(); } } guard{completed};
        co_return co_await fn(s);
    });
    // Do not close scheduler admission before the client starts its read loop.
    bool finished = signal.wait_for(10s) == std::future_status::ready;
    bool drained = s.shutdown(2s);
    REQUIRE(finished);
    REQUIRE(drained);
    REQUIRE(done.is_ready());
    return done.await_resume();
}

TEST_CASE("RPC field bindings receive directly across short and variable fields", "[rpc][into]") {
    std::array<uint8_t, 4> a{}, b{}, extra{};
    a.fill(99); b.fill(99); extra.fill(99);
    size_t n = 99;
    auto p = std::make_shared<peer>();
    auto r = run([&](auto&) -> coro::task<rpc_result<reply>> {
        auto c = client::create(stream{p}, {.frame_read_timeout = 0s});
        std::array<iovec, 3> pages{{{a.data(), a.size()}, {nullptr, 0}, {b.data(), b.size()}}};
        auto result = co_await c->call<method>(empty_request{},
            bind_fields(into<&reply::extra>(extra), into<&reply::data>(pages, n)), 2s);
        c->close();
        co_return result;
    });
    REQUIRE(r.ok());
    CHECK(r->labels == std::vector<std::string>{"a", "variable length"});
    CHECK(r->generation == p->requests.front());
    CHECK(r->trailer.size() == 1);
    CHECK(r->trailer.data()[0] == 1);
    CHECK(r->data.empty());
    CHECK(n == 5);
    CHECK(a == std::array<uint8_t, 4>{1, 2, 3, 4});
    CHECK(b == std::array<uint8_t, 4>{5, 99, 99, 99});
    CHECK(r->extra.data() == extra.data());
    CHECK(r->extra.size() == 2);
    CHECK(extra == std::array<uint8_t, 4>{7, 8, 99, 99});
    for (auto [ptr, len] : std::array<std::pair<void*, size_t>, 3>{{
             {a.data(), 4}, {b.data(), 1}, {extra.data(), 2}}}) {
        CHECK(std::count(p->reads.begin(), p->reads.end(), std::pair{ptr, len}) == 1);
    }
}

TEST_CASE("RPC bound destination errors keep frame boundaries and do not commit counts", "[rpc][into]") {
    auto p = std::make_shared<peer>();
    std::array<uint8_t, 2> dst{99, 99};
    size_t n = 42;
    bool bad_crc = false;
    SECTION("capacity failure drains and a zero-length next response succeeds") {}
    SECTION("checksum failure closes the stream without success") { bad_crc = true; }
    auto errors = run([&](auto&) -> coro::task<std::pair<rpc_error, rpc_error>> {
        auto c = client::create(stream{p}, {.frame_read_timeout = 0s});
        iovec v{dst.data(), dst.size()};
        p->corrupt = bad_crc;
        if (bad_crc) p->blob_size = 2;
        auto failed = co_await c->call<method>(empty_request{}, into<&reply::data>(std::span(&v, 1), n));
        p->blob_size = 0;
        auto second = co_await c->call<method>(empty_request{}, into<&reply::data>(dst));
        c->close();
        co_return std::pair{failed.error(), second.error()};
    });
    CHECK(n == 42);
    if (bad_crc) {
        CHECK(errors.first == rpc_error::connection_closed);
        CHECK(errors.second == rpc_error::connection_closed);
    } else {
        CHECK(errors.first == rpc_error::resource_exhausted);
        CHECK(errors.second == rpc_error::success);
        CHECK(dst == std::array<uint8_t, 2>{99, 99});
    }
}

TEST_CASE("RPC bound calls route out-of-order responses by request ID", "[rpc][into]") {
    auto p = std::make_shared<peer>();
    p->reorder = true;
    std::array<uint8_t, 5> first{}, second{};
    auto results = run([&](runtime::scheduler& s) -> coro::task<std::pair<rpc_result<reply>, rpc_result<reply>>> {
        auto c = client::create(stream{p}, {.frame_read_timeout = 0s});
        empty_request req;
        auto a = s.go_joinable(c->call<method>(req, into<&reply::data>(first)));
        co_await p->request_seen.wait();
        auto b = s.go_joinable(c->call<method>(req, into<&reply::data>(second)));
        auto x = co_await std::move(a);
        auto y = co_await std::move(b);
        c->close();
        co_return std::pair{std::move(x), std::move(y)};
    });
    REQUIRE(results.first.ok());
    REQUIRE(results.second.ok());
    CHECK(results.first->data.data() == first.data());
    CHECK(results.second->data.data() == second.data());
    REQUIRE(p->requests.size() == 2);
    CHECK(results.first->generation == p->requests[0]);
    CHECK(results.second->generation == p->requests[1]);
    CHECK(first == std::array<uint8_t, 5>{1, 2, 3, 4, 5});
    CHECK(second == first);
}

TEST_CASE("RPC bound call cannot return until an admitted destination read has quiesced",
          "[rpc][into][cancel]") {
    rpc_error expected = rpc_error::cancelled;
    SECTION("caller token") {}
    SECTION("call deadline") { expected = rpc_error::timeout; }
    SECTION("connection close") { expected = rpc_error::connection_closed; }
    auto p = std::make_shared<peer>();
    std::array<uint8_t, 2> first{};
    std::array<uint8_t, 3> second{99, 99, 99};
    p->pause_at = first.data();
    size_t n = 23;
    sync::event waiting;
    using gate = elio::rpc::detail::into_gate;
    gate::quiescence_wait_for_test.store(&waiting, std::memory_order_release);
    struct reset_hook { ~reset_hook() { gate::quiescence_wait_for_test.store(nullptr); } } reset;
    auto observed = run([&](runtime::scheduler& s) -> coro::task<std::pair<bool, rpc_error>> {
        auto c = client::create(stream{p}, {.frame_read_timeout = 0s});
        empty_request req;
        coro::cancel_source stop;
        std::array<iovec, 2> pages{{{first.data(), first.size()}, {second.data(), second.size()}}};
        auto h = s.go_joinable(c->call<method>(req, into<&reply::data>(pages, n),
            expected == rpc_error::timeout ? 1s : 8s, stop.get_token()));
        co_await p->reading_target.wait();
        if (expected == rpc_error::cancelled) stop.cancel();
        if (expected == rpc_error::connection_closed) c->close();
        co_await waiting.wait();
        bool pending = !h.is_ready();
        p->allow_read.set();
        auto result = co_await std::move(h);
        c->close();
        co_return std::pair{pending, result.error()};
    });
    REQUIRE(observed.first);
    CHECK(observed.second == expected);
    CHECK(n == 23);
    CHECK(second == std::array<uint8_t, 3>{99, 99, 99});
    CHECK(std::none_of(p->reads.begin(), p->reads.end(), [&](auto r) { return r.first == second.data(); }));
}

TEST_CASE("late replies and remote errors never write a bound target", "[rpc][into]") {
    auto p = std::make_shared<peer>();
    p->hold_response = true;
    std::array<uint8_t, 5> dst{99, 99, 99, 99, 99};
    auto errors = run([&](runtime::scheduler& s) -> coro::task<std::pair<rpc_error, rpc_error>> {
        auto c = client::create(stream{p}, {.frame_read_timeout = 0s});
        empty_request req;
        coro::cancel_source stop;
        auto h = s.go_joinable(c->call<method>(req, into<&reply::data>(dst), stop.get_token()));
        co_await p->request_seen.wait();
        stop.cancel();
        auto first = co_await std::move(h);
        p->respond(p->requests.front());
        p->hold_response = false;
        p->send_error = true;
        auto second = co_await c->call<method>(req, into<&reply::data>(dst));
        c->close();
        co_return std::pair{first.error(), second.error()};
    });
    CHECK(errors.first == rpc_error::cancelled);
    CHECK(errors.second == rpc_error::method_not_found);
    CHECK(dst == std::array<uint8_t, 5>{99, 99, 99, 99, 99});
    CHECK(std::none_of(p->reads.begin(), p->reads.end(), [&](auto r) { return r.first == dst.data(); }));
}

TEST_CASE("RPC field bindings commit counts only after typed exact consumption", "[rpc][into]") {
    auto p = std::make_shared<peer>();
    std::array<uint8_t, 8> dst{};
    size_t n = 77;
    auto errors = run([&](runtime::scheduler&) -> coro::task<std::pair<rpc_error, rpc_error>> {
        auto c = client::create(stream{p}, {.frame_read_timeout = 0s});
        iovec v{dst.data(), dst.size()};
        p->trailing_field = true;
        auto r = co_await c->call<method>(empty_request{}, into<&reply::data>(std::span(&v, 1), n));
        p->trailing_field = false;
        auto next = co_await c->call<method>(empty_request{});
        c->close();
        co_return std::pair{r.error(), next.error()};
    });
    CHECK(errors.first == rpc_error::serialization_error);
    CHECK(errors.second == rpc_error::success);
    CHECK(n == 77);
}

TEST_CASE("RPC field binding uses the unchanged wire protocol on a real stream", "[rpc][into]") {
    int fds[2];
    REQUIRE(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0, fds) == 0);
    auto a = net::tcp_stream::adopt(fds[0]);
    auto b = net::tcp_stream::adopt(fds[1]);
    REQUIRE(a);
    REQUIRE(b);
    std::array<uint8_t, 4096> first{}, second{};
    size_t n = 0;
    auto ok = run([&](runtime::scheduler& s) -> coro::task<bool> {
        auto c = rpc_client<net::tcp_stream>::create(std::move(*a));
        auto remote = s.go_joinable([&]() -> coro::task<bool> {
            auto request = co_await read_frame(*b);
            if (!request) co_return false;
            std::vector<uint8_t> bytes(5000, 37);
            reply response{{"remote"}, buffer_ref(bytes), 11, {}, {}};
            auto frame = build_response(request->first.request_id, response, true);
            co_return co_await write_frame(*b, frame.first, frame.second);
        });
        std::array<iovec, 2> pages{{{first.data(), first.size()}, {second.data(), second.size()}}};
        auto r = co_await c->call<method>(empty_request{}, into<&reply::data>(pages, n));
        c->close();
        auto sent = co_await std::move(remote);
        co_return sent && r.ok() && r->generation == 11;
    });
    REQUIRE(ok);
    REQUIRE(n == 5000);
    CHECK(std::all_of(first.begin(), first.end(), [](auto v) { return v == 37; }));
    CHECK(std::all_of(second.begin(), second.begin() + 904, [](auto v) { return v == 37; }));
    CHECK(std::all_of(second.begin() + 904, second.end(), [](auto v) { return v == 0; }));
}

} // namespace
