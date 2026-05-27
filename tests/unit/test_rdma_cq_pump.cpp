// Stage S7 — cq_pump coroutine: io_context-bound CQ pump.
//
// Real RDMA-Core uses an `ibv_comp_channel` fd to signal CQE
// readiness; cq_pump abstracts the "wait on fd, then drain" loop.
// Here we substitute `eventfd` for the channel: writes from the
// test thread wake the pump, the drain callable consumes the
// readiness and invokes dispatcher.deliver() for each pending WR.
//
// Coverage:
//   * Single signal → single drain call → dispatcher delivers.
//   * Multiple signals processed in order.
//   * cancel_token stops the pump cleanly (token + eventfd wake to
//     unblock the in-flight poll).
//   * The drain receives the *same* dispatcher instance every time.

#include <catch2/catch_test_macros.hpp>

#include <elio/coro/cancel_token.hpp>
#include <elio/coro/task.hpp>
#include <elio/rdma/rdma.hpp>
#include <elio/runtime/scheduler.hpp>

#include <sys/eventfd.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <coroutine>
#include <cstdint>
#include <mutex>
#include <queue>
#include <thread>

using elio::coro::cancel_source;
using elio::coro::cancel_token;
using elio::coro::task;
using elio::rdma::cq_pump;
using elio::rdma::detail::op_state;
using elio::rdma::dispatcher;
using elio::rdma::wc_status;
using elio::rdma::wr_id;
using elio::runtime::scheduler;

namespace {

// Test fixture: an eventfd that stands in for an ibv_comp_channel
// fd, plus a thread-safe queue of pending wr_ids the test pushes to.
struct mock_cq {
    int                fd;
    std::mutex         mu;
    std::queue<wr_id>  pending;

    explicit mock_cq() : fd(::eventfd(0, EFD_NONBLOCK)) {
        if (fd < 0) throw std::runtime_error("eventfd failed");
    }
    ~mock_cq() { if (fd >= 0) ::close(fd); }
    mock_cq(const mock_cq&) = delete;
    mock_cq& operator=(const mock_cq&) = delete;

    void enqueue_and_signal(wr_id id) {
        {
            std::lock_guard lg{mu};
            pending.push(id);
        }
        std::uint64_t one = 1;
        auto written = ::write(fd, &one, sizeof(one));
        (void)written;
    }

    // Wake a blocked pump without enqueueing work (used by the
    // cancellation test to unblock async_poll_read after cancel()).
    void wake() {
        std::uint64_t one = 1;
        auto written = ::write(fd, &one, sizeof(one));
        (void)written;
    }
};

}  // namespace

TEST_CASE("cq_pump: drain runs on each fd readiness signal",
          "[rdma][cq_pump]") {
    scheduler sched(2);
    sched.start();

    mock_cq cq;
    dispatcher disp;
    cancel_source src;

    // Each drained id resumes the corresponding op_state.handle (a
    // probe coroutine). The test owns the heap nodes through
    // unique_ptr so destruction releases them.
    std::atomic<int>           delivered{0};
    std::vector<std::unique_ptr<op_state>> ops;

    // Bound dispatcher reference inside the drain captures by ref.
    auto drain = [&](dispatcher& d) noexcept {
        // Consume the eventfd readiness counter.
        std::uint64_t val = 0;
        auto bytes_read = ::read(cq.fd, &val, sizeof(val));
        (void)bytes_read;

        std::vector<wr_id> snapshot;
        {
            std::lock_guard lg{cq.mu};
            while (!cq.pending.empty()) {
                snapshot.push_back(cq.pending.front());
                cq.pending.pop();
            }
        }
        for (auto id : snapshot) {
            d.deliver(id, wc_status::success, /*byte_len=*/64);
            delivered.fetch_add(1);
        }
    };

    sched.go([&]() -> task<void> {
        co_await cq_pump(cq.fd, disp, drain, src.get_token());
    });

    // Push three completions through the pump. Each one is a
    // freshly-allocated op_state with no coroutine handle (we only
    // care that deliver was called).
    for (int i = 0; i < 3; ++i) {
        auto op = std::make_unique<op_state>();
        auto id = dispatcher::make_wr_id(op.get());
        ops.push_back(std::move(op));
        cq.enqueue_and_signal(id);
    }

    // Spin-wait briefly for the pump to process all three.
    auto deadline = std::chrono::steady_clock::now()
                  + std::chrono::seconds(2);
    while (delivered.load() < 3
           && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    REQUIRE(delivered.load() == 3);

    src.cancel();
    cq.wake();  // unblock the in-flight poll so the loop exits

    REQUIRE(sched.shutdown(std::chrono::milliseconds(5000)));
}

TEST_CASE("cq_pump: cancel_token stops the loop after the next drain",
          "[rdma][cq_pump][cancel]") {
    scheduler sched(2);
    sched.start();

    mock_cq cq;
    dispatcher disp;
    cancel_source src;
    std::atomic<int> drain_calls{0};

    auto drain = [&](dispatcher&) noexcept {
        std::uint64_t val = 0;
        auto bytes_read = ::read(cq.fd, &val, sizeof(val));
        (void)bytes_read;
        drain_calls.fetch_add(1);
    };

    sched.go([&]() -> task<void> {
        co_await cq_pump(cq.fd, disp, drain, src.get_token());
    });

    // One genuine signal so we know the pump is running.
    cq.wake();
    auto deadline = std::chrono::steady_clock::now()
                  + std::chrono::seconds(2);
    while (drain_calls.load() < 1
           && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    REQUIRE(drain_calls.load() >= 1);

    // Cancellation arrives mid-flight. The pump's next iteration
    // observes is_cancelled() and exits without invoking drain again
    // for spurious wakes. We still write to the fd to unblock the
    // already-issued poll.
    src.cancel();
    cq.wake();

    REQUIRE(sched.shutdown(std::chrono::milliseconds(5000)));
}

TEST_CASE("cq_pump: dispatcher passed to drain is the same instance",
          "[rdma][cq_pump]") {
    scheduler sched(2);
    sched.start();

    mock_cq cq;
    dispatcher disp;
    cancel_source src;

    std::atomic<const dispatcher*> seen{nullptr};
    auto drain = [&](dispatcher& d) noexcept {
        std::uint64_t val = 0;
        auto bytes_read = ::read(cq.fd, &val, sizeof(val));
        (void)bytes_read;
        seen.store(&d);
    };

    sched.go([&]() -> task<void> {
        co_await cq_pump(cq.fd, disp, drain, src.get_token());
    });

    cq.wake();
    auto deadline = std::chrono::steady_clock::now()
                  + std::chrono::seconds(2);
    while (seen.load() == nullptr
           && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    REQUIRE(seen.load() == &disp);

    src.cancel();
    cq.wake();
    REQUIRE(sched.shutdown(std::chrono::milliseconds(5000)));
}
