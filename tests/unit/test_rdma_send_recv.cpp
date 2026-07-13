// Stage S3 — send_awaitable / recv_awaitable wired into connection.
//
// Covers four behaviours that together prove the S3 design:
//   * Happy path — co_await c.send(...) suspends, the mock backend
//     receives the WR with the wr_id encoded by the dispatcher, the
//     test triggers a CQE via `dispatcher::deliver`, the coroutine
//     resumes with a populated wc_result.
//   * Synchronous post failure — the mock backend returns a negative
//     errno; the awaiter must NOT suspend, must synthesise a
//     wr_flush_error wc_result, and the heap op_state must be freed
//     by the awaiter's unique_ptr (no leak under ASAN).
//   * Orphan race — the coroutine is destroyed BEFORE the CQE arrives;
//     the dispatcher's late deliver() must take ownership and free the
//     state (no UAF / leak under ASAN).
//   * Polymorphic backend dispatch — same surface, but through the
//     polymorphic_backend vtable.

#include <catch2/catch_test_macros.hpp>

#include <elio/rdma/rdma.hpp>

#include <atomic>
#include <coroutine>
#include <cstdint>
#include <span>
#include <type_traits>
#include <utility>

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

// Captured per-test state shared between the static-traits mock and
// the test body. Static-traits backends require static member
// functions, so the link is via a static pointer set up per test
// through `state_guard`.
struct mock_state {
    std::atomic<int> sends{0};
    std::atomic<int> recvs{0};
    std::atomic<int> writes{0};
    std::atomic<int> reads{0};
    wr_id            last_send_id{0};
    wr_id            last_recv_id{0};
    int              send_rc = 0;   // forced rc for post_send
    int              recv_rc = 0;   // forced rc for post_recv
    sge              last_send_sge{};
    sge              last_recv_sge{};
    send_flags       last_send_flags{};
    std::uint32_t    last_send_imm = 0;
    void*            last_qp = nullptr;
    dispatcher*      inline_dispatcher = nullptr;
    bool*            inline_done = nullptr;
    bool             inline_send_success = false;
    bool             resumed_before_post_return = false;
};

struct mock_static_backend {
    static inline mock_state* state = nullptr;

    static int post_send(void* qp, std::span<const sge> sges,
                         send_flags flags, std::uint32_t imm_data,
                         wr_id id) noexcept {
        if (state) {
            state->sends.fetch_add(1);
            state->last_send_id    = id;
            state->last_send_sge   = sges.empty() ? sge{} : sges.front();
            state->last_send_flags = flags;
            state->last_send_imm   = imm_data;
            state->last_qp         = qp;
            if (state->inline_send_success && state->inline_dispatcher
                && state->send_rc == 0) {
                state->inline_dispatcher->deliver(
                    id, wc_status::success,
                    sges.empty() ? 0 : sges.front().length,
                    imm_data, /*wc_flags=*/0);
                if (state->inline_done && *state->inline_done) {
                    state->resumed_before_post_return = true;
                }
            }
            return state->send_rc;
        }
        return 0;
    }
    static int post_recv(void* qp, std::span<const sge> sges,
                         wr_id id) noexcept {
        if (state) {
            state->recvs.fetch_add(1);
            state->last_recv_id  = id;
            state->last_recv_sge = sges.empty() ? sge{} : sges.front();
            state->last_qp       = qp;
            return state->recv_rc;
        }
        return 0;
    }
    static int post_rdma_write(void*, std::span<const sge>,
                               remote_buffer, send_flags,
                               std::uint32_t, wr_id) noexcept {
        if (state) state->writes.fetch_add(1);
        return 0;
    }
    static int post_rdma_read(void*, std::span<const sge>,
                              remote_buffer, wr_id) noexcept {
        if (state) state->reads.fetch_add(1);
        return 0;
    }
};

struct state_guard {
    explicit state_guard(mock_state* s) noexcept {
        mock_static_backend::state = s;
    }
    ~state_guard() noexcept { mock_static_backend::state = nullptr; }
    state_guard(const state_guard&) = delete;
    state_guard& operator=(const state_guard&) = delete;
};

struct mock_poly_backend : polymorphic_backend {
    mock_state state;

    int post_send(void* qp, std::span<const sge> sges,
                  send_flags flags, std::uint32_t imm_data,
                  wr_id id) noexcept override {
        state.sends.fetch_add(1);
        state.last_send_id    = id;
        state.last_send_sge   = sges.empty() ? sge{} : sges.front();
        state.last_send_flags = flags;
        state.last_send_imm   = imm_data;
        state.last_qp         = qp;
        return state.send_rc;
    }
    int post_recv(void* qp, std::span<const sge> sges,
                  wr_id id) noexcept override {
        state.recvs.fetch_add(1);
        state.last_recv_id  = id;
        state.last_recv_sge = sges.empty() ? sge{} : sges.front();
        state.last_qp       = qp;
        return state.recv_rc;
    }
    int post_rdma_write(void*, std::span<const sge>,
                        remote_buffer, send_flags,
                        std::uint32_t, wr_id) noexcept override {
        return 0;
    }
    int post_rdma_read(void*, std::span<const sge>,
                       remote_buffer, wr_id) noexcept override {
        return 0;
    }
};

// Throwaway coroutine type — initial_suspend = suspend_never so the
// coroutine runs to the first co_await on creation. final_suspend
// keeps the frame alive until the test destroys it; that lets us
// observe `done` and (for the orphan test) destroy the coroutine
// while it's still suspended on the RDMA awaiter.
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
    ~probe_task() {
        if (handle) handle.destroy();
    }
};

template <typename Conn>
probe_task run_send(Conn& c, buffer_view buf, send_flags flags,
                    wc_result& out, bool& done) {
    out  = co_await c.send(buf, flags);
    done = true;
}

template <typename Conn>
probe_task run_recv(Conn& c, buffer_view buf,
                    wc_result& out, bool& done) {
    out  = co_await c.recv(buf);
    done = true;
}

template <typename Awaiter>
probe_task run_started(Awaiter op, wc_result& out, bool& done) {
    out  = co_await std::move(op);
    done = true;
}

}  // namespace

TEST_CASE("unawaited RDMA operations release their unstarted state",
          "[rdma][lifetime]") {
    mock_state st;
    state_guard guard{&st};
    dispatcher disp;
    int qp_value = 55;
    connection<mock_static_backend> c{&qp_value, disp};

    char payload[8] = {};
    buffer_view local{payload, sizeof(payload), 0x1234};
    remote_buffer remote{0x1000, sizeof(payload), 0x5678};

    {
        auto send = c.send(local);
        auto recv = c.recv(local);
        auto write = c.rdma_write(local, remote);
        auto read = c.rdma_read(local, remote);
    }

    REQUIRE(st.sends.load() == 0);
    REQUIRE(st.recvs.load() == 0);
    REQUIRE(st.writes.load() == 0);
    REQUIRE(st.reads.load() == 0);
    // LeakSanitizer verifies that all four shared op_state allocations
    // were released when their lazy awaitables left scope.
}

TEST_CASE("RDMA operation awaitables are movable but not move-assignable",
          "[rdma][lifetime][contract]") {
    using send_op = elio::rdma::detail::send_awaitable<mock_static_backend>;
    using recv_op = elio::rdma::detail::recv_awaitable<mock_static_backend>;
    using write_op = elio::rdma::detail::rdma_write_awaitable<mock_static_backend>;
    using read_op = elio::rdma::detail::rdma_read_awaitable<mock_static_backend>;
    using cas_op = elio::rdma::detail::rdma_cas_awaitable<mock_static_backend>;
    using faa_op = elio::rdma::detail::rdma_faa_awaitable<mock_static_backend>;
    using srq_recv_op = elio::rdma::detail::srq_recv_awaitable<mock_static_backend>;

    STATIC_REQUIRE(std::is_move_constructible_v<send_op>);
    STATIC_REQUIRE(std::is_move_constructible_v<recv_op>);
    STATIC_REQUIRE(std::is_move_constructible_v<write_op>);
    STATIC_REQUIRE(std::is_move_constructible_v<read_op>);
    STATIC_REQUIRE(std::is_move_constructible_v<cas_op>);
    STATIC_REQUIRE(std::is_move_constructible_v<faa_op>);
    STATIC_REQUIRE(std::is_move_constructible_v<srq_recv_op>);

    STATIC_REQUIRE_FALSE(std::is_move_assignable_v<send_op>);
    STATIC_REQUIRE_FALSE(std::is_move_assignable_v<recv_op>);
    STATIC_REQUIRE_FALSE(std::is_move_assignable_v<write_op>);
    STATIC_REQUIRE_FALSE(std::is_move_assignable_v<read_op>);
    STATIC_REQUIRE_FALSE(std::is_move_assignable_v<cas_op>);
    STATIC_REQUIRE_FALSE(std::is_move_assignable_v<faa_op>);
    STATIC_REQUIRE_FALSE(std::is_move_assignable_v<srq_recv_op>);

    STATIC_REQUIRE(std::is_same_v<decltype(std::declval<send_op&>().start()),
                                  send_op&>);
    STATIC_REQUIRE(std::is_same_v<decltype(std::declval<send_op&&>().start()),
                                  send_op>);
    STATIC_REQUIRE(std::is_same_v<decltype(std::declval<recv_op&>().start()),
                                  recv_op&>);
    STATIC_REQUIRE(std::is_same_v<decltype(std::declval<recv_op&&>().start()),
                                  recv_op>);
}

TEST_CASE("recv: start posts immediately and later await does not repost",
          "[rdma][recv][start]") {
    mock_state st;
    state_guard guard{&st};
    dispatcher disp;
    int qp_value = 701;
    connection<mock_static_backend> c{&qp_value, disp};

    alignas(8) char payload[32] = {};
    buffer_view bv{payload, sizeof(payload), 0xCAFE};

    auto op = c.recv(bv).start();
    REQUIRE(st.recvs.load() == 1);
    const auto id = st.last_recv_id;
    REQUIRE(id != 0);

    wc_result result{};
    bool done = false;
    auto task = run_started(std::move(op), result, done);

    REQUIRE_FALSE(done);
    REQUIRE(st.recvs.load() == 1);

    disp.deliver(id, wc_status::success, /*byte_len=*/24,
                 /*imm=*/0x1234, /*flags=*/0x2);
    REQUIRE(done);
    REQUIRE(result.ok());
    REQUIRE(result.byte_len == 24u);
    REQUIRE(result.imm_data == 0x1234u);
    REQUIRE(result.wc_flags == 0x2u);
}

TEST_CASE("recv: completion before awaiting a started operation resumes inline",
          "[rdma][recv][start][completed-before-await]") {
    mock_state st;
    state_guard guard{&st};
    dispatcher disp;
    int qp_value = 702;
    connection<mock_static_backend> c{&qp_value, disp};

    char payload[16] = {};
    buffer_view bv{payload, sizeof(payload), 0xBEEF};

    auto op = c.recv(bv).start();
    const auto id = st.last_recv_id;
    disp.deliver(id, wc_status::success, /*byte_len=*/12);

    wc_result result{};
    bool done = false;
    auto task = run_started(std::move(op), result, done);

    REQUIRE(done);
    REQUIRE(st.recvs.load() == 1);
    REQUIRE(result.ok());
    REQUIRE(result.byte_len == 12u);
}

TEST_CASE("send: inline completion during start is stored for later await",
          "[rdma][send][start][inline-completion]") {
    mock_state st;
    state_guard guard{&st};
    dispatcher disp;
    int qp_value = 703;
    connection<mock_static_backend> c{&qp_value, disp};

    char payload[8] = {};
    buffer_view bv{payload, sizeof(payload), 0x1234};
    st.inline_dispatcher = &disp;
    st.inline_send_success = true;

    auto op = c.send(bv).start();
    REQUIRE(st.sends.load() == 1);

    wc_result result{};
    bool done = false;
    auto task = run_started(std::move(op), result, done);

    REQUIRE(done);
    REQUIRE(result.ok());
    REQUIRE(result.byte_len == 8u);
}

TEST_CASE("send: started post failure is returned on later await",
          "[rdma][send][start][error]") {
    mock_state st;
    st.send_rc = -11;
    state_guard guard{&st};
    dispatcher disp;
    int qp_value = 704;
    connection<mock_static_backend> c{&qp_value, disp};

    char payload[8] = {};
    buffer_view bv{payload, sizeof(payload), 0x1};

    auto op = c.send(bv).start();
    REQUIRE(st.sends.load() == 1);

    wc_result result{};
    bool done = false;
    auto task = run_started(std::move(op), result, done);

    REQUIRE(done);
    REQUIRE_FALSE(result.ok());
    REQUIRE(result.status == wc_status::wr_flush_error);
    REQUIRE(result.imm_data == 11u);
}

TEST_CASE("send: started operation can be orphaned before CQE",
          "[rdma][send][start][orphan]") {
    mock_state st;
    state_guard guard{&st};
    dispatcher disp;
    int qp_value = 705;
    connection<mock_static_backend> c{&qp_value, disp};

    char payload[8] = {};
    buffer_view bv{payload, sizeof(payload), 0x1};

    wr_id captured = 0;
    {
        auto op = c.send(bv).start();
        captured = st.last_send_id;
        REQUIRE(captured != 0);
    }

    disp.deliver(captured, wc_status::success, 8);
    SUCCEED();
}

TEST_CASE("send: happy path suspends and dispatcher resumes",
          "[rdma][send]") {
    mock_state st;
    state_guard guard{&st};
    dispatcher disp;
    int qp_value = 99;
    connection<mock_static_backend> c{&qp_value, disp};

    alignas(8) char payload[16] = {};
    buffer_view bv{payload, sizeof(payload), 0xCAFE};

    wc_result result{};
    bool done = false;
    auto task = run_send(c, bv, send_flags{}, result, done);

    REQUIRE_FALSE(done);
    REQUIRE(st.sends.load() == 1);
    REQUIRE(st.last_qp == &qp_value);
    REQUIRE(st.last_send_sge.addr == payload);
    REQUIRE(st.last_send_sge.length == 16u);
    REQUIRE(st.last_send_sge.lkey == 0xCAFEu);
    REQUIRE(st.last_send_flags.signaled);
    REQUIRE(st.last_send_id != 0);

    disp.deliver(st.last_send_id, wc_status::success, /*byte_len=*/16,
                 /*imm=*/0xABCD, /*flags=*/0x2);
    REQUIRE(done);
    REQUIRE(result.ok());
    REQUIRE(result.byte_len == 16u);
    REQUIRE(result.imm_data == 0xABCDu);
    REQUIRE(result.wc_flags == 0x2u);
}

TEST_CASE("send: inline successful completion resumes after post returns",
          "[rdma][send][inline-completion]") {
    mock_state st;
    state_guard guard{&st};
    dispatcher disp;
    int qp_value = 101;
    connection<mock_static_backend> c{&qp_value, disp};

    alignas(8) char payload[16] = {};
    buffer_view bv{payload, sizeof(payload), 0xC0DE};

    wc_result result{};
    bool done = false;
    st.inline_dispatcher = &disp;
    st.inline_done = &done;
    st.inline_send_success = true;

    auto task = run_send(c, bv, send_flags{}, result, done);

    REQUIRE(st.sends.load() == 1);
    REQUIRE_FALSE(st.resumed_before_post_return);
    REQUIRE(done);
    REQUIRE(result.ok());
    REQUIRE(result.byte_len == 16u);
}

TEST_CASE("send: awaited operation forces completion for unsignaled flags",
          "[rdma][send][signaled]") {
    mock_state st;
    state_guard guard{&st};
    dispatcher disp;
    int qp_value = 100;
    connection<mock_static_backend> c{&qp_value, disp};

    char payload[8] = {};
    buffer_view bv{payload, sizeof(payload), 0x1234};

    send_flags flags = send_flags::none();
    flags.solicited = true;

    wc_result result{};
    bool done = false;
    auto task = run_send(c, bv, flags, result, done);

    REQUIRE_FALSE(done);
    REQUIRE(st.sends.load() == 1);
    REQUIRE(st.last_send_flags.signaled);
    REQUIRE(st.last_send_flags.solicited);

    disp.deliver(st.last_send_id, wc_status::success, /*byte_len=*/8);
    REQUIRE(done);
    REQUIRE(result.ok());
}

TEST_CASE("recv: happy path suspends and dispatcher resumes",
          "[rdma][recv]") {
    mock_state st;
    state_guard guard{&st};
    dispatcher disp;
    int qp_value = 7;
    connection<mock_static_backend> c{&qp_value, disp};

    alignas(8) char payload[64] = {};
    buffer_view bv{payload, sizeof(payload), 0xBEEF};

    wc_result result{};
    bool done = false;
    auto task = run_recv(c, bv, result, done);

    REQUIRE_FALSE(done);
    REQUIRE(st.recvs.load() == 1);
    REQUIRE(st.last_qp == &qp_value);
    REQUIRE(st.last_recv_sge.addr == payload);
    REQUIRE(st.last_recv_sge.length == 64u);
    REQUIRE(st.last_recv_sge.lkey == 0xBEEFu);
    REQUIRE(st.last_recv_id != 0);

    disp.deliver(st.last_recv_id, wc_status::success, /*byte_len=*/40);
    REQUIRE(done);
    REQUIRE(result.ok());
    REQUIRE(result.byte_len == 40u);
}

TEST_CASE("send: post failure resumes inline with synthesised flush error",
          "[rdma][send][error]") {
    mock_state st;
    st.send_rc = -11;  // -EAGAIN
    state_guard guard{&st};
    dispatcher disp;
    int qp_value = 1;
    connection<mock_static_backend> c{&qp_value, disp};

    char payload[4] = {};
    buffer_view bv{payload, sizeof(payload), 0};

    wc_result result{};
    bool done = false;
    auto task = run_send(c, bv, send_flags{}, result, done);

    // Backend failed synchronously — awaiter must NOT suspend.
    REQUIRE(done);
    REQUIRE_FALSE(result.ok());
    REQUIRE(result.status == wc_status::wr_flush_error);
    REQUIRE(result.imm_data == 11u);  // -(-11)
    // No call to disp.deliver() — heap op_state is freed by the
    // awaiter's unique_ptr because finalize_post_ flipped the phase
    // to `completed`, defeating try_orphan in the dtor. ASAN catches
    // a regression.
}

TEST_CASE("recv: post failure resumes inline with synthesised flush error",
          "[rdma][recv][error]") {
    mock_state st;
    st.recv_rc = -22;  // -EINVAL
    state_guard guard{&st};
    dispatcher disp;
    int qp_value = 2;
    connection<mock_static_backend> c{&qp_value, disp};

    char payload[4] = {};
    buffer_view bv{payload, sizeof(payload), 0};

    wc_result result{};
    bool done = false;
    auto task = run_recv(c, bv, result, done);

    REQUIRE(done);
    REQUIRE_FALSE(result.ok());
    REQUIRE(result.status == wc_status::wr_flush_error);
    REQUIRE(result.imm_data == 22u);
}

TEST_CASE("send: orphan race — awaiter destroyed before CQE",
          "[rdma][send][orphan]") {
    mock_state st;
    state_guard guard{&st};
    dispatcher disp;
    int qp_value = 33;
    connection<mock_static_backend> c{&qp_value, disp};

    char payload[8] = {};
    buffer_view bv{payload, sizeof(payload), 0x1};

    wr_id captured = 0;
    {
        wc_result result{};
        bool done = false;
        auto task = run_send(c, bv, send_flags{}, result, done);
        REQUIRE_FALSE(done);
        captured = st.last_send_id;
        REQUIRE(captured != 0);
        // probe_task destructor destroys the coroutine, which destroys
        // the suspended awaitable. op_awaiter_base dtor CASes
        // pending → orphaned and releases unique_ptr ownership.
    }

    // Now the late CQE arrives. Dispatcher must take ownership and
    // free the orphaned op_state. ASAN flags a UAF or leak.
    disp.deliver(captured, wc_status::success, 8);
    SUCCEED();
}

TEST_CASE("send via polymorphic_backend dispatches through the vtable",
          "[rdma][send][polymorphic]") {
    mock_poly_backend impl{};
    dispatcher disp;
    int qp_value = 42;
    connection<polymorphic_backend> c{&qp_value, impl, disp};

    alignas(8) char payload[32] = {};
    buffer_view bv{payload, sizeof(payload), 0x99};

    wc_result result{};
    bool done = false;
    send_flags flags{};
    flags.solicited = true;
    auto task = run_send(c, bv, flags, result, done);

    REQUIRE_FALSE(done);
    REQUIRE(impl.state.sends.load() == 1);
    REQUIRE(impl.state.last_qp == &qp_value);
    REQUIRE(impl.state.last_send_sge.addr == payload);
    REQUIRE(impl.state.last_send_sge.length == 32u);
    REQUIRE(impl.state.last_send_sge.lkey == 0x99u);
    REQUIRE(impl.state.last_send_flags.solicited);
    REQUIRE(impl.state.last_send_id != 0);

    disp.deliver(impl.state.last_send_id, wc_status::success, 32,
                 /*imm=*/0xDEAD, /*flags=*/0x4);
    REQUIRE(done);
    REQUIRE(result.ok());
    REQUIRE(result.byte_len == 32u);
    REQUIRE(result.imm_data == 0xDEADu);
    REQUIRE(result.wc_flags == 0x4u);
}

TEST_CASE("recv via polymorphic_backend dispatches through the vtable",
          "[rdma][recv][polymorphic]") {
    mock_poly_backend impl{};
    dispatcher disp;
    int qp_value = 17;
    connection<polymorphic_backend> c{&qp_value, impl, disp};

    alignas(8) char payload[128] = {};
    buffer_view bv{payload, sizeof(payload), 0x77};

    wc_result result{};
    bool done = false;
    auto task = run_recv(c, bv, result, done);

    REQUIRE_FALSE(done);
    REQUIRE(impl.state.recvs.load() == 1);
    REQUIRE(impl.state.last_recv_sge.addr == payload);
    REQUIRE(impl.state.last_recv_sge.length == 128u);

    disp.deliver(impl.state.last_recv_id, wc_status::success, 96);
    REQUIRE(done);
    REQUIRE(result.byte_len == 96u);
}

TEST_CASE("send via polymorphic_backend: orphan race",
          "[rdma][send][polymorphic][orphan]") {
    mock_poly_backend impl{};
    dispatcher disp;
    int qp_value = 5;
    connection<polymorphic_backend> c{&qp_value, impl, disp};

    char payload[8] = {};
    buffer_view bv{payload, sizeof(payload), 0};

    wr_id captured = 0;
    {
        wc_result result{};
        bool done = false;
        auto task = run_send(c, bv, send_flags{}, result, done);
        REQUIRE_FALSE(done);
        captured = impl.state.last_send_id;
    }
    disp.deliver(captured, wc_status::success, 8);
    SUCCEED();
}
