// Stage S11 — IMM data round-trip.
//
// `connection::send_with_imm` and `connection::rdma_write_with_imm`
// set `send_flags::with_imm` and carry an extra uint32_t through to
// the backend. The backend selects the IMM-opcode and forwards the
// imm value. The CQE on the recv side carries imm_data back through
// `wc_result.imm_data`. These tests verify the flag, the carried
// value, and that omitting the IMM API leaves with_imm=false.

#include <catch2/catch_test_macros.hpp>

#include <elio/rdma/rdma.hpp>

#include <atomic>
#include <coroutine>
#include <cstdint>
#include <span>

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

struct imm_state {
    std::atomic<int> sends{0};
    std::atomic<int> writes{0};
    wr_id            last_id{0};
    send_flags       last_flags{};
    std::uint32_t    last_imm = 0xFFFFFFFFu;  // sentinel
};

struct imm_static_backend {
    static inline imm_state* state = nullptr;

    static int post_send(void*, std::span<const sge>, send_flags flags,
                         std::uint32_t imm, wr_id id) noexcept {
        if (state) {
            state->sends.fetch_add(1);
            state->last_id    = id;
            state->last_flags = flags;
            state->last_imm   = imm;
        }
        return 0;
    }
    static int post_recv(void*, std::span<const sge>, wr_id) noexcept {
        return 0;
    }
    static int post_rdma_write(void*, std::span<const sge>,
                               remote_buffer, send_flags flags,
                               std::uint32_t imm, wr_id id) noexcept {
        if (state) {
            state->writes.fetch_add(1);
            state->last_id    = id;
            state->last_flags = flags;
            state->last_imm   = imm;
        }
        return 0;
    }
    static int post_rdma_read(void*, std::span<const sge>,
                              remote_buffer, wr_id) noexcept {
        return 0;
    }
};

struct state_guard {
    explicit state_guard(imm_state* s) noexcept {
        imm_static_backend::state = s;
    }
    ~state_guard() noexcept { imm_static_backend::state = nullptr; }
    state_guard(const state_guard&) = delete;
    state_guard& operator=(const state_guard&) = delete;
};

struct imm_poly_backend : polymorphic_backend {
    imm_state state;

    int post_send(void*, std::span<const sge>, send_flags flags,
                  std::uint32_t imm, wr_id id) noexcept override {
        state.sends.fetch_add(1);
        state.last_id    = id;
        state.last_flags = flags;
        state.last_imm   = imm;
        return 0;
    }
    int post_recv(void*, std::span<const sge>, wr_id) noexcept override {
        return 0;
    }
    int post_rdma_write(void*, std::span<const sge>,
                        remote_buffer, send_flags flags,
                        std::uint32_t imm, wr_id id) noexcept override {
        state.writes.fetch_add(1);
        state.last_id    = id;
        state.last_flags = flags;
        state.last_imm   = imm;
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

}  // namespace

TEST_CASE("send_with_imm sets with_imm flag and carries imm_data",
          "[rdma][send][imm]") {
    imm_state st;
    state_guard guard{&st};
    dispatcher disp;
    int qp_value = 1;
    connection<imm_static_backend> c{&qp_value, disp};

    char payload[32] = {};
    buffer_view bv{payload, sizeof(payload), 0};

    wc_result result{};
    bool done = false;
    auto run = [&]() -> probe_task {
        result = co_await c.send_with_imm(bv, /*imm=*/0xDEADBEEF);
        done   = true;
    };
    auto task = run();

    REQUIRE_FALSE(done);
    REQUIRE(st.sends.load() == 1);
    REQUIRE(st.last_flags.with_imm);
    REQUIRE(st.last_flags.signaled);  // default-on preserved
    REQUIRE(st.last_imm == 0xDEADBEEFu);

    // The dispatcher synthesises a recv-side CQE carrying the same
    // imm_data; verify the caller sees it through wc_result.
    disp.deliver(st.last_id, wc_status::success, /*byte_len=*/32,
                 /*imm_data=*/0xDEADBEEF, /*wc_flags=*/0);
    REQUIRE(done);
    REQUIRE(result.ok());
    REQUIRE(result.imm_data == 0xDEADBEEFu);
}

TEST_CASE("plain send does NOT set with_imm even with non-zero default",
          "[rdma][send][imm]") {
    imm_state st;
    state_guard guard{&st};
    dispatcher disp;
    int qp_value = 2;
    connection<imm_static_backend> c{&qp_value, disp};

    char payload[4] = {};
    buffer_view bv{payload, 4, 0};

    wc_result result{};
    bool done = false;
    auto run = [&]() -> probe_task {
        result = co_await c.send(bv);
        done   = true;
    };
    auto task = run();

    REQUIRE_FALSE(done);
    REQUIRE_FALSE(st.last_flags.with_imm);
    REQUIRE(st.last_imm == 0u);
    disp.deliver(st.last_id, wc_status::success, 4);
    REQUIRE(done);
}

TEST_CASE("rdma_write_with_imm sets with_imm flag and carries imm_data",
          "[rdma][write][imm]") {
    imm_state st;
    state_guard guard{&st};
    dispatcher disp;
    int qp_value = 3;
    connection<imm_static_backend> c{&qp_value, disp};

    char payload[64] = {};
    buffer_view bv{payload, sizeof(payload), 0};
    remote_buffer remote{0xCAFE, 64, 0xBEEF};

    wc_result result{};
    bool done = false;
    auto run = [&]() -> probe_task {
        result = co_await c.rdma_write_with_imm(bv, remote, 0x12345678);
        done   = true;
    };
    auto task = run();

    REQUIRE_FALSE(done);
    REQUIRE(st.writes.load() == 1);
    REQUIRE(st.last_flags.with_imm);
    REQUIRE(st.last_imm == 0x12345678u);

    disp.deliver(st.last_id, wc_status::success, 0,
                 /*imm_data=*/0x12345678, /*wc_flags=*/0);
    REQUIRE(done);
}

TEST_CASE("polymorphic send_with_imm dispatches via vtable and carries imm",
          "[rdma][send][imm][polymorphic]") {
    imm_poly_backend impl{};
    dispatcher disp;
    int qp_value = 4;
    connection<polymorphic_backend> c{&qp_value, impl, disp};

    char payload[16] = {};
    buffer_view bv{payload, 16, 0};

    wc_result result{};
    bool done = false;
    auto run = [&]() -> probe_task {
        result = co_await c.send_with_imm(bv, 0xABABABAB);
        done   = true;
    };
    auto task = run();

    REQUIRE_FALSE(done);
    REQUIRE(impl.state.last_flags.with_imm);
    REQUIRE(impl.state.last_imm == 0xABABABABu);

    disp.deliver(impl.state.last_id, wc_status::success, 16,
                 0xABABABAB, 0);
    REQUIRE(done);
    REQUIRE(result.imm_data == 0xABABABABu);
}

TEST_CASE("send_with_imm respects an explicitly-passed flags struct",
          "[rdma][send][imm][flags]") {
    imm_state st;
    state_guard guard{&st};
    dispatcher disp;
    int qp_value = 5;
    connection<imm_static_backend> c{&qp_value, disp};

    char payload[8] = {};
    buffer_view bv{payload, 8, 0};

    send_flags flags{};
    flags.solicited = true;
    flags.fence     = true;

    wc_result result{};
    bool done = false;
    auto run = [&]() -> probe_task {
        result = co_await c.send_with_imm(bv, 0xFEEDBABE, flags);
        done   = true;
    };
    auto task = run();

    REQUIRE(st.last_flags.with_imm);
    REQUIRE(st.last_flags.solicited);
    REQUIRE(st.last_flags.fence);
    REQUIRE(st.last_imm == 0xFEEDBABEu);
    disp.deliver(st.last_id, wc_status::success, 8);
    REQUIRE(done);
}
