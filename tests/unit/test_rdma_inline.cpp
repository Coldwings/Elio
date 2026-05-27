// Stage S5b — inline send precondition.
//
// `send_flags::inline_send` requests the backend stage the payload
// inline rather than DMA-fetching it from registered memory. The
// limit is per-QP, surfaced through `connection_config::max_inline_data`.
//
// The awaiter (send_awaitable + rdma_write_awaitable) performs a
// fail-fast size check BEFORE calling the backend:
//   * total SGE bytes > max_inline_data → synthesise
//     `local_length_error`, stash the offending byte count in
//     `imm_data`, and resume the coroutine inline without posting
//     anything to the backend.
//   * total bytes ≤ max_inline_data → call the backend with the flag
//     intact and let the backend handle the inline path.
//
// Default `max_inline_data = 0` matches the safest case: any inline
// request is rejected unless the user opted in.

#include <catch2/catch_test_macros.hpp>

#include <elio/rdma/rdma.hpp>

#include <atomic>
#include <array>
#include <coroutine>
#include <cstdint>
#include <span>

using elio::rdma::buffer_view;
using elio::rdma::connection;
using elio::rdma::connection_config;
using elio::rdma::dispatcher;
using elio::rdma::polymorphic_backend;
using elio::rdma::remote_buffer;
using elio::rdma::send_flags;
using elio::rdma::sge;
using elio::rdma::wc_result;
using elio::rdma::wc_status;
using elio::rdma::wr_id;

namespace {

struct inline_state {
    std::atomic<int> sends{0};
    std::atomic<int> writes{0};
    wr_id            last_id{0};
    send_flags       last_flags{};
};

struct inline_static_backend {
    static inline inline_state* state = nullptr;

    static int post_send(void*, std::span<const sge>,
                         send_flags flags, wr_id id) noexcept {
        if (state) {
            state->sends.fetch_add(1);
            state->last_id    = id;
            state->last_flags = flags;
        }
        return 0;
    }
    static int post_recv(void*, std::span<const sge>, wr_id) noexcept {
        return 0;
    }
    static int post_rdma_write(void*, std::span<const sge>,
                               remote_buffer, send_flags flags,
                               wr_id id) noexcept {
        if (state) {
            state->writes.fetch_add(1);
            state->last_id    = id;
            state->last_flags = flags;
        }
        return 0;
    }
    static int post_rdma_read(void*, std::span<const sge>,
                              remote_buffer, wr_id) noexcept {
        return 0;
    }
};

struct state_guard {
    explicit state_guard(inline_state* s) noexcept {
        inline_static_backend::state = s;
    }
    ~state_guard() noexcept { inline_static_backend::state = nullptr; }
    state_guard(const state_guard&) = delete;
    state_guard& operator=(const state_guard&) = delete;
};

struct probe_task {
    struct promise_type {
        probe_task get_return_object() noexcept {
            return probe_task{
                std::coroutine_handle<promise_type>::from_promise(*this)};
        }
        std::suspend_never initial_suspend() noexcept { return {}; }
        std::suspend_always final_suspend() noexcept { return {}; }
        void return_void() noexcept {}
        void unhandled_exception() {}
    };
    std::coroutine_handle<promise_type> handle;
    explicit probe_task(std::coroutine_handle<promise_type> h) noexcept
        : handle(h) {}
    probe_task(const probe_task&) = delete;
    probe_task& operator=(const probe_task&) = delete;
    probe_task(probe_task&&) = delete;
    probe_task& operator=(probe_task&&) = delete;
    ~probe_task() { if (handle) handle.destroy(); }
};

template <typename Conn>
probe_task run_send_buf(Conn& c, buffer_view buf, send_flags flags,
                        wc_result& out, bool& done) {
    out  = co_await c.send(buf, flags);
    done = true;
}

template <typename Conn>
probe_task run_send_span(Conn& c, std::span<const sge> sges,
                         send_flags flags, wc_result& out, bool& done) {
    out  = co_await c.send(sges, flags);
    done = true;
}

template <typename Conn>
probe_task run_write_buf(Conn& c, buffer_view buf, remote_buffer remote,
                         send_flags flags, wc_result& out, bool& done) {
    out  = co_await c.rdma_write(buf, remote, flags);
    done = true;
}

}  // namespace

TEST_CASE("inline send: payload within max_inline_data posts normally",
          "[rdma][send][inline]") {
    inline_state st;
    state_guard guard{&st};
    dispatcher disp;
    int qp_value = 1;
    connection<inline_static_backend> c{
        &qp_value, disp, connection_config{.max_inline_data = 256}};

    char buf[200] = {};
    buffer_view bv{buf, sizeof(buf), 0xAB};

    send_flags flags{};
    flags.inline_send = true;
    wc_result result{};
    bool done = false;
    auto task = run_send_buf(c, bv, flags, result, done);

    REQUIRE_FALSE(done);
    REQUIRE(st.sends.load() == 1);
    REQUIRE(st.last_flags.inline_send);

    disp.deliver(st.last_id, wc_status::success, 200);
    REQUIRE(done);
    REQUIRE(result.ok());
}

TEST_CASE("inline send: payload exactly at max_inline_data still posts",
          "[rdma][send][inline][boundary]") {
    inline_state st;
    state_guard guard{&st};
    dispatcher disp;
    int qp_value = 2;
    connection<inline_static_backend> c{
        &qp_value, disp, connection_config{.max_inline_data = 64}};

    char buf[64] = {};
    buffer_view bv{buf, sizeof(buf), 0};

    send_flags flags{};
    flags.inline_send = true;
    wc_result result{};
    bool done = false;
    auto task = run_send_buf(c, bv, flags, result, done);

    REQUIRE_FALSE(done);
    REQUIRE(st.sends.load() == 1);
    disp.deliver(st.last_id, wc_status::success, 64);
    REQUIRE(done);
    REQUIRE(result.ok());
}

TEST_CASE("inline send: payload beyond max_inline_data is rejected pre-post",
          "[rdma][send][inline][reject]") {
    inline_state st;
    state_guard guard{&st};
    dispatcher disp;
    int qp_value = 3;
    connection<inline_static_backend> c{
        &qp_value, disp, connection_config{.max_inline_data = 128}};

    char buf[256] = {};
    buffer_view bv{buf, sizeof(buf), 0};

    send_flags flags{};
    flags.inline_send = true;
    wc_result result{};
    bool done = false;
    auto task = run_send_buf(c, bv, flags, result, done);

    // Awaiter rejected BEFORE calling the backend; no WR posted, no
    // CQE will arrive, coroutine resumed inline.
    REQUIRE(done);
    REQUIRE(st.sends.load() == 0);
    REQUIRE_FALSE(result.ok());
    REQUIRE(result.status == wc_status::local_length_error);
    REQUIRE(result.imm_data == 256u);  // offending payload size
}

TEST_CASE("inline send: multi-SGE total exceeds limit → rejected",
          "[rdma][send][inline][sge]") {
    inline_state st;
    state_guard guard{&st};
    dispatcher disp;
    int qp_value = 4;
    connection<inline_static_backend> c{
        &qp_value, disp, connection_config{.max_inline_data = 100}};

    char a[60] = {};
    char b[60] = {};
    std::array<sge, 2> sges{
        sge{a, 60, 0},
        sge{b, 60, 0},
    };

    send_flags flags{};
    flags.inline_send = true;
    wc_result result{};
    bool done = false;
    auto task = run_send_span(c, std::span<const sge>(sges),
                              flags, result, done);

    REQUIRE(done);
    REQUIRE(st.sends.load() == 0);
    REQUIRE(result.status == wc_status::local_length_error);
    REQUIRE(result.imm_data == 120u);
}

TEST_CASE("send without inline flag ignores max_inline_data entirely",
          "[rdma][send][inline]") {
    inline_state st;
    state_guard guard{&st};
    dispatcher disp;
    int qp_value = 5;
    // max_inline_data=0 (the default for unspecified config) would
    // reject any inline request. Without the flag, the awaiter posts
    // regardless of size.
    connection<inline_static_backend> c{&qp_value, disp};

    char buf[4096] = {};
    buffer_view bv{buf, sizeof(buf), 0};

    wc_result result{};
    bool done = false;
    auto task = run_send_buf(c, bv, send_flags{}, result, done);

    REQUIRE_FALSE(done);
    REQUIRE(st.sends.load() == 1);
    REQUIRE_FALSE(st.last_flags.inline_send);
    disp.deliver(st.last_id, wc_status::success, sizeof(buf));
    REQUIRE(done);
    REQUIRE(result.ok());
}

TEST_CASE("inline send: default max_inline_data=0 rejects any inline request",
          "[rdma][send][inline][default]") {
    inline_state st;
    state_guard guard{&st};
    dispatcher disp;
    int qp_value = 6;
    connection<inline_static_backend> c{&qp_value, disp};

    char buf[1] = {};
    buffer_view bv{buf, 1, 0};

    send_flags flags{};
    flags.inline_send = true;
    wc_result result{};
    bool done = false;
    auto task = run_send_buf(c, bv, flags, result, done);

    REQUIRE(done);
    REQUIRE(st.sends.load() == 0);
    REQUIRE(result.status == wc_status::local_length_error);
    REQUIRE(result.imm_data == 1u);
}

TEST_CASE("inline rdma_write: payload within max_inline_data posts normally",
          "[rdma][write][inline]") {
    inline_state st;
    state_guard guard{&st};
    dispatcher disp;
    int qp_value = 7;
    connection<inline_static_backend> c{
        &qp_value, disp, connection_config{.max_inline_data = 256}};

    char buf[100] = {};
    buffer_view bv{buf, sizeof(buf), 0};
    remote_buffer remote{0xDEAD, 100, 0xBEEF};

    send_flags flags{};
    flags.inline_send = true;
    wc_result result{};
    bool done = false;
    auto task = run_write_buf(c, bv, remote, flags, result, done);

    REQUIRE_FALSE(done);
    REQUIRE(st.writes.load() == 1);
    REQUIRE(st.last_flags.inline_send);
    disp.deliver(st.last_id, wc_status::success, 0);
    REQUIRE(done);
}

TEST_CASE("inline rdma_write: oversized payload rejected pre-post",
          "[rdma][write][inline][reject]") {
    inline_state st;
    state_guard guard{&st};
    dispatcher disp;
    int qp_value = 8;
    connection<inline_static_backend> c{
        &qp_value, disp, connection_config{.max_inline_data = 128}};

    char buf[256] = {};
    buffer_view bv{buf, sizeof(buf), 0};
    remote_buffer remote{0xCAFE, 256, 0xBABE};

    send_flags flags{};
    flags.inline_send = true;
    wc_result result{};
    bool done = false;
    auto task = run_write_buf(c, bv, remote, flags, result, done);

    REQUIRE(done);
    REQUIRE(st.writes.load() == 0);
    REQUIRE(result.status == wc_status::local_length_error);
    REQUIRE(result.imm_data == 256u);
}
