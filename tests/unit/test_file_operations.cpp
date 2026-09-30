#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
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

struct native_control {
    uint8_t capabilities = elio::io::detail::native_file_sync |
        elio::io::detail::native_file_allocate | elio::io::detail::native_file_truncate;
    int32_t result = 0;
    std::atomic<bool> admitted{false}, release{false};
    std::atomic<elio::io::io_backend*> backend{nullptr};
    std::atomic<unsigned> completions{0};
    std::atomic<int> fd_error{0};
    std::atomic<bool> resume_failed{false};

    void unblock() noexcept {
        if (!release.exchange(true, std::memory_order_acq_rel)) {
            if (auto* owner = backend.load(std::memory_order_acquire)) owner->notify();
        }
    }
};

// Simulate backend admission/completion without depending on kernel io_uring.
// Non-file operations and cross-thread wakeups retain the real epoll path.
class controlled_file_backend : public elio::io::io_backend {
public:
    explicit controlled_file_backend(native_control& control) : control_(control) {
        control_.backend.store(this, std::memory_order_release);
    }
    ~controlled_file_backend() override {
        control_.backend.store(nullptr, std::memory_order_release);
    }
    bool supports_file_operation(io_op operation) const noexcept override {
        const auto bit = operation == io_op::file_sync ? elio::io::detail::native_file_sync
            : operation == io_op::file_allocate ? elio::io::detail::native_file_allocate
            : operation == io_op::file_truncate ? elio::io::detail::native_file_truncate : 0;
        return (control_.capabilities & bit) != 0;
    }
    bool prepare(const elio::io::io_request& request) override {
        if (!supports_file_operation(request.op)) return wake_.prepare(request);
        if (pending_) return false;
        pending_ = request;
        pending_count_.store(1, std::memory_order_release);
        control_.admitted.store(true, std::memory_order_release);
        return true;
    }
    int submit() override { return wake_.submit(); }
    int poll(std::chrono::milliseconds timeout) override {
        if (!pending_ || !control_.release.load(std::memory_order_acquire)) {
            return wake_.poll(timeout);
        }
        const auto request = *pending_;
        pending_.reset();
        pending_count_.store(0, std::memory_order_release);
        struct stat metadata{};
        if (::fstat(request.fd, &metadata) != 0) control_.fd_error.store(errno);
        auto* state = request.state;
        const auto handle = state->handle;
        state->result = control_.result;
        state->flags = 0;
        state->operation_guard.release();
        uint8_t expected = elio::io::op_state::phase_pending;
        if (!state->phase.compare_exchange_strong(expected, elio::io::op_state::phase_completed,
                std::memory_order_acq_rel, std::memory_order_acquire)) {
            delete state;
            return 1;
        }
        control_.completions.fetch_add(1, std::memory_order_relaxed);
        if (!elio::runtime::get_current_scheduler()->try_schedule(handle)) {
            control_.resume_failed.store(true, std::memory_order_release);
        }
        return 1;
    }
    bool has_pending() const noexcept override {
        return pending_count_.load(std::memory_order_acquire) != 0 || wake_.has_pending();
    }
    size_t pending_count() const noexcept override {
        return pending_count_.load(std::memory_order_acquire) + wake_.pending_count();
    }
    bool cancel(void* data) override { return wake_.cancel(data); }
    void notify() noexcept override { wake_.notify(); }
    void drain_notify() override { wake_.drain_notify(); }
private:
    native_control& control_;
    elio::io::epoll_backend wake_;
    std::optional<elio::io::io_request> pending_;
    std::atomic<size_t> pending_count_{0};
};

std::atomic<native_control*> active_native_control{nullptr};
elio::io::io_backend* make_controlled_backend(size_t) {
    return new controlled_file_backend(*active_native_control.load(std::memory_order_acquire));
}
struct native_factory_guard {
    explicit native_factory_guard(native_control& control) {
        active_native_control.store(&control, std::memory_order_release);
        elio::io::detail::worker_backend_factory_for_test.store(
            make_controlled_backend, std::memory_order_release);
    }
    ~native_factory_guard() {
        elio::io::detail::worker_backend_factory_for_test.store(nullptr, std::memory_order_release);
        active_native_control.store(nullptr, std::memory_order_release);
    }
};
struct native_release_guard {
    native_control& control;
    ~native_release_guard() { control.unblock(); }
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

TEST_CASE("a scheduler starting thread is not a file-operation worker",
          "[io][file_operations][context]") {
    syscall_control control;
    hook_guard hooks(control);
    elio::runtime::scheduler scheduler(1);
    scheduler.start();
    REQUIRE(elio::runtime::get_current_scheduler() == &scheduler);
    REQUIRE(elio::runtime::worker_thread::current() == nullptr);
    file_status result;
    SECTION("sync") { result = run_immediate(elio::io::sync_file(42)); }
    SECTION("allocate") { result = run_immediate(elio::io::allocate_file_range(42, 0, 0, 8)); }
    SECTION("truncate") { result = run_immediate(elio::io::truncate_file(42, 8)); }
    REQUIRE(result.end == file_operation_end::error);
    REQUIRE(result.error_value() == ENOTSUP);
    REQUIRE_FALSE(control.entered.load(std::memory_order_acquire));
    REQUIRE(scheduler.shutdown(elio::test::scaled_ms(5000)));
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
    elio::runtime::detail::graceful_admission_closed_for_test.store(
        false, std::memory_order_release);
    std::atomic<bool> shutdown_done{false};
    bool drained = false;
    std::thread shutdown([&] {
        drained = scheduler.shutdown();
        shutdown_done.store(true, std::memory_order_release);
    });
    const bool started = wait_for([&] {
        return elio::runtime::detail::graceful_admission_closed_for_test.load(
            std::memory_order_acquire);
    });
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

TEST_CASE("worker-side pool teardown rejects queued file syscalls",
          "[io][file_operations][shutdown][blocking]") {
    backend_guard backend_choice(io_context::backend_type::epoll);
    syscall_control control;
    control.block = true;
    hook_guard hooks(control);
    elio::coro::cancel_source source;
    bool cancel_queued = false;
    SECTION("uncancelled queued work reports rejection") {}
    SECTION("queued cancellation retains precedence") { cancel_queued = true; }
    file_status running, queued;
    bool queued_done = false, shutdown_done = false;
    elio::runtime::scheduler scheduler(1, elio::runtime::wait_strategy::blocking(), 1);
    release_guard release{control};
    scheduler.start();
    scheduler.go([&]() -> task<void> {
        running = co_await elio::io::sync_file(42);
    });
    const bool entered = wait_for([&] { return control.entered.load(std::memory_order_acquire); });
    scheduler.go([&]() -> task<void> {
        queued = co_await elio::io::sync_file(43, file_sync_mode::data_only, source.get_token());
        queued_done = true;
    });
    auto* pool = scheduler.get_blocking_pool();
    const bool parked = wait_for([&] { return pool->queued_count_for_test() == 1; });
    if (cancel_queued) source.cancel();
    scheduler.go([&]() -> task<void> {
        // This is the same pool drain invoked by worker-side shutdown_force,
        // but keeps the scheduler alive so its returned value is observable.
        pool->shutdown();
        shutdown_done = true;
        co_return;
    });
    const bool stopped = wait_for([&] { return pool->stopped_for_test(); });
    control.unblock();
    const bool drained = scheduler.shutdown(elio::test::scaled_ms(5000));
    REQUIRE(entered);
    REQUIRE(parked);
    REQUIRE(stopped);
    REQUIRE(drained);
    REQUIRE(shutdown_done);
    REQUIRE(queued_done);
    REQUIRE(running);
    REQUIRE(queued.end == (cancel_queued ? file_operation_end::cancelled : file_operation_end::error));
    REQUIRE(queued.error_value() == (cancel_queued ? ECANCELED : EAGAIN));
    REQUIRE(control.calls[0].load() == 1);
    REQUIRE_FALSE(control.on_worker.load(std::memory_order_acquire));
}

TEST_CASE("bounded pool admission does not create standalone per-call threads",
          "[blocking_pool][file_operations]") {
    elio::runtime::blocking_pool pool(0);
    std::function<void()> work = [] {};
    REQUIRE_FALSE(pool.submit_bounded(std::move(work), 1));
    REQUIRE(static_cast<bool>(work));
    pool.shutdown();
}

TEST_CASE("native file completion wins over cancellation after admission",
          "[io][file_operations][native][cancel]") {
    const auto operation = GENERATE(io_op::file_sync, io_op::file_allocate, io_op::file_truncate);
    const int32_t completion = GENERATE(int32_t{0}, int32_t{-EIO});
    char path[] = "/tmp/elio_native_file_ops_XXXXXX";
    elio::io::fd_guard fd(::mkstemp(path));
    REQUIRE(fd.get() >= 0);
    REQUIRE(::unlink(path) == 0);
    syscall_control fallback;
    hook_guard hooks(fallback);
    native_control control;
    control.result = completion;
    native_factory_guard factory(control);
    elio::coro::cancel_source source;
    file_status result;
    std::atomic<bool> done{false};
    elio::runtime::scheduler scheduler(1);
    native_release_guard release{control};
    scheduler.start();
    scheduler.go([&]() -> task<void> {
        if (operation == io_op::file_sync) {
            result = co_await elio::io::sync_file(fd.get(), file_sync_mode::data_only, source.get_token());
        } else if (operation == io_op::file_allocate) {
            result = co_await elio::io::allocate_file_range(fd.get(), 0, 0, 8, source.get_token());
        } else {
            result = co_await elio::io::truncate_file(fd.get(), 8, source.get_token());
        }
        done.store(true, std::memory_order_release);
    });
    const bool admitted = wait_for([&] { return control.admitted.load(std::memory_order_acquire); });
    source.cancel();
    const bool early = done.load(std::memory_order_acquire);
    struct stat metadata{};
    const bool fd_valid_while_held = ::fstat(fd.get(), &metadata) == 0;
    control.unblock();
    const bool drained = scheduler.shutdown(elio::test::scaled_ms(5000));
    REQUIRE(admitted);
    REQUIRE_FALSE(early);
    REQUIRE(fd_valid_while_held);
    REQUIRE(drained);
    REQUIRE(done.load(std::memory_order_acquire));
    REQUIRE(result.end == (completion == 0 ? file_operation_end::complete : file_operation_end::error));
    REQUIRE(result.error_value() == (completion == 0 ? 0 : EIO));
    REQUIRE(control.completions.load() == 1);
    REQUIRE(control.fd_error.load() == 0);
    REQUIRE_FALSE(control.resume_failed.load());
    REQUIRE_FALSE(fallback.entered.load(std::memory_order_acquire));
}

#if ELIO_HAS_IO_URING
TEST_CASE("a null native probe disables every file capability and selects fallback",
          "[io][file_operations][capability][blocking]") {
    native_control control;
    control.capabilities = elio::io::detail::native_file_capabilities(nullptr);
    REQUIRE(control.capabilities == 0);
    native_factory_guard factory(control);
    syscall_control fallback;
    hook_guard hooks(fallback);
    std::array<bool, 3> capabilities{};
    std::array<file_status, 4> results{};
    elio::runtime::scheduler scheduler(1, elio::runtime::wait_strategy::blocking(), 1);
    scheduler.start();
    scheduler.go([&]() -> task<void> {
        auto& context = elio::io::current_io_context();
        capabilities = {context.supports_file_operation(io_op::file_sync),
                        context.supports_file_operation(io_op::file_allocate),
                        context.supports_file_operation(io_op::file_truncate)};
        results[0] = co_await elio::io::sync_file(42);
        results[1] = co_await elio::io::allocate_file_range(42, 0, 0, 8);
        results[2] = co_await elio::io::truncate_file(42, 8);
        results[3] = co_await elio::io::sync_file(42, file_sync_mode::data_only, {}, {.max_queued = 0});
    });
    REQUIRE(scheduler.shutdown(elio::test::scaled_ms(5000)));
    for (bool capability : capabilities) REQUIRE_FALSE(capability);
    for (size_t index = 0; index < 3; ++index) {
        REQUIRE(results[index]);
        REQUIRE(fallback.calls[index].load() == 1);
    }
    REQUIRE(results[3].error_value() == EINVAL);
    REQUIRE_FALSE(fallback.on_worker.load(std::memory_order_acquire));
    REQUIRE_FALSE(control.admitted.load(std::memory_order_acquire));
}
#endif

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
