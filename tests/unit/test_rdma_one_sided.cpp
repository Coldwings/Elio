// Stage S4 — rdma_write / rdma_read awaiter coverage.
//
// One-sided RDMA mirrors the SEND/RECV awaiter shape from S3 but
// carries an extra `remote_buffer` (addr, length, rkey). These tests
// verify:
//   * Backend receives the local SGE AND the remote_buffer verbatim.
//   * Dispatcher resumes the coroutine with the wc_result the
//     completion carried (byte_len matters for READ).
//   * Synchronous post failure (-EAGAIN, -EINVAL) resumes inline
//     with a synthesised wr_flush_error result.
//   * Orphan path: coroutine destroyed before CQE arrives, late
//     deliver must not UAF (ASAN-checked).
//   * Polymorphic backend dispatches identically.

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

struct one_sided_state {
    std::atomic<int> writes{0};
    std::atomic<int> reads{0};
    wr_id            last_write_id{0};
    wr_id            last_read_id{0};
    int              write_rc = 0;
    int              read_rc  = 0;
    sge              last_write_sge{};
    sge              last_read_sge{};
    remote_buffer    last_write_remote{};
    remote_buffer    last_read_remote{};
    send_flags       last_write_flags{};
    std::uint32_t    last_write_imm = 0;
    void*            last_qp = nullptr;
    dispatcher*      inline_dispatcher = nullptr;
    bool*            inline_done = nullptr;
    bool             inline_write_success = false;
    bool             resumed_before_post_return = false;
};

struct one_sided_static_backend {
    static inline one_sided_state* state = nullptr;

    static int post_send(void*, std::span<const sge>,
                         send_flags, std::uint32_t, wr_id) noexcept {
        return 0;
    }
    static int post_recv(void*, std::span<const sge>, wr_id) noexcept {
        return 0;
    }
    static int post_rdma_write(void* qp, std::span<const sge> sges,
                               remote_buffer rb, send_flags flags,
                               std::uint32_t imm_data, wr_id id) noexcept {
        if (state) {
            state->writes.fetch_add(1);
            state->last_write_id     = id;
            state->last_write_sge    = sges.empty() ? sge{} : sges.front();
            state->last_write_remote = rb;
            state->last_write_flags  = flags;
            state->last_write_imm    = imm_data;
            state->last_qp           = qp;
            if (state->inline_write_success && state->inline_dispatcher
                && state->write_rc == 0) {
                state->inline_dispatcher->deliver(
                    id, wc_status::success, /*byte_len=*/0,
                    imm_data, /*wc_flags=*/0);
                if (state->inline_done && *state->inline_done) {
                    state->resumed_before_post_return = true;
                }
            }
            return state->write_rc;
        }
        return 0;
    }
    static int post_rdma_read(void* qp, std::span<const sge> sges,
                              remote_buffer rb, wr_id id) noexcept {
        if (state) {
            state->reads.fetch_add(1);
            state->last_read_id     = id;
            state->last_read_sge    = sges.empty() ? sge{} : sges.front();
            state->last_read_remote = rb;
            state->last_qp          = qp;
            return state->read_rc;
        }
        return 0;
    }
};

struct state_guard {
    explicit state_guard(one_sided_state* s) noexcept {
        one_sided_static_backend::state = s;
    }
    ~state_guard() noexcept { one_sided_static_backend::state = nullptr; }
    state_guard(const state_guard&) = delete;
    state_guard& operator=(const state_guard&) = delete;
};

struct one_sided_poly_backend : polymorphic_backend {
    one_sided_state state;

    int post_send(void*, std::span<const sge>,
                  send_flags, std::uint32_t, wr_id) noexcept override {
        return 0;
    }
    int post_recv(void*, std::span<const sge>, wr_id) noexcept override {
        return 0;
    }
    int post_rdma_write(void* qp, std::span<const sge> sges,
                        remote_buffer rb, send_flags flags,
                        std::uint32_t imm_data, wr_id id) noexcept override {
        state.writes.fetch_add(1);
        state.last_write_id     = id;
        state.last_write_sge    = sges.empty() ? sge{} : sges.front();
        state.last_write_remote = rb;
        state.last_write_flags  = flags;
        state.last_write_imm    = imm_data;
        state.last_qp           = qp;
        return state.write_rc;
    }
    int post_rdma_read(void* qp, std::span<const sge> sges,
                       remote_buffer rb, wr_id id) noexcept override {
        state.reads.fetch_add(1);
        state.last_read_id     = id;
        state.last_read_sge    = sges.empty() ? sge{} : sges.front();
        state.last_read_remote = rb;
        state.last_qp          = qp;
        return state.read_rc;
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
    ~probe_task() {
        if (handle) handle.destroy();
    }
};

template <typename Conn>
probe_task run_write(Conn& c, buffer_view local, remote_buffer remote,
                     send_flags flags, wc_result& out, bool& done) {
    out  = co_await c.rdma_write(local, remote, flags);
    done = true;
}

template <typename Conn>
probe_task run_read(Conn& c, buffer_view local, remote_buffer remote,
                    wc_result& out, bool& done) {
    out  = co_await c.rdma_read(local, remote);
    done = true;
}

}  // namespace

TEST_CASE("rdma_write: backend receives local SGE + remote_buffer + flags",
          "[rdma][write]") {
    one_sided_state st;
    state_guard guard{&st};
    dispatcher disp;
    int qp_value = 11;
    connection<one_sided_static_backend> c{&qp_value, disp};

    alignas(8) char payload[64] = {};
    buffer_view local{payload, sizeof(payload), 0x1111};
    remote_buffer remote{
        .addr   = 0xCAFEBABE'DEADBEEFull,
        .length = 64,
        .rkey   = 0x2222,
    };

    wc_result result{};
    bool done = false;
    send_flags flags{};
    flags.fence = true;
    auto task = run_write(c, local, remote, flags, result, done);

    REQUIRE_FALSE(done);
    REQUIRE(st.writes.load() == 1);
    REQUIRE(st.last_qp == &qp_value);
    REQUIRE(st.last_write_sge.addr == payload);
    REQUIRE(st.last_write_sge.length == 64u);
    REQUIRE(st.last_write_sge.lkey == 0x1111u);
    REQUIRE(st.last_write_remote.addr == remote.addr);
    REQUIRE(st.last_write_remote.length == 64u);
    REQUIRE(st.last_write_remote.rkey == 0x2222u);
    REQUIRE(st.last_write_flags.fence);
    REQUIRE(st.last_write_flags.signaled);  // default-on
    REQUIRE(st.last_write_id != 0);

    // RDMA WRITE's CQE typically reports byte_len=0; verify the value
    // the dispatcher hands us is what surfaces.
    disp.deliver(st.last_write_id, wc_status::success, /*byte_len=*/0);
    REQUIRE(done);
    REQUIRE(result.ok());
    REQUIRE(result.byte_len == 0u);
}

TEST_CASE("rdma_write: inline successful completion resumes after post returns",
          "[rdma][write][inline-completion]") {
    one_sided_state st;
    state_guard guard{&st};
    dispatcher disp;
    int qp_value = 14;
    connection<one_sided_static_backend> c{&qp_value, disp};

    alignas(8) char payload[32] = {};
    buffer_view local{payload, sizeof(payload), 0x777};
    remote_buffer remote{0xCAFE, sizeof(payload), 0xBEEF};

    wc_result result{};
    bool done = false;
    st.inline_dispatcher = &disp;
    st.inline_done = &done;
    st.inline_write_success = true;

    auto task = run_write(c, local, remote, send_flags{}, result, done);

    REQUIRE(st.writes.load() == 1);
    REQUIRE_FALSE(st.resumed_before_post_return);
    REQUIRE(done);
    REQUIRE(result.ok());
    REQUIRE(result.byte_len == 0u);
}

TEST_CASE("rdma_write: awaited operation forces completion for unsignaled flags",
          "[rdma][write][signaled]") {
    one_sided_state st;
    state_guard guard{&st};
    dispatcher disp;
    int qp_value = 12;
    connection<one_sided_static_backend> c{&qp_value, disp};

    char payload[16] = {};
    buffer_view local{payload, sizeof(payload), 0x1234};
    remote_buffer remote{0xCAFE, sizeof(payload), 0xBEEF};

    send_flags flags = send_flags::none();
    flags.fence = true;

    wc_result result{};
    bool done = false;
    auto task = run_write(c, local, remote, flags, result, done);

    REQUIRE_FALSE(done);
    REQUIRE(st.writes.load() == 1);
    REQUIRE(st.last_write_flags.signaled);
    REQUIRE(st.last_write_flags.fence);

    disp.deliver(st.last_write_id, wc_status::success, /*byte_len=*/0);
    REQUIRE(done);
    REQUIRE(result.ok());
}

TEST_CASE("rdma_read: backend receives local SGE + remote_buffer; "
          "byte_len returned to caller",
          "[rdma][read]") {
    one_sided_state st;
    state_guard guard{&st};
    dispatcher disp;
    int qp_value = 22;
    connection<one_sided_static_backend> c{&qp_value, disp};

    alignas(8) char payload[256] = {};
    buffer_view local{payload, sizeof(payload), 0x3333};
    remote_buffer remote{
        .addr   = 0x1234'5678'9ABC'DEF0ull,
        .length = 256,
        .rkey   = 0x4444,
    };

    wc_result result{};
    bool done = false;
    auto task = run_read(c, local, remote, result, done);

    REQUIRE_FALSE(done);
    REQUIRE(st.reads.load() == 1);
    REQUIRE(st.last_qp == &qp_value);
    REQUIRE(st.last_read_sge.addr == payload);
    REQUIRE(st.last_read_sge.length == 256u);
    REQUIRE(st.last_read_sge.lkey == 0x3333u);
    REQUIRE(st.last_read_remote.addr == remote.addr);
    REQUIRE(st.last_read_remote.rkey == 0x4444u);
    REQUIRE(st.last_read_id != 0);

    disp.deliver(st.last_read_id, wc_status::success, /*byte_len=*/200);
    REQUIRE(done);
    REQUIRE(result.ok());
    REQUIRE(result.byte_len == 200u);
}

TEST_CASE("rdma_write: post failure resumes inline with flush error",
          "[rdma][write][error]") {
    one_sided_state st;
    st.write_rc = -11;  // -EAGAIN
    state_guard guard{&st};
    dispatcher disp;
    int qp_value = 1;
    connection<one_sided_static_backend> c{&qp_value, disp};

    char payload[4] = {};
    buffer_view local{payload, 4, 0};
    remote_buffer remote{0xDEAD, 4, 0x1};

    wc_result result{};
    bool done = false;
    auto task = run_write(c, local, remote, send_flags{}, result, done);

    REQUIRE(done);
    REQUIRE_FALSE(result.ok());
    REQUIRE(result.status == wc_status::wr_flush_error);
    REQUIRE(result.imm_data == 11u);
}

TEST_CASE("rdma_read: post failure resumes inline with flush error",
          "[rdma][read][error]") {
    one_sided_state st;
    st.read_rc = -22;  // -EINVAL
    state_guard guard{&st};
    dispatcher disp;
    int qp_value = 1;
    connection<one_sided_static_backend> c{&qp_value, disp};

    char payload[4] = {};
    buffer_view local{payload, 4, 0};
    remote_buffer remote{0xDEAD, 4, 0x1};

    wc_result result{};
    bool done = false;
    auto task = run_read(c, local, remote, result, done);

    REQUIRE(done);
    REQUIRE_FALSE(result.ok());
    REQUIRE(result.status == wc_status::wr_flush_error);
    REQUIRE(result.imm_data == 22u);
}

TEST_CASE("rdma_write: orphan race — coroutine destroyed before CQE",
          "[rdma][write][orphan]") {
    one_sided_state st;
    state_guard guard{&st};
    dispatcher disp;
    int qp_value = 9;
    connection<one_sided_static_backend> c{&qp_value, disp};

    char payload[8] = {};
    buffer_view local{payload, sizeof(payload), 0};
    remote_buffer remote{0xABCD, 8, 0};

    wr_id captured = 0;
    {
        wc_result result{};
        bool done = false;
        auto task = run_write(c, local, remote, send_flags{}, result, done);
        REQUIRE_FALSE(done);
        captured = st.last_write_id;
        REQUIRE(captured != 0);
    }
    disp.deliver(captured, wc_status::remote_access_error, 0);
    SUCCEED();
}

TEST_CASE("rdma_read: orphan race — coroutine destroyed before CQE",
          "[rdma][read][orphan]") {
    one_sided_state st;
    state_guard guard{&st};
    dispatcher disp;
    int qp_value = 13;
    connection<one_sided_static_backend> c{&qp_value, disp};

    char payload[8] = {};
    buffer_view local{payload, sizeof(payload), 0};
    remote_buffer remote{0xBEEF, 8, 0};

    wr_id captured = 0;
    {
        wc_result result{};
        bool done = false;
        auto task = run_read(c, local, remote, result, done);
        REQUIRE_FALSE(done);
        captured = st.last_read_id;
    }
    disp.deliver(captured, wc_status::retry_exceeded, 0);
    SUCCEED();
}

TEST_CASE("rdma_write via polymorphic_backend dispatches through the vtable",
          "[rdma][write][polymorphic]") {
    one_sided_poly_backend impl{};
    dispatcher disp;
    int qp_value = 55;
    connection<polymorphic_backend> c{&qp_value, impl, disp};

    alignas(8) char payload[32] = {};
    buffer_view local{payload, sizeof(payload), 0x5555};
    remote_buffer remote{
        .addr   = 0x1111'2222'3333'4444ull,
        .length = 32,
        .rkey   = 0x6666,
    };

    wc_result result{};
    bool done = false;
    auto task = run_write(c, local, remote, send_flags{}, result, done);

    REQUIRE_FALSE(done);
    REQUIRE(impl.state.writes.load() == 1);
    REQUIRE(impl.state.last_qp == &qp_value);
    REQUIRE(impl.state.last_write_sge.addr == payload);
    REQUIRE(impl.state.last_write_remote.addr == remote.addr);
    REQUIRE(impl.state.last_write_remote.rkey == 0x6666u);

    disp.deliver(impl.state.last_write_id, wc_status::success, 0);
    REQUIRE(done);
    REQUIRE(result.ok());
}

TEST_CASE("rdma_read via polymorphic_backend dispatches through the vtable",
          "[rdma][read][polymorphic]") {
    one_sided_poly_backend impl{};
    dispatcher disp;
    int qp_value = 66;
    connection<polymorphic_backend> c{&qp_value, impl, disp};

    alignas(8) char payload[128] = {};
    buffer_view local{payload, sizeof(payload), 0x7777};
    remote_buffer remote{
        .addr   = 0xAAAA'BBBB'CCCC'DDDDull,
        .length = 128,
        .rkey   = 0x8888,
    };

    wc_result result{};
    bool done = false;
    auto task = run_read(c, local, remote, result, done);

    REQUIRE_FALSE(done);
    REQUIRE(impl.state.reads.load() == 1);
    REQUIRE(impl.state.last_read_remote.rkey == 0x8888u);

    disp.deliver(impl.state.last_read_id, wc_status::success, 100);
    REQUIRE(done);
    REQUIRE(result.byte_len == 100u);
}
