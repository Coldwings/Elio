// Stage S15 — ATOMIC CAS / FAA.
//
// `connection::cas` and `connection::fetch_add` are gated by the
// optional `backend_with_atomic<Backend>` concept (static) or the
// polymorphic_backend vtable (runtime, default -ENOTSUP). Each
// awaiter resolves to `atomic_result` whose `old_value_host()`
// helper does the IB-canonical big-endian → host conversion on the
// 8 bytes the backend wrote into the local buffer.
//
// Tests:
//   * CAS happy path on static + polymorphic backends. Backend
//     records args; test writes the "old value" into the local
//     buffer big-endian; result.old_value_host() reads it back.
//   * FAA happy path with the same shape.
//   * Synchronous post failure path resumes inline with a
//     synthesised flush error.
//   * Orphan path (coroutine destroyed before CQE) doesn't UAF.
//   * Polymorphic default (no override) yields wr_flush_error with
//     ENOTSUP in imm_data.
//   * Raw vs host accessor distinction.

#include <catch2/catch_test_macros.hpp>

#include <elio/rdma/rdma.hpp>

#include <endian.h>

#include <atomic>
#include <coroutine>
#include <cstdint>
#include <cstring>
#include <span>

using elio::rdma::atomic_result;
using elio::rdma::buffer_view;
using elio::rdma::connection;
using elio::rdma::dispatcher;
using elio::rdma::polymorphic_backend;
using elio::rdma::remote_buffer;
using elio::rdma::send_flags;
using elio::rdma::sge;
using elio::rdma::wc_status;
using elio::rdma::wr_id;

namespace {

struct atomic_state {
    std::atomic<int> cas_calls{0};
    std::atomic<int> faa_calls{0};
    wr_id            last_id{0};
    void*            last_qp{nullptr};
    remote_buffer    last_remote{};
    std::uint64_t    last_compare{0};
    std::uint64_t    last_swap{0};
    std::uint64_t    last_add{0};
    sge              last_sge{};
    int              cas_rc = 0;
    int              faa_rc = 0;

    // When the test triggers the CQE, post_atomic_* helpers leave
    // this big-endian uint64 in the local SGE so the awaiter's
    // old_value_host() helper sees something meaningful.
    std::uint64_t    fake_old_value_host = 0;
};

struct atomic_static_backend {
    static inline atomic_state* state = nullptr;

    // backend_traits required surface — most return 0 / -ENOTSUP since
    // the atomic tests don't exercise them.
    static int post_send(void*, std::span<const sge>,
                         send_flags, std::uint32_t, wr_id) noexcept { return 0; }
    static int post_recv(void*, std::span<const sge>, wr_id) noexcept { return 0; }
    static int post_rdma_write(void*, std::span<const sge>,
                               remote_buffer, send_flags,
                               std::uint32_t, wr_id) noexcept { return 0; }
    static int post_rdma_read(void*, std::span<const sge>,
                              remote_buffer, wr_id) noexcept { return 0; }

    // atomic surface
    static int post_atomic_cas(void* qp,
                               std::span<const sge> sges,
                               remote_buffer rb,
                               std::uint64_t compare,
                               std::uint64_t swap,
                               send_flags /*flags*/,
                               wr_id id) noexcept {
        if (!state) return 0;
        state->cas_calls.fetch_add(1);
        state->last_id      = id;
        state->last_qp      = qp;
        state->last_remote  = rb;
        state->last_compare = compare;
        state->last_swap    = swap;
        state->last_sge     = sges.empty() ? sge{} : sges.front();
        // Simulate the hardware: write the (claimed) old value
        // big-endian into the local buffer.
        if (!sges.empty() && sges.front().addr
            && sges.front().length >= 8) {
            std::uint64_t be = htobe64(state->fake_old_value_host);
            std::memcpy(sges.front().addr, &be, 8);
        }
        return state->cas_rc;
    }
    static int post_atomic_fetch_add(void* qp,
                                     std::span<const sge> sges,
                                     remote_buffer rb,
                                     std::uint64_t add,
                                     send_flags /*flags*/,
                                     wr_id id) noexcept {
        if (!state) return 0;
        state->faa_calls.fetch_add(1);
        state->last_id     = id;
        state->last_qp     = qp;
        state->last_remote = rb;
        state->last_add    = add;
        state->last_sge    = sges.empty() ? sge{} : sges.front();
        if (!sges.empty() && sges.front().addr
            && sges.front().length >= 8) {
            std::uint64_t be = htobe64(state->fake_old_value_host);
            std::memcpy(sges.front().addr, &be, 8);
        }
        return state->faa_rc;
    }
};

struct state_guard {
    explicit state_guard(atomic_state* s) noexcept {
        atomic_static_backend::state = s;
    }
    ~state_guard() noexcept { atomic_static_backend::state = nullptr; }
    state_guard(const state_guard&) = delete;
    state_guard& operator=(const state_guard&) = delete;
};

struct atomic_poly_backend : polymorphic_backend {
    atomic_state state;

    int post_send(void*, std::span<const sge>,
                  send_flags, std::uint32_t, wr_id) noexcept override { return 0; }
    int post_recv(void*, std::span<const sge>, wr_id) noexcept override { return 0; }
    int post_rdma_write(void*, std::span<const sge>,
                        remote_buffer, send_flags,
                        std::uint32_t, wr_id) noexcept override { return 0; }
    int post_rdma_read(void*, std::span<const sge>,
                       remote_buffer, wr_id) noexcept override { return 0; }

    int post_atomic_cas(void* qp, std::span<const sge> sges,
                        remote_buffer rb, std::uint64_t compare,
                        std::uint64_t swap, send_flags /*flags*/,
                        wr_id id) noexcept override {
        state.cas_calls.fetch_add(1);
        state.last_id      = id;
        state.last_qp      = qp;
        state.last_remote  = rb;
        state.last_compare = compare;
        state.last_swap    = swap;
        state.last_sge     = sges.empty() ? sge{} : sges.front();
        if (!sges.empty() && sges.front().addr
            && sges.front().length >= 8) {
            std::uint64_t be = htobe64(state.fake_old_value_host);
            std::memcpy(sges.front().addr, &be, 8);
        }
        return state.cas_rc;
    }
    int post_atomic_fetch_add(void* qp, std::span<const sge> sges,
                              remote_buffer rb, std::uint64_t add,
                              send_flags /*flags*/,
                              wr_id id) noexcept override {
        state.faa_calls.fetch_add(1);
        state.last_id     = id;
        state.last_qp     = qp;
        state.last_remote = rb;
        state.last_add    = add;
        state.last_sge    = sges.empty() ? sge{} : sges.front();
        if (!sges.empty() && sges.front().addr
            && sges.front().length >= 8) {
            std::uint64_t be = htobe64(state.fake_old_value_host);
            std::memcpy(sges.front().addr, &be, 8);
        }
        return state.faa_rc;
    }
};

// A polymorphic backend that does NOT override atomic methods,
// exercising the default -ENOTSUP path.
struct atomic_poly_no_override : polymorphic_backend {
    int post_send(void*, std::span<const sge>,
                  send_flags, std::uint32_t, wr_id) noexcept override { return 0; }
    int post_recv(void*, std::span<const sge>, wr_id) noexcept override { return 0; }
    int post_rdma_write(void*, std::span<const sge>,
                        remote_buffer, send_flags,
                        std::uint32_t, wr_id) noexcept override { return 0; }
    int post_rdma_read(void*, std::span<const sge>,
                       remote_buffer, wr_id) noexcept override { return 0; }
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

}  // namespace

TEST_CASE("backend_with_atomic concept matches a conforming backend",
          "[rdma][atomic][concept]") {
    STATIC_REQUIRE(elio::rdma::backend_with_atomic<atomic_static_backend>);
}

TEST_CASE("cas: backend receives args verbatim; old value round-trips",
          "[rdma][atomic][cas]") {
    atomic_state st;
    st.fake_old_value_host = 0xDEADBEEFCAFEBABEull;
    state_guard guard{&st};
    dispatcher disp;
    int qp_value = 9;
    connection<atomic_static_backend> c{&qp_value, disp};

    alignas(8) char local[8] = {};
    buffer_view bv{local, sizeof(local), 0x123};
    remote_buffer remote{
        .addr   = 0x1000'2000ull,
        .length = 8,
        .rkey   = 0xABCD,
    };

    atomic_result result{};
    bool done = false;
    auto run = [&]() -> probe_task {
        result = co_await c.cas(bv, remote, /*compare=*/42, /*swap=*/100);
        done   = true;
    };
    auto task = run();

    REQUIRE_FALSE(done);
    REQUIRE(st.cas_calls.load() == 1);
    REQUIRE(st.last_qp == &qp_value);
    REQUIRE(st.last_remote.addr == remote.addr);
    REQUIRE(st.last_remote.rkey == 0xABCDu);
    REQUIRE(st.last_compare == 42u);
    REQUIRE(st.last_swap == 100u);
    REQUIRE(st.last_sge.addr == local);
    REQUIRE(st.last_sge.length == 8u);
    REQUIRE(st.last_sge.lkey == 0x123u);

    disp.deliver(st.last_id, wc_status::success, /*byte_len=*/8);
    REQUIRE(done);
    REQUIRE(result.ok());
    REQUIRE(result.old_value_host() == 0xDEADBEEFCAFEBABEull);
    REQUIRE(result.old_value_raw() == htobe64(0xDEADBEEFCAFEBABEull));
}

TEST_CASE("fetch_add: backend receives add operand; old value round-trips",
          "[rdma][atomic][faa]") {
    atomic_state st;
    st.fake_old_value_host = 0xAABBCCDD00112233ull;
    state_guard guard{&st};
    dispatcher disp;
    int qp_value = 10;
    connection<atomic_static_backend> c{&qp_value, disp};

    alignas(8) char local[8] = {};
    buffer_view bv{local, sizeof(local), 0};
    remote_buffer remote{0xDEAD, 8, 0xBEEF};

    atomic_result result{};
    bool done = false;
    auto run = [&]() -> probe_task {
        result = co_await c.fetch_add(bv, remote, /*add=*/7);
        done   = true;
    };
    auto task = run();

    REQUIRE_FALSE(done);
    REQUIRE(st.faa_calls.load() == 1);
    REQUIRE(st.last_add == 7u);

    disp.deliver(st.last_id, wc_status::success, /*byte_len=*/8);
    REQUIRE(done);
    REQUIRE(result.ok());
    REQUIRE(result.old_value_host() == 0xAABBCCDD00112233ull);
}

TEST_CASE("cas: synchronous post failure resumes inline as flush error",
          "[rdma][atomic][cas][error]") {
    atomic_state st;
    st.cas_rc = -22;  // -EINVAL
    state_guard guard{&st};
    dispatcher disp;
    int qp_value = 1;
    connection<atomic_static_backend> c{&qp_value, disp};

    alignas(8) char local[8] = {};
    buffer_view bv{local, sizeof(local), 0};
    remote_buffer remote{0, 8, 0};

    atomic_result result{};
    bool done = false;
    auto run = [&]() -> probe_task {
        result = co_await c.cas(bv, remote, 0, 0);
        done   = true;
    };
    auto task = run();

    REQUIRE(done);
    REQUIRE_FALSE(result.ok());
    REQUIRE(result.wc.status == wc_status::wr_flush_error);
    REQUIRE(result.wc.imm_data == 22u);
}

TEST_CASE("fetch_add: orphan path — awaiter destroyed before CQE",
          "[rdma][atomic][faa][orphan]") {
    atomic_state st;
    state_guard guard{&st};
    dispatcher disp;
    int qp_value = 2;
    connection<atomic_static_backend> c{&qp_value, disp};

    alignas(8) char local[8] = {};
    buffer_view bv{local, sizeof(local), 0};
    remote_buffer remote{0, 8, 0};

    wr_id captured = 0;
    {
        atomic_result result{};
        bool done = false;
        auto run = [&]() -> probe_task {
            result = co_await c.fetch_add(bv, remote, 1);
            done   = true;
        };
        auto task = run();
        REQUIRE_FALSE(done);
        captured = st.last_id;
        REQUIRE(captured != 0);
    }
    // Late CQE; must not UAF.
    disp.deliver(captured, wc_status::success, 8);
    SUCCEED();
}

TEST_CASE("polymorphic cas dispatches via vtable; old value reads through",
          "[rdma][atomic][cas][polymorphic]") {
    atomic_poly_backend impl{};
    impl.state.fake_old_value_host = 0x55ull;
    dispatcher disp;
    int qp_value = 11;
    connection<polymorphic_backend> c{&qp_value, impl, disp};

    alignas(8) char local[8] = {};
    buffer_view bv{local, sizeof(local), 0};
    remote_buffer remote{0xCAFE, 8, 0xBABE};

    atomic_result result{};
    bool done = false;
    auto run = [&]() -> probe_task {
        result = co_await c.cas(bv, remote, /*compare=*/0x10, /*swap=*/0x20);
        done   = true;
    };
    auto task = run();

    REQUIRE_FALSE(done);
    REQUIRE(impl.state.cas_calls.load() == 1);
    REQUIRE(impl.state.last_compare == 0x10u);
    REQUIRE(impl.state.last_swap == 0x20u);

    disp.deliver(impl.state.last_id, wc_status::success, 8);
    REQUIRE(done);
    REQUIRE(result.old_value_host() == 0x55ull);
}

TEST_CASE("polymorphic default (no override) yields ENOTSUP flush error",
          "[rdma][atomic][polymorphic][default]") {
    atomic_poly_no_override impl{};
    dispatcher disp;
    int qp_value = 0;
    connection<polymorphic_backend> c{&qp_value, impl, disp};

    alignas(8) char local[8] = {};
    buffer_view bv{local, sizeof(local), 0};
    remote_buffer remote{0, 8, 0};

    SECTION("cas defaults") {
        atomic_result result{};
        bool done = false;
        auto run = [&]() -> probe_task {
            result = co_await c.cas(bv, remote, 1, 2);
            done   = true;
        };
        auto task = run();
        REQUIRE(done);
        REQUIRE_FALSE(result.ok());
        REQUIRE(result.wc.status == wc_status::wr_flush_error);
        REQUIRE(result.wc.imm_data == 95u);  // -(-ENOTSUP)
    }

    SECTION("fetch_add defaults") {
        atomic_result result{};
        bool done = false;
        auto run = [&]() -> probe_task {
            result = co_await c.fetch_add(bv, remote, 1);
            done   = true;
        };
        auto task = run();
        REQUIRE(done);
        REQUIRE(result.wc.status == wc_status::wr_flush_error);
        REQUIRE(result.wc.imm_data == 95u);
    }
}

TEST_CASE("atomic_result helpers handle the empty / short buffer cases safely",
          "[rdma][atomic][result]") {
    atomic_result r1{};
    REQUIRE(r1.old_value_host() == 0u);
    REQUIRE(r1.old_value_raw() == 0u);

    char too_short[4] = {1, 2, 3, 4};
    atomic_result r2{
        .wc = elio::rdma::wc_result{.status = wc_status::success, .byte_len = 4},
        .local = buffer_view{too_short, sizeof(too_short), 0},
    };
    // Less than 8 bytes; helpers must not over-read.
    REQUIRE(r2.old_value_host() == 0u);
    REQUIRE(r2.old_value_raw() == 0u);
}
