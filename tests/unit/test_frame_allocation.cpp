#include <catch2/catch_test_macros.hpp>

#include <elio/coro/frame.hpp>
#include <elio/coro/task.hpp>
#include <elio/io/io_awaitables.hpp>
#include <elio/runtime/async_main.hpp>
#include <elio/runtime/scheduler.hpp>

#include <atomic>
#include <cstdint>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstring>
#include <mutex>
#include <thread>
#include <type_traits>

#include <fcntl.h>
#include <unistd.h>

#include "../test_main.cpp"

using namespace elio::coro;
using namespace elio::runtime;
using namespace elio::test;

namespace {

template<typename Promise>
concept has_class_specific_allocation = requires(std::size_t size, void* ptr) {
    Promise::operator new(size);
    Promise::operator delete(ptr, size);
};

task<void> empty_task() {
    co_return;
}

task<int> deep_task_chain(int depth) {
    if (depth == 0) {
        co_return 1;
    }
    co_return co_await deep_task_chain(depth - 1) + 1;
}

task<void> run_set_flag(std::atomic<bool>* executed) {
    executed->store(true, std::memory_order_release);
    co_return;
}

task<void> observe_over_aligned_local(std::atomic<bool>* aligned) {
    alignas(64) std::byte buffer[64];
    aligned->store(reinterpret_cast<std::uintptr_t>(buffer) % 64 == 0,
                   std::memory_order_release);
    co_return;
}

task<void> observe_nested_over_aligned_locals(std::atomic<int>* aligned_count) {
    auto inner = [aligned_count]() -> task<void> {
        alignas(64) std::byte buffer[64];
        if (reinterpret_cast<std::uintptr_t>(buffer) % 64 == 0) {
            aligned_count->fetch_add(1, std::memory_order_relaxed);
        }
        co_return;
    };

    for (int i = 0; i < 10; ++i) {
        co_await inner();
    }
}

struct destruction_observation {
    std::thread::id thread_id;
    bool called = false;
};

struct io_observation {
    std::mutex mutex;
    std::condition_variable cv;
    bool completed = false;
    bool frame_preserved = false;
    int result = 0;
};

task<void> read_and_observe_frame(int fd, char* buffer, std::size_t size,
                                  io_observation* observation) {
    auto* frame_before = promise_base::current_frame();
    auto result = co_await elio::io::async_read(fd, buffer, size);
    auto* frame_after = promise_base::current_frame();

    {
        std::lock_guard lock(observation->mutex);
        observation->frame_preserved = frame_before != nullptr &&
                                       frame_after == frame_before;
        observation->result = result.result;
        observation->completed = true;
    }
    observation->cv.notify_one();
}

} // namespace

TEST_CASE("task promises use standard coroutine frame allocation",
          "[task][frame_allocation]") {
    STATIC_REQUIRE_FALSE(has_class_specific_allocation<task<void>::promise_type>);
    STATIC_REQUIRE_FALSE(has_class_specific_allocation<task<int>::promise_type>);
}

TEST_CASE("detached task frame can be destroyed on another thread",
          "[task][frame_allocation][thread]") {
    auto* previous_frame = promise_base::current_frame();
    auto t = empty_task();
    auto handle = elio::coro::detail::task_access::release(t);
    destruction_observation observation;

    handle.promise().on_spawn_completion_data_ = &observation;
    handle.promise().on_spawn_completion_ = +[](void* data) noexcept {
        auto* observed = static_cast<destruction_observation*>(data);
        observed->thread_id = std::this_thread::get_id();
        observed->called = true;
    };
    handle.promise().detach_from_parent();
    promise_base::set_current_frame(previous_frame);

    const auto owner_thread = std::this_thread::get_id();
    std::thread destroyer([handle]() mutable { handle.destroy(); });
    destroyer.join();

    REQUIRE(observation.called);
    REQUIRE(observation.thread_id != owner_thread);
    REQUIRE(promise_base::current_frame() == previous_frame);
}

TEST_CASE("standard task frames support deep co_await chains",
          "[task][frame_allocation]") {
    REQUIRE(elio::run([]() -> task<int> {
        co_return co_await deep_task_chain(32);
    }) == 33);
}

TEST_CASE("elio::run supports void root tasks with standard frames",
          "[task][frame_allocation][run]") {
    std::atomic<bool> executed{false};
    elio::run(run_set_flag, &executed);
    REQUIRE(executed.load(std::memory_order_acquire));
}

TEST_CASE("standard task frames preserve over-aligned locals",
          "[task][frame_allocation][alignment]") {
    scheduler sched(1);
    sched.start();

    std::atomic<bool> aligned{false};
    auto joined = sched.go_joinable(observe_over_aligned_local, &aligned);
    joined.wait_destroyed();
    sched.shutdown();

    REQUIRE(aligned.load(std::memory_order_acquire));
}

TEST_CASE("nested standard task frames preserve over-aligned locals",
          "[task][frame_allocation][alignment]") {
    scheduler sched(1);
    sched.start();

    std::atomic<int> aligned_count{0};
    auto joined = sched.go_joinable(observe_nested_over_aligned_locals,
                                    &aligned_count);
    joined.wait_destroyed();
    sched.shutdown();

    REQUIRE(aligned_count.load(std::memory_order_acquire) == 10);
}

TEST_CASE("I/O completion restores the awaiting frame context",
          "[task][frame_allocation][io]") {
    int pipe_fds[2];
    REQUIRE(::pipe2(pipe_fds, O_CLOEXEC | O_NONBLOCK) == 0);

    scheduler sched(1);
    sched.start();

    char buffer[32]{};
    io_observation observation;
    auto joined = sched.go_joinable(read_and_observe_frame, pipe_fds[0],
                                    buffer, sizeof(buffer), &observation);

    constexpr char payload[] = "frame-context";
    REQUIRE(::write(pipe_fds[1], payload, sizeof(payload)) ==
            static_cast<ssize_t>(sizeof(payload)));

    {
        std::unique_lock lock(observation.mutex);
        REQUIRE(observation.cv.wait_for(
            lock, scaled_ms(2000), [&] { return observation.completed; }));
    }
    joined.wait_destroyed();
    sched.shutdown();

    REQUIRE(observation.result == static_cast<int>(sizeof(payload)));
    REQUIRE(observation.frame_preserved);
    REQUIRE(std::memcmp(buffer, payload, sizeof(payload)) == 0);

    ::close(pipe_fds[0]);
    ::close(pipe_fds[1]);
}
