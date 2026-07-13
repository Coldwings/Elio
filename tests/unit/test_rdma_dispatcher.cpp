// Stage S1 — dispatcher and op_state lifecycle tests.
//
// The dispatcher's central invariant: exactly one party (the dispatcher
// itself on CQE arrival, or the awaiter's destructor) frees the heap
// `op_state` node, and the awaited coroutine is resumed at most once.
// These tests exercise both arms of the race and verify the no-resume
// orphan path.
//
// Awaiters and a real backend land in S2/S3; here we drive the
// dispatcher directly with manually-constructed op_state nodes and a
// minimal test coroutine. `runtime::schedule_handle` resumes
// synchronously when no scheduler is current (the same behaviour
// `sync` primitives and `cancel_token` rely on for non-scheduler
// callers), which makes the test deterministic and avoids spinning up
// a scheduler just to observe the resume.

#include <catch2/catch_test_macros.hpp>

#include <elio/rdma/rdma.hpp>

#include <atomic>
#include <coroutine>
#include <memory>

using elio::rdma::dispatcher;
using elio::rdma::wc_result;
using elio::rdma::wc_status;
using elio::rdma::wr_id;
using elio::rdma::detail::op_phase;
using elio::rdma::detail::op_state;

namespace {

// Minimal coroutine type used purely to fabricate a `coroutine_handle<>`
// for the dispatcher to resume. Body starts suspended (caller hands the
// handle to the dispatcher first); final_suspend also suspends so the
// caller controls destruction.
struct probe_coroutine {
    struct promise_type {
        probe_coroutine get_return_object() noexcept {
            return probe_coroutine{
                std::coroutine_handle<promise_type>::from_promise(*this)};
        }
        std::suspend_always initial_suspend() noexcept { return {}; }
        std::suspend_always final_suspend() noexcept { return {}; }
        void return_void() noexcept {}
        void unhandled_exception() {}
    };

    std::coroutine_handle<promise_type> handle;

    explicit probe_coroutine(std::coroutine_handle<promise_type> h) noexcept
        : handle(h) {}

    ~probe_coroutine() {
        if (handle) handle.destroy();
    }
    probe_coroutine(probe_coroutine&&) = delete;
    probe_coroutine& operator=(probe_coroutine&&) = delete;
};

// Body parameters are captured by reference; the body runs on the first
// (and only) handle.resume() call from the dispatcher.
probe_coroutine make_probe(std::atomic<int>& resume_counter) {
    resume_counter.fetch_add(1, std::memory_order_acq_rel);
    co_return;
}

std::unique_ptr<op_state> make_pending_op() {
    auto op = std::make_unique<op_state>();
    op->phase.store(op_phase::pending);
    return op;
}

}  // namespace

TEST_CASE("dispatcher::make_wr_id is invertible", "[rdma][dispatcher]") {
    auto op = std::make_unique<op_state>();
    REQUIRE(op->phase.load() == op_phase::unstarted);
    const auto id = dispatcher::make_wr_id(op.get());
    REQUIRE(dispatcher::decode_wr_id(id) == op.get());

    SECTION("null pointer round-trips to a null wr_id decode") {
        REQUIRE(dispatcher::decode_wr_id(0) == nullptr);
    }
}

TEST_CASE("dispatcher delivers a single completion to the waiting coroutine",
          "[rdma][dispatcher]") {
    dispatcher disp;

    std::atomic<int> resumes{0};
    auto coro = make_probe(resumes);

    auto op = make_pending_op();
    op->handle = coro.handle;

    const auto id = dispatcher::make_wr_id(op.get());

    disp.deliver(id, wc_status::success, /*byte_len=*/128,
                 /*imm_data=*/0xDEADBEEF, /*wc_flags=*/0x4);

    // schedule_handle resumed inline (no current scheduler). The
    // coroutine body bumped the counter via return_void.
    REQUIRE(resumes.load() == 1);

    // Dispatcher should have CASed to completed; awaiter (us) still
    // owns the unique_ptr. Verify the result was written.
    REQUIRE(op->phase.load() == op_phase::completed);
    REQUIRE(op->result.status == wc_status::success);
    REQUIRE(op->result.byte_len == 128u);
    REQUIRE(op->result.imm_data == 0xDEADBEEFu);
    REQUIRE(op->result.wc_flags == 0x4u);
    REQUIRE(op->result.ok());

    // unique_ptr frees op here; test ends without leaks.
}

TEST_CASE("dispatcher silently drops a CQE for an orphaned op_state",
          "[rdma][dispatcher]") {
    dispatcher disp;

    auto op = make_pending_op();
    // No handle assigned — this op was never tied to a coroutine; the
    // test only cares about the lifecycle race.

    // Awaiter destructor wins the race: it CASes pending → orphaned and
    // releases the unique_ptr. The dispatcher must take ownership when
    // the late CQE arrives.
    REQUIRE(dispatcher::try_orphan(op.get()));
    REQUIRE(op->phase.load() == op_phase::orphaned);
    auto* raw_op = op.release();

    const auto id = dispatcher::make_wr_id(raw_op);
    // No assertion needed on success here — the contract is "must not
    // crash and must free the heap node". A leak would show under ASAN
    // even though the test would otherwise pass.
    disp.deliver(id, wc_status::wr_flush_error, /*byte_len=*/0);
}

TEST_CASE("dispatcher race: deliver-before-orphan leaves the awaiter to free",
          "[rdma][dispatcher]") {
    dispatcher disp;

    std::atomic<int> resumes{0};
    auto coro = make_probe(resumes);

    auto op = make_pending_op();
    op->handle = coro.handle;

    // Dispatcher wins.
    disp.deliver(dispatcher::make_wr_id(op.get()),
                 wc_status::success, /*byte_len=*/16);
    REQUIRE(resumes.load() == 1);
    REQUIRE(op->phase.load() == op_phase::completed);

    // Awaiter destructor runs afterwards: try_orphan must return false
    // (state already completed) so the unique_ptr destructor frees `op`.
    REQUIRE_FALSE(dispatcher::try_orphan(op.get()));
    // unique_ptr frees on scope exit. No leak.
}

TEST_CASE("dispatcher race: orphan-before-deliver hands the free to dispatcher",
          "[rdma][dispatcher]") {
    dispatcher disp;

    auto op = make_pending_op();
    // Coroutine deliberately absent — awaiter went away first, the
    // dispatcher must not even try to read `handle`.

    REQUIRE(dispatcher::try_orphan(op.get()));
    REQUIRE(op->phase.load() == op_phase::orphaned);
    auto* raw_op = op.release();

    // Repeated deliver calls on an orphaned wr_id would be UAF after
    // the first one frees the heap node — production CQE rings can't
    // double-deliver because each wr_id is unique per WR, but we still
    // assert "exactly one deliver per wr_id" is the contract. Run only
    // one here, ASAN catches any heap mismatch.
    disp.deliver(dispatcher::make_wr_id(raw_op),
                 wc_status::remote_access_error, /*byte_len=*/0);
}

TEST_CASE("dispatcher handles a no-handle op_state without crashing",
          "[rdma][dispatcher]") {
    dispatcher disp;
    auto op = make_pending_op();
    op->handle = {};  // null handle

    disp.deliver(dispatcher::make_wr_id(op.get()),
                 wc_status::success, 0);
    REQUIRE(op->phase.load() == op_phase::completed);
    REQUIRE(op->result.ok());
}

TEST_CASE("dispatcher ignores a zero wr_id", "[rdma][dispatcher]") {
    dispatcher disp;
    // Convention: backends may use wr_id=0 for no-completion SQEs they
    // post internally (e.g. cancel ops); deliver must treat it as a
    // no-op and not crash.
    disp.deliver(/*id=*/0, wc_status::success, 0);
    SUCCEED();
}
