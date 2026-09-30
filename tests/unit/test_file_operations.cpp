#include <catch2/catch_test_macros.hpp>
#include <elio/io/file_operations.hpp>
#include <elio/io/file_helpers.hpp>
#include "../test_main.cpp"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <functional>
#include <thread>
#include <sys/stat.h>

namespace {

using elio::io::file_operation_end;
using elio::io::file_status;
using elio::io::file_sync_mode;
using elio::io::io_context;
using elio::io::io_op;
using elio::coro::task;

struct syscall_control {
    std::atomic<bool> entered{false};
    std::atomic<bool> release{false};
    std::atomic<bool> on_worker{false};
    std::array<std::atomic<size_t>, 3> calls{};
    bool block = false;
    bool passthrough = false;
    int error = 0;

    void unblock() noexcept {
        release.store(true, std::memory_order_release);
        release.notify_all();
    }
};

std::atomic<syscall_control*> active_control{nullptr};

int observed_syscall(const elio::io::detail::file_operation_request& request) {
    auto* control = active_control.load(std::memory_order_acquire);
    const auto index = request.operation == io_op::file_sync ? 0u
        : request.operation == io_op::file_allocate ? 1u : 2u;
    control->calls[index].fetch_add(1, std::memory_order_relaxed);
    if (elio::runtime::worker_thread::current()) {
        control->on_worker.store(true, std::memory_order_release);
    }
    control->entered.store(true, std::memory_order_release);
    control->entered.notify_all();
    if (control->block) {
        while (!control->release.load(std::memory_order_acquire)) {
            control->release.wait(false, std::memory_order_acquire);
        }
    }
    if (control->error) {
        errno = control->error;
        return -1;
    }
    if (!control->passthrough) return 0;
    switch (request.operation) {
        case io_op::file_sync:
            return request.flags ? ::fdatasync(request.fd) : ::fsync(request.fd);
        case io_op::file_allocate:
            return ::fallocate(request.fd, request.flags,
                static_cast<off_t>(request.offset), static_cast<off_t>(request.length));
        case io_op::file_truncate:
            return ::ftruncate(request.fd, static_cast<off_t>(request.length));
        default:
            errno = EINVAL;
            return -1;
    }
}

struct hook_guard {
    explicit hook_guard(syscall_control& control, bool force_fallback = false) {
        active_control.store(&control, std::memory_order_release);
        elio::io::detail::file_syscall_for_test.store(observed_syscall, std::memory_order_release);
        elio::io::detail::force_file_operations_fallback_for_test.store(
            force_fallback, std::memory_order_release);
    }
    ~hook_guard() {
        elio::io::detail::file_syscall_for_test.store(nullptr, std::memory_order_release);
        elio::io::detail::force_file_operations_fallback_for_test.store(
            false, std::memory_order_release);
        active_control.store(nullptr, std::memory_order_release);
    }
};

struct release_guard {
    syscall_control& control;
    ~release_guard() { control.unblock(); }
};

struct backend_guard {
    explicit backend_guard(io_context::backend_type backend)
        : previous(elio::runtime::detail::worker_io_backend_for_test.exchange(
              backend, std::memory_order_acq_rel)) {}
    ~backend_guard() {
        elio::runtime::detail::worker_io_backend_for_test.store(previous, std::memory_order_release);
    }
    io_context::backend_type previous;
};

template<typename Predicate>
bool wait_for(Predicate predicate) {
    const auto deadline = std::chrono::steady_clock::now() + elio::test::scaled_ms(5000);
    while (!predicate()) {
        if (std::chrono::steady_clock::now() >= deadline) return false;
        std::this_thread::yield();
    }
    return true;
}

file_status run_immediate(task<file_status> operation) {
    const auto handle = elio::coro::detail::task_access::handle(operation);
    handle.resume();
    REQUIRE(handle.done());
    return operation.await_resume();
}

} // namespace

TEST_CASE("file operations validate before requiring a scheduler",
          "[io][file_operations]") {
    file_status result;
    int expected = EINVAL;
    SECTION("invalid sync mode") {
        result = run_immediate(elio::io::sync_file(-1, static_cast<file_sync_mode>(99)));
    }
    SECTION("empty allocation") {
        result = run_immediate(elio::io::allocate_file_range(-1, 0, 0, 0));
    }
    SECTION("overflowing allocation range") {
        result = run_immediate(elio::io::allocate_file_range(
            -1, 0, elio::io::detail::file_operation_max_offset, 1));
        expected = EOVERFLOW;
    }
    SECTION("overflowing truncation") {
        result = run_immediate(elio::io::truncate_file(
            -1, elio::io::detail::file_operation_max_offset + 1));
        expected = EOVERFLOW;
    }
    SECTION("zero queue bound") {
        result = run_immediate(elio::io::sync_file(
            -1, file_sync_mode::data_only, {}, {.max_queued = 0}));
    }
    SECTION("unbounded sentinel is not a file-operation limit") {
        result = run_immediate(elio::io::truncate_file(
            -1, 0, {}, {.max_queued = std::numeric_limits<size_t>::max()}));
    }
    REQUIRE_FALSE(result);
    REQUIRE(result.end == file_operation_end::error);
    REQUIRE(result.error == std::error_code(expected, std::generic_category()));
}

TEST_CASE("standalone file operations reject without detached or inline work",
          "[io][file_operations]") {
    syscall_control control;
    hook_guard hooks(control);
    file_status result;
    SECTION("sync") { result = run_immediate(elio::io::sync_file(42)); }
    SECTION("allocate") { result = run_immediate(elio::io::allocate_file_range(42, 0, 0, 8)); }
    SECTION("truncate") { result = run_immediate(elio::io::truncate_file(42, 8)); }
    REQUIRE_FALSE(result);
    REQUIRE(result.error_value() == ENOTSUP);
    REQUIRE_FALSE(control.entered.load(std::memory_order_acquire));
}

TEST_CASE("already cancelled file operations never invoke a syscall",
          "[io][file_operations][cancel]") {
    elio::coro::cancel_source source;
    source.cancel();
    const auto result = run_immediate(elio::io::sync_file(
        42, file_sync_mode::data_and_metadata, source.get_token()));
    REQUIRE(result.end == file_operation_end::cancelled);
    REQUIRE(result.error_value() == ECANCELED);
}

TEST_CASE("file operations use native capabilities or bounded off-worker syscalls",
          "[io][file_operations][backend][file]") {
    auto run = [](io_context::backend_type backend, bool force_fallback = false) {
        backend_guard backend_choice(backend);
        syscall_control control;
        control.passthrough = true;
        hook_guard hooks(control, force_fallback);
        char path[] = "/tmp/elio_file_ops_XXXXXX";
        elio::io::fd_guard fd(::mkstemp(path));
        REQUIRE(fd.get() >= 0);
        REQUIRE(::unlink(path) == 0);
        std::array<char, 4096> contents{};
        contents.fill('x');
        REQUIRE(::pwrite(fd.get(), contents.data(), contents.size(), 0) == 4096);
        std::array<file_status, 8> results{};
        std::array<bool, 3> native{};
        bool done = false;
        elio::runtime::scheduler scheduler(1);
        scheduler.start();
        scheduler.go([&]() -> task<void> {
            auto& context = elio::io::current_io_context();
            native = {context.supports_file_operation(io_op::file_sync) && !force_fallback,
                      context.supports_file_operation(io_op::file_allocate) && !force_fallback,
                      context.supports_file_operation(io_op::file_truncate) && !force_fallback};
            results[0] = co_await elio::io::sync_file(fd.get(), file_sync_mode::data_only);
            results[1] = co_await elio::io::sync_file(fd.get());
            results[2] = co_await elio::io::allocate_file_range(fd.get(), 0, 8192, 4096);
            results[3] = co_await elio::io::truncate_file(fd.get(), 8192);
            results[4] = co_await elio::io::allocate_file_range(
                fd.get(), FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE, 0, 4096);
            results[5] = co_await elio::io::sync_file(-1);
            results[6] = co_await elio::io::truncate_file(-1, 0);
            results[7] = co_await elio::io::allocate_file_range(fd.get(), 0x40000000, 0, 4096);
            done = true;
        });
        const bool drained = scheduler.shutdown(elio::test::scaled_ms(5000));
        REQUIRE(drained);
        REQUIRE(done);
        REQUIRE(results[0]);
        REQUIRE(results[1]);
        REQUIRE(results[3]);
        // Filesystem capability errors are value results, not backend failures.
        for (size_t index : {size_t{2}, size_t{4}}) {
            if (!results[index]) {
                REQUIRE((results[index].error_value() == EOPNOTSUPP ||
                         results[index].error_value() == ENOSYS));
            }
        }
        REQUIRE(results[5].error_value() == EBADF);
        REQUIRE(results[6].error_value() == EBADF);
        REQUIRE_FALSE(results[7]);
        REQUIRE((results[7].error_value() == EOPNOTSUPP || results[7].error_value() == EINVAL));
        struct stat metadata{};
        REQUIRE(::fstat(fd.get(), &metadata) == 0);
        REQUIRE(metadata.st_size == 8192);
        REQUIRE(::pread(fd.get(), contents.data(), contents.size(), 0) == 4096);
        if (results[4]) {
            REQUIRE(std::all_of(contents.begin(), contents.end(), [](char value) { return value == 0; }));
        }
        REQUIRE(control.calls[0].load() == (native[0] ? 0 : 3));
        REQUIRE(control.calls[1].load() == (native[1] ? 0 : 3));
        REQUIRE(control.calls[2].load() == (native[2] ? 0 : 2));
        REQUIRE_FALSE(control.on_worker.load(std::memory_order_acquire));
    };
    SECTION("forced epoll fallback") { run(io_context::backend_type::epoll); }
#if ELIO_HAS_IO_URING
    SECTION("available io_uring native capabilities") {
        if (!elio::io::io_uring_backend::is_available()) SKIP("io_uring unavailable");
        run(io_context::backend_type::io_uring);
    }
    SECTION("available io_uring with forced capability fallback") {
        if (!elio::io::io_uring_backend::is_available()) SKIP("io_uring unavailable");
        run(io_context::backend_type::io_uring, true);
    }
#endif
}

TEST_CASE("queued file cancellation skips dispatch and admission stays bounded",
          "[io][file_operations][cancel][blocking]") {
    backend_guard backend_choice(io_context::backend_type::epoll);
    syscall_control control;
    control.block = true;
    hook_guard hooks(control);
    elio::coro::cancel_source queued_cancel;
    file_status first, queued, rejected;
    std::atomic<bool> queued_done{false}, rejected_done{false};
    elio::runtime::scheduler scheduler(1, elio::runtime::wait_strategy::blocking(), 1);
    release_guard release{control};
    scheduler.start();
    scheduler.go([&]() -> task<void> {
        first = co_await elio::io::sync_file(42, file_sync_mode::data_only, {}, {.max_queued = 1});
    });
    const bool entered = wait_for([&] { return control.entered.load(std::memory_order_acquire); });
    scheduler.go([&]() -> task<void> {
        queued = co_await elio::io::sync_file(
            43, file_sync_mode::data_only, queued_cancel.get_token(), {.max_queued = 1});
        queued_done.store(true, std::memory_order_release);
    });
    const bool parked = wait_for([&] { return scheduler.get_blocking_pool()->queued_count_for_test() == 1; });
    queued_cancel.cancel();
    scheduler.go([&]() -> task<void> {
        rejected = co_await elio::io::truncate_file(44, 8, {}, {.max_queued = 1});
        rejected_done.store(true, std::memory_order_release);
    });
    const bool refusal_completed = wait_for([&] { return rejected_done.load(std::memory_order_acquire); });
    const bool early = queued_done.load(std::memory_order_acquire);
    control.unblock();
    const bool drained = scheduler.shutdown(elio::test::scaled_ms(5000));
    REQUIRE(entered);
    REQUIRE(parked);
    REQUIRE(refusal_completed);
    REQUIRE_FALSE(early);
    REQUIRE(drained);
    REQUIRE(first);
    REQUIRE(queued.end == file_operation_end::cancelled);
    REQUIRE(queued.error_value() == ECANCELED);
    REQUIRE(rejected.error_value() == EAGAIN);
    REQUIRE(control.calls[0].load() == 1);
    REQUIRE(control.calls[2].load() == 0);
    REQUIRE_FALSE(control.on_worker.load(std::memory_order_acquire));
}

TEST_CASE("running file calls preserve completion and delay graceful shutdown",
          "[io][file_operations][cancel][shutdown]") {
    backend_guard backend_choice(io_context::backend_type::epoll);
    syscall_control control;
    control.block = true;
    SECTION("success wins") {}
    SECTION("errno is captured on the syscall thread") { control.error = EIO; }
    hook_guard hooks(control);
    elio::coro::cancel_source source;
    file_status result;
    std::atomic<bool> done{false};
    elio::runtime::scheduler scheduler(1, elio::runtime::wait_strategy::blocking(), 1);
    release_guard release{control};
    scheduler.start();
    scheduler.go([&]() -> task<void> {
        result = co_await elio::io::sync_file(42, file_sync_mode::data_only, source.get_token());
        done.store(true, std::memory_order_release);
    });
    const bool entered = wait_for([&] { return control.entered.load(std::memory_order_acquire); });
    source.cancel();
    const bool early = done.load(std::memory_order_acquire);
    std::atomic<bool> shutdown_started{false}, shutdown_done{false};
    bool drained = false;
    std::thread shutdown([&] {
        shutdown_started.store(true, std::memory_order_release);
        drained = scheduler.shutdown();
        shutdown_done.store(true, std::memory_order_release);
    });
    const bool started = wait_for([&] { return shutdown_started.load(std::memory_order_acquire); });
    const bool early_shutdown = shutdown_done.load(std::memory_order_acquire);
    errno = ENOSPC;
    control.unblock();
    shutdown.join();
    REQUIRE(entered);
    REQUIRE(started);
    REQUIRE_FALSE(early);
    REQUIRE_FALSE(early_shutdown);
    REQUIRE(drained);
    REQUIRE(done.load(std::memory_order_acquire));
    if (control.error == 0) {
        REQUIRE(result);
        REQUIRE_FALSE(result.error);
    } else {
        REQUIRE(result.end == file_operation_end::error);
        REQUIRE(result.error == std::error_code(EIO, std::generic_category()));
    }
    REQUIRE(control.calls[0].load() == 1);
}

TEST_CASE("bounded pool admission does not create standalone per-call threads",
          "[blocking_pool][file_operations]") {
    elio::runtime::blocking_pool pool(0);
    std::function<void()> work = [] {};
    REQUIRE_FALSE(pool.submit_bounded(std::move(work), 1));
    REQUIRE(static_cast<bool>(work));
    pool.shutdown();
}

#if ELIO_HAS_IO_URING
namespace truncate_header_mock {
struct sqe { int fd = -1; int64_t length = -1; };
void io_uring_prep_ftruncate(sqe* entry, int fd, int64_t length) {
    entry->fd = fd;
    entry->length = length;
}
struct absent_sqe {};
}

TEST_CASE("native truncate preparation follows actual header capability",
          "[io][file_operations][capability]") {
    truncate_header_mock::sqe supported;
    REQUIRE(elio::io::detail::prepare_native_file_truncate(&supported, 42, 99));
    REQUIRE(supported.fd == 42);
    REQUIRE(supported.length == 99);
    truncate_header_mock::absent_sqe unsupported;
    REQUIRE_FALSE(elio::io::detail::prepare_native_file_truncate(&unsupported, 42, 99));
}
#endif
