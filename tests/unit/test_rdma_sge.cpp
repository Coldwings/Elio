// Stage S5a — multi-SGE awaiter coverage.
//
// Each data-path operation (send / recv / rdma_write / rdma_read) now
// accepts either a single `buffer_view` (convenience) or an
// `std::span<const sge>` (scatter-gather). These tests verify the
// span path threads through to the backend verbatim while the
// single-buffer path keeps working unchanged.

#include <catch2/catch_test_macros.hpp>

#include <elio/rdma/rdma.hpp>

#include <atomic>
#include <array>
#include <coroutine>
#include <cstdint>
#include <span>
#include <vector>

using elio::rdma::buffer_view;
using elio::rdma::connection;
using elio::rdma::dispatcher;
using elio::rdma::polymorphic_backend;
using elio::rdma::remote_buffer;
using elio::rdma::send_flags;
using elio::rdma::sge;
using elio::rdma::wc_result;
using elio::rdma::wc_status;
using elio::rdma::wr_id;

namespace {

struct sge_state {
    std::atomic<int>  posts{0};
    std::vector<sge>  last_sges;
    wr_id             last_id{0};
    remote_buffer     last_remote{};
    send_flags        last_flags{};
    void*             last_qp = nullptr;
};

struct sge_static_backend {
    static inline sge_state* state = nullptr;

    static void record_(void* qp, std::span<const sge> sges, wr_id id) noexcept {
        if (!state) return;
        state->posts.fetch_add(1);
        state->last_qp   = qp;
        state->last_id   = id;
        state->last_sges.assign(sges.begin(), sges.end());
    }
    static int post_send(void* qp, std::span<const sge> sges,
                         send_flags flags, wr_id id) noexcept {
        record_(qp, sges, id);
        if (state) state->last_flags = flags;
        return 0;
    }
    static int post_recv(void* qp, std::span<const sge> sges,
                         wr_id id) noexcept {
        record_(qp, sges, id);
        return 0;
    }
    static int post_rdma_write(void* qp, std::span<const sge> sges,
                               remote_buffer rb, send_flags flags,
                               wr_id id) noexcept {
        record_(qp, sges, id);
        if (state) {
            state->last_remote = rb;
            state->last_flags  = flags;
        }
        return 0;
    }
    static int post_rdma_read(void* qp, std::span<const sge> sges,
                              remote_buffer rb, wr_id id) noexcept {
        record_(qp, sges, id);
        if (state) state->last_remote = rb;
        return 0;
    }
};

struct state_guard {
    explicit state_guard(sge_state* s) noexcept {
        sge_static_backend::state = s;
    }
    ~state_guard() noexcept { sge_static_backend::state = nullptr; }
    state_guard(const state_guard&) = delete;
    state_guard& operator=(const state_guard&) = delete;
};

struct sge_poly_backend : polymorphic_backend {
    sge_state state;
    int post_send(void* qp, std::span<const sge> sges,
                  send_flags flags, wr_id id) noexcept override {
        state.posts.fetch_add(1);
        state.last_qp = qp;
        state.last_id = id;
        state.last_sges.assign(sges.begin(), sges.end());
        state.last_flags = flags;
        return 0;
    }
    int post_recv(void* qp, std::span<const sge> sges,
                  wr_id id) noexcept override {
        state.posts.fetch_add(1);
        state.last_qp = qp;
        state.last_id = id;
        state.last_sges.assign(sges.begin(), sges.end());
        return 0;
    }
    int post_rdma_write(void* qp, std::span<const sge> sges,
                        remote_buffer rb, send_flags flags,
                        wr_id id) noexcept override {
        state.posts.fetch_add(1);
        state.last_qp = qp;
        state.last_id = id;
        state.last_sges.assign(sges.begin(), sges.end());
        state.last_remote = rb;
        state.last_flags  = flags;
        return 0;
    }
    int post_rdma_read(void* qp, std::span<const sge> sges,
                       remote_buffer rb, wr_id id) noexcept override {
        state.posts.fetch_add(1);
        state.last_qp = qp;
        state.last_id = id;
        state.last_sges.assign(sges.begin(), sges.end());
        state.last_remote = rb;
        return 0;
    }
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
probe_task run_send_span(Conn& c, std::span<const sge> sges,
                         send_flags flags, wc_result& out, bool& done) {
    out  = co_await c.send(sges, flags);
    done = true;
}

template <typename Conn>
probe_task run_recv_span(Conn& c, std::span<const sge> sges,
                         wc_result& out, bool& done) {
    out  = co_await c.recv(sges);
    done = true;
}

template <typename Conn>
probe_task run_write_span(Conn& c, std::span<const sge> locals,
                          remote_buffer remote, send_flags flags,
                          wc_result& out, bool& done) {
    out  = co_await c.rdma_write(locals, remote, flags);
    done = true;
}

template <typename Conn>
probe_task run_read_span(Conn& c, std::span<const sge> locals,
                         remote_buffer remote,
                         wc_result& out, bool& done) {
    out  = co_await c.rdma_read(locals, remote);
    done = true;
}

}  // namespace

TEST_CASE("send: multi-SGE span threads through to the backend verbatim",
          "[rdma][send][sge]") {
    sge_state st;
    state_guard guard{&st};
    dispatcher disp;
    int qp_value = 1;
    connection<sge_static_backend> c{&qp_value, disp};

    alignas(8) char a[16] = {};
    alignas(8) char b[24] = {};
    alignas(8) char d[8]  = {};
    std::array<sge, 3> sges{
        sge{a, 16, 0xAA},
        sge{b, 24, 0xBB},
        sge{d,  8, 0xCC},
    };

    wc_result result{};
    bool done = false;
    auto task = run_send_span(c, std::span<const sge>(sges),
                              send_flags{}, result, done);

    REQUIRE_FALSE(done);
    REQUIRE(st.posts.load() == 1);
    REQUIRE(st.last_qp == &qp_value);
    REQUIRE(st.last_sges.size() == 3u);
    REQUIRE(st.last_sges[0].addr == a);
    REQUIRE(st.last_sges[0].length == 16u);
    REQUIRE(st.last_sges[0].lkey == 0xAAu);
    REQUIRE(st.last_sges[1].addr == b);
    REQUIRE(st.last_sges[1].length == 24u);
    REQUIRE(st.last_sges[1].lkey == 0xBBu);
    REQUIRE(st.last_sges[2].addr == d);
    REQUIRE(st.last_sges[2].length == 8u);
    REQUIRE(st.last_sges[2].lkey == 0xCCu);

    disp.deliver(st.last_id, wc_status::success, 48);
    REQUIRE(done);
    REQUIRE(result.ok());
    REQUIRE(result.byte_len == 48u);
}

TEST_CASE("recv: multi-SGE span threads through to the backend verbatim",
          "[rdma][recv][sge]") {
    sge_state st;
    state_guard guard{&st};
    dispatcher disp;
    int qp_value = 2;
    connection<sge_static_backend> c{&qp_value, disp};

    alignas(8) char a[32] = {};
    alignas(8) char b[64] = {};
    std::array<sge, 2> sges{
        sge{a, 32, 0x11},
        sge{b, 64, 0x22},
    };

    wc_result result{};
    bool done = false;
    auto task = run_recv_span(c, std::span<const sge>(sges), result, done);

    REQUIRE_FALSE(done);
    REQUIRE(st.posts.load() == 1);
    REQUIRE(st.last_sges.size() == 2u);
    REQUIRE(st.last_sges[0].addr == a);
    REQUIRE(st.last_sges[1].addr == b);

    disp.deliver(st.last_id, wc_status::success, 80);
    REQUIRE(done);
    REQUIRE(result.byte_len == 80u);
}

TEST_CASE("rdma_write: multi-SGE gather with remote_buffer + flags",
          "[rdma][write][sge]") {
    sge_state st;
    state_guard guard{&st};
    dispatcher disp;
    int qp_value = 3;
    connection<sge_static_backend> c{&qp_value, disp};

    alignas(8) char a[8] = {};
    alignas(8) char b[16] = {};
    std::array<sge, 2> sges{
        sge{a,  8, 0xA},
        sge{b, 16, 0xB},
    };
    remote_buffer remote{
        .addr = 0xDEAD'BEEF'CAFE'BABEull,
        .length = 24,
        .rkey   = 0x99,
    };
    send_flags flags{};
    flags.fence = true;

    wc_result result{};
    bool done = false;
    auto task = run_write_span(c, std::span<const sge>(sges),
                               remote, flags, result, done);

    REQUIRE_FALSE(done);
    REQUIRE(st.last_sges.size() == 2u);
    REQUIRE(st.last_remote.addr == remote.addr);
    REQUIRE(st.last_remote.rkey == 0x99u);
    REQUIRE(st.last_flags.fence);

    disp.deliver(st.last_id, wc_status::success, 0);
    REQUIRE(done);
}

TEST_CASE("rdma_read: multi-SGE scatter with remote_buffer",
          "[rdma][read][sge]") {
    sge_state st;
    state_guard guard{&st};
    dispatcher disp;
    int qp_value = 4;
    connection<sge_static_backend> c{&qp_value, disp};

    alignas(8) char a[128] = {};
    alignas(8) char b[64]  = {};
    std::array<sge, 2> sges{
        sge{a, 128, 0x1},
        sge{b,  64, 0x2},
    };
    remote_buffer remote{
        .addr   = 0x1111'2222'3333'4444ull,
        .length = 192,
        .rkey   = 0x77,
    };

    wc_result result{};
    bool done = false;
    auto task = run_read_span(c, std::span<const sge>(sges),
                              remote, result, done);

    REQUIRE_FALSE(done);
    REQUIRE(st.last_sges.size() == 2u);
    REQUIRE(st.last_remote.length == 192u);

    disp.deliver(st.last_id, wc_status::success, 150);
    REQUIRE(done);
    REQUIRE(result.byte_len == 150u);
}

TEST_CASE("send: single-buffer_view overload delivers exactly one SGE",
          "[rdma][send][sge][single]") {
    sge_state st;
    state_guard guard{&st};
    dispatcher disp;
    int qp_value = 5;
    connection<sge_static_backend> c{&qp_value, disp};

    char buf[4] = {};
    buffer_view bv{buf, 4, 0xFEED};

    wc_result result{};
    bool done = false;
    // Goes through the buffer_view overload, NOT the span overload —
    // covers the sge_holder single-inline mode.
    auto run = [&](buffer_view local) -> probe_task {
        result = co_await c.send(local);
        done   = true;
    };
    auto task = run(bv);

    REQUIRE_FALSE(done);
    REQUIRE(st.last_sges.size() == 1u);
    REQUIRE(st.last_sges[0].addr == buf);
    REQUIRE(st.last_sges[0].length == 4u);
    REQUIRE(st.last_sges[0].lkey == 0xFEEDu);

    disp.deliver(st.last_id, wc_status::success, 4);
    REQUIRE(done);
}

TEST_CASE("polymorphic send: multi-SGE dispatches through vtable",
          "[rdma][send][sge][polymorphic]") {
    sge_poly_backend impl{};
    dispatcher disp;
    int qp_value = 9;
    connection<polymorphic_backend> c{&qp_value, impl, disp};

    alignas(8) char a[16] = {};
    alignas(8) char b[16] = {};
    std::array<sge, 2> sges{
        sge{a, 16, 0xAB},
        sge{b, 16, 0xCD},
    };

    wc_result result{};
    bool done = false;
    auto task = run_send_span(c, std::span<const sge>(sges),
                              send_flags{}, result, done);

    REQUIRE_FALSE(done);
    REQUIRE(impl.state.posts.load() == 1);
    REQUIRE(impl.state.last_sges.size() == 2u);
    REQUIRE(impl.state.last_sges[1].lkey == 0xCDu);

    disp.deliver(impl.state.last_id, wc_status::success, 32);
    REQUIRE(done);
    REQUIRE(result.byte_len == 32u);
}

TEST_CASE("polymorphic rdma_write: multi-SGE dispatches through vtable",
          "[rdma][write][sge][polymorphic]") {
    sge_poly_backend impl{};
    dispatcher disp;
    int qp_value = 10;
    connection<polymorphic_backend> c{&qp_value, impl, disp};

    alignas(8) char a[8] = {};
    alignas(8) char b[8] = {};
    std::array<sge, 2> sges{
        sge{a, 8, 0x1},
        sge{b, 8, 0x2},
    };
    remote_buffer remote{0xAAAA'BBBB, 16, 0x33};

    wc_result result{};
    bool done = false;
    auto task = run_write_span(c, std::span<const sge>(sges),
                               remote, send_flags{}, result, done);

    REQUIRE_FALSE(done);
    REQUIRE(impl.state.last_sges.size() == 2u);
    REQUIRE(impl.state.last_remote.rkey == 0x33u);

    disp.deliver(impl.state.last_id, wc_status::success, 0);
    REQUIRE(done);
}

TEST_CASE("empty SGE span posts a zero-length scatter list",
          "[rdma][sge][empty]") {
    sge_state st;
    state_guard guard{&st};
    dispatcher disp;
    int qp_value = 11;
    connection<sge_static_backend> c{&qp_value, disp};

    std::array<sge, 0> empty{};
    wc_result result{};
    bool done = false;
    auto task = run_send_span(c, std::span<const sge>(empty),
                              send_flags{}, result, done);

    REQUIRE_FALSE(done);
    REQUIRE(st.last_sges.size() == 0u);

    // Backends typically reject this at post time; here the mock
    // returned 0, so we still need to deliver to drive the awaiter
    // to completion.
    disp.deliver(st.last_id, wc_status::success, 0);
    REQUIRE(done);
}
