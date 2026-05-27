// Stage S5c — Shared Receive Queue (SRQ).
//
// SRQs decouple receive-buffer credit from individual QPs: many QPs
// can draw recv WRs from one shared pool. The data path mirrors the
// per-QP recv except that:
//   * `post_srq_recv(srq_ptr, sges, wr_id)` is the backend call.
//   * The wrapper is `srq<Backend>` (and the polymorphic variant),
//     parallel to `connection<Backend>`.
//   * Completion arrives via whichever CQ consumed the WR; the
//     dispatcher routes purely by `op_state*` so SRQ vs per-QP origin
//     is invisible at delivery time.
//
// Coverage: happy path (static + polymorphic), post failure inline
// resume, orphan race under ASAN, the polymorphic default
// `-ENOTSUP` (-95) for backends that don't override `post_srq_recv`,
// and the multi-SGE overload.

#include <catch2/catch_test_macros.hpp>

#include <elio/rdma/rdma.hpp>

#include <atomic>
#include <array>
#include <coroutine>
#include <cstdint>
#include <span>
#include <vector>

using elio::rdma::buffer_view;
using elio::rdma::dispatcher;
using elio::rdma::polymorphic_backend;
using elio::rdma::remote_buffer;
using elio::rdma::send_flags;
using elio::rdma::sge;
using elio::rdma::srq;
using elio::rdma::wc_result;
using elio::rdma::wc_status;
using elio::rdma::wr_id;

namespace {

struct srq_state {
    std::atomic<int>  posts{0};
    wr_id             last_id{0};
    void*             last_srq = nullptr;
    std::vector<sge>  last_sges;
    int               return_rc = 0;
};

struct srq_static_backend {
    static inline srq_state* state = nullptr;

    static int post_send(void*, std::span<const sge>,
                         send_flags, wr_id) noexcept { return 0; }
    static int post_recv(void*, std::span<const sge>, wr_id) noexcept {
        return 0;
    }
    static int post_rdma_write(void*, std::span<const sge>,
                               remote_buffer, send_flags, wr_id) noexcept {
        return 0;
    }
    static int post_rdma_read(void*, std::span<const sge>,
                              remote_buffer, wr_id) noexcept {
        return 0;
    }
    static int post_srq_recv(void* srq_ptr, std::span<const sge> sges,
                             wr_id id) noexcept {
        if (state) {
            state->posts.fetch_add(1);
            state->last_id   = id;
            state->last_srq  = srq_ptr;
            state->last_sges.assign(sges.begin(), sges.end());
            return state->return_rc;
        }
        return 0;
    }
};

struct state_guard {
    explicit state_guard(srq_state* s) noexcept {
        srq_static_backend::state = s;
    }
    ~state_guard() noexcept { srq_static_backend::state = nullptr; }
    state_guard(const state_guard&) = delete;
    state_guard& operator=(const state_guard&) = delete;
};

struct srq_poly_backend : polymorphic_backend {
    srq_state state;

    int post_send(void*, std::span<const sge>,
                  send_flags, wr_id) noexcept override { return 0; }
    int post_recv(void*, std::span<const sge>, wr_id) noexcept override {
        return 0;
    }
    int post_rdma_write(void*, std::span<const sge>,
                        remote_buffer, send_flags, wr_id) noexcept override {
        return 0;
    }
    int post_rdma_read(void*, std::span<const sge>,
                       remote_buffer, wr_id) noexcept override {
        return 0;
    }
    int post_srq_recv(void* srq_ptr, std::span<const sge> sges,
                      wr_id id) noexcept override {
        state.posts.fetch_add(1);
        state.last_id   = id;
        state.last_srq  = srq_ptr;
        state.last_sges.assign(sges.begin(), sges.end());
        return state.return_rc;
    }
};

// A polymorphic backend that intentionally does NOT override
// post_srq_recv — should yield wr_flush_error via the -ENOTSUP path.
struct poly_no_srq : polymorphic_backend {
    int post_send(void*, std::span<const sge>,
                  send_flags, wr_id) noexcept override { return 0; }
    int post_recv(void*, std::span<const sge>, wr_id) noexcept override {
        return 0;
    }
    int post_rdma_write(void*, std::span<const sge>,
                        remote_buffer, send_flags, wr_id) noexcept override {
        return 0;
    }
    int post_rdma_read(void*, std::span<const sge>,
                       remote_buffer, wr_id) noexcept override {
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

template <typename SRQ>
probe_task run_srq_recv(SRQ& q, buffer_view buf,
                        wc_result& out, bool& done) {
    out  = co_await q.recv(buf);
    done = true;
}

template <typename SRQ>
probe_task run_srq_recv_span(SRQ& q, std::span<const sge> sges,
                             wc_result& out, bool& done) {
    out  = co_await q.recv(sges);
    done = true;
}

}  // namespace

TEST_CASE("srq<Backend>::recv posts to backend and resumes on CQE",
          "[rdma][srq]") {
    srq_state st;
    state_guard guard{&st};
    dispatcher disp;
    int srq_handle = 0xABCD;
    srq<srq_static_backend> q{&srq_handle, disp};

    char payload[32] = {};
    buffer_view bv{payload, sizeof(payload), 0x55};

    wc_result result{};
    bool done = false;
    auto task = run_srq_recv(q, bv, result, done);

    REQUIRE_FALSE(done);
    REQUIRE(st.posts.load() == 1);
    REQUIRE(st.last_srq == &srq_handle);
    REQUIRE(st.last_sges.size() == 1u);
    REQUIRE(st.last_sges[0].addr == payload);
    REQUIRE(st.last_sges[0].length == 32u);
    REQUIRE(st.last_sges[0].lkey == 0x55u);
    REQUIRE(st.last_id != 0);

    disp.deliver(st.last_id, wc_status::success, 24,
                 /*imm=*/0xCAFE, /*flags=*/0);
    REQUIRE(done);
    REQUIRE(result.ok());
    REQUIRE(result.byte_len == 24u);
    REQUIRE(result.imm_data == 0xCAFEu);
}

TEST_CASE("srq<Backend>::recv multi-SGE scatter list",
          "[rdma][srq][sge]") {
    srq_state st;
    state_guard guard{&st};
    dispatcher disp;
    int srq_handle = 0;
    srq<srq_static_backend> q{&srq_handle, disp};

    alignas(8) char a[16] = {};
    alignas(8) char b[16] = {};
    std::array<sge, 2> sges{
        sge{a, 16, 0x1},
        sge{b, 16, 0x2},
    };

    wc_result result{};
    bool done = false;
    auto task = run_srq_recv_span(q, std::span<const sge>(sges),
                                  result, done);

    REQUIRE_FALSE(done);
    REQUIRE(st.last_sges.size() == 2u);

    disp.deliver(st.last_id, wc_status::success, 32);
    REQUIRE(done);
    REQUIRE(result.byte_len == 32u);
}

TEST_CASE("srq<Backend>::recv post failure resumes inline with flush error",
          "[rdma][srq][error]") {
    srq_state st;
    st.return_rc = -12;  // -ENOMEM
    state_guard guard{&st};
    dispatcher disp;
    int srq_handle = 0;
    srq<srq_static_backend> q{&srq_handle, disp};

    char buf[4] = {};
    buffer_view bv{buf, 4, 0};

    wc_result result{};
    bool done = false;
    auto task = run_srq_recv(q, bv, result, done);

    REQUIRE(done);
    REQUIRE_FALSE(result.ok());
    REQUIRE(result.status == wc_status::wr_flush_error);
    REQUIRE(result.imm_data == 12u);
}

TEST_CASE("srq<Backend>::recv orphan race — coroutine destroyed before CQE",
          "[rdma][srq][orphan]") {
    srq_state st;
    state_guard guard{&st};
    dispatcher disp;
    int srq_handle = 0;
    srq<srq_static_backend> q{&srq_handle, disp};

    char buf[8] = {};
    buffer_view bv{buf, 8, 0};

    wr_id captured = 0;
    {
        wc_result result{};
        bool done = false;
        auto task = run_srq_recv(q, bv, result, done);
        REQUIRE_FALSE(done);
        captured = st.last_id;
    }
    disp.deliver(captured, wc_status::success, 8);
    SUCCEED();
}

TEST_CASE("srq<polymorphic_backend>::recv dispatches via vtable",
          "[rdma][srq][polymorphic]") {
    srq_poly_backend impl{};
    dispatcher disp;
    int srq_handle = 0xBEEF;
    srq<polymorphic_backend> q{&srq_handle, impl, disp};

    char buf[64] = {};
    buffer_view bv{buf, sizeof(buf), 0x33};

    wc_result result{};
    bool done = false;
    auto task = run_srq_recv(q, bv, result, done);

    REQUIRE_FALSE(done);
    REQUIRE(impl.state.posts.load() == 1);
    REQUIRE(impl.state.last_srq == &srq_handle);
    REQUIRE(impl.state.last_sges.size() == 1u);

    disp.deliver(impl.state.last_id, wc_status::success, 40);
    REQUIRE(done);
    REQUIRE(result.byte_len == 40u);
}

TEST_CASE("srq<polymorphic_backend>::recv with default post_srq_recv "
          "yields -ENOTSUP surfaced as wr_flush_error",
          "[rdma][srq][polymorphic][default]") {
    poly_no_srq impl{};
    dispatcher disp;
    int srq_handle = 0;
    srq<polymorphic_backend> q{&srq_handle, impl, disp};

    char buf[4] = {};
    buffer_view bv{buf, 4, 0};

    wc_result result{};
    bool done = false;
    auto task = run_srq_recv(q, bv, result, done);

    REQUIRE(done);
    REQUIRE_FALSE(result.ok());
    REQUIRE(result.status == wc_status::wr_flush_error);
    REQUIRE(result.imm_data == 95u);  // -(-ENOTSUP)
}

TEST_CASE("srq exposes native handle and dispatcher accessors",
          "[rdma][srq][accessors]") {
    dispatcher disp;
    int srq_handle = 7;
    srq<srq_static_backend> q{&srq_handle, disp};
    REQUIRE(q.native() == &srq_handle);
    REQUIRE(&q.dispatcher_ref() == &disp);

    srq_poly_backend impl{};
    srq<polymorphic_backend> qp{&srq_handle, impl, disp};
    REQUIRE(qp.native() == &srq_handle);
    REQUIRE(&qp.backend() == &impl);
    REQUIRE(&qp.dispatcher_ref() == &disp);
}
