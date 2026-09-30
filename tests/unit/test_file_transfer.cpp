#include <catch2/catch_test_macros.hpp>
#include <elio/io/file_helpers.hpp>
#include <elio/io/file_transfer.hpp>
#include <elio/runtime/scheduler.hpp>
#include "../test_main.cpp"

#include <array>
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <limits>
#include <vector>
#include <fcntl.h>
#include <sys/mman.h>
#include <unistd.h>

namespace {

using elio::coro::cancel_source;
using elio::io::file_transfer_result;
using elio::io::transfer_end;

struct recorded_request {
    int fd;
    const std::byte* buffer;
    size_t length;
    int64_t offset;
};

struct recorded_io {
    std::vector<int32_t> results;
    std::vector<recorded_request> requests;
    cancel_source* cancel = nullptr;
    size_t cancel_on_call = std::numeric_limits<size_t>::max();
};

struct ready_completion {
    int32_t result;
    bool await_ready() const noexcept { return true; }
    void await_suspend(std::coroutine_handle<>) const noexcept {}
    elio::io::io_result await_resume() const noexcept { return {result, 0}; }
};

struct injected_operation {
    recorded_io& io;

    template<typename Byte>
    auto operator()(int fd, std::span<Byte> buffer, int64_t offset) {
        const auto index = io.requests.size();
        io.requests.push_back({fd, buffer.data(), buffer.size(), offset});
        if (io.cancel && index == io.cancel_on_call) {
            io.cancel->cancel();
        }
        return ready_completion{io.results.at(index)};
    }
};

file_transfer_result run_immediate(elio::coro::task<file_transfer_result> operation) {
    auto handle = elio::coro::detail::task_access::handle(operation);
    handle.resume();
    REQUIRE(handle.done());
    return operation.await_resume();
}

template<bool Write>
file_transfer_result transfer(recorded_io& io, std::span<std::byte> buffer,
                              uint64_t offset = 13, bool exact = true,
                              elio::coro::cancel_token token = {}) {
    return run_immediate(elio::io::detail::transfer_file_range<Write>(
        42, buffer, offset, exact, std::move(token), injected_operation{io}));
}

struct held_io {
    std::span<std::byte> buffer;
    std::coroutine_handle<> continuation;
    int32_t result = 0;
    size_t calls = 0;

    void complete(int32_t count) {
        result = count;
        if (count > 0) {
            std::fill_n(buffer.begin(), count, std::byte{0x5a});
        }
        continuation.resume();
    }
};

struct held_operation {
    held_io& io;

    struct awaitable {
        held_io& io;
        bool await_ready() const noexcept { return false; }
        void await_suspend(std::coroutine_handle<> continuation) const noexcept {
            io.continuation = continuation;
        }
        elio::io::io_result await_resume() const noexcept { return {io.result, 0}; }
    };

    awaitable operator()(int, std::span<std::byte> buffer, int64_t) {
        ++io.calls;
        io.buffer = buffer;
        return {io};
    }
};

class transfer_backend_guard {
public:
    explicit transfer_backend_guard(elio::io::io_context::backend_type backend)
        : previous_(elio::runtime::detail::worker_io_backend_for_test.exchange(
              backend, std::memory_order_acq_rel)) {}
    ~transfer_backend_guard() {
        elio::runtime::detail::worker_io_backend_for_test.store(
            previous_, std::memory_order_release);
    }

private:
    elio::io::io_context::backend_type previous_;
};

} // namespace

TEST_CASE("exact file transfers advance only completed cursors",
          "[io][file_transfer]") {
    std::array<std::byte, 8> buffer{};
    recorded_io io{{2, 3, 3}, {}};
    file_transfer_result result;
    SECTION("read") { result = transfer<false>(io, buffer); }
    SECTION("write") { result = transfer<true>(io, buffer); }
    REQUIRE(result.transferred == buffer.size());
    REQUIRE(result.end == transfer_end::complete);
    REQUIRE_FALSE(result.error);
    REQUIRE(io.requests.size() == 3);
    REQUIRE(io.requests[0].fd == 42);
    REQUIRE(io.requests[0].buffer == buffer.data());
    REQUIRE(io.requests[0].offset == 13);
    REQUIRE(io.requests[0].length == 8);
    REQUIRE(io.requests[1].buffer == buffer.data() + 2);
    REQUIRE(io.requests[1].offset == 15);
    REQUIRE(io.requests[1].length == 6);
    REQUIRE(io.requests[2].buffer == buffer.data() + 5);
    REQUIRE(io.requests[2].offset == 18);
    REQUIRE(io.requests[2].length == 3);
}

TEST_CASE("file transfer errors preserve prior progress",
          "[io][file_transfer]") {
    std::array<std::byte, 8> buffer{};
    recorded_io io{{3, -ENOSPC, 5}, {}};
    file_transfer_result result;
    SECTION("read") { result = transfer<false>(io, buffer); }
    SECTION("write") { result = transfer<true>(io, buffer); }
    REQUIRE(result.transferred == 3);
    REQUIRE(result.end == transfer_end::error);
    REQUIRE(result.error == std::error_code(ENOSPC, std::generic_category()));
    REQUIRE(io.requests.size() == 2);
    REQUIRE(io.requests.back().buffer == buffer.data() + 3);
    REQUIRE(io.requests.back().offset == 16);
}

TEST_CASE("file transfer EINTR retries do not replay prior progress",
          "[io][file_transfer]") {
    std::array<std::byte, 5> buffer{};
    recorded_io io{{2, -EINTR, 3}, {}};
    file_transfer_result result;
    SECTION("read") { result = transfer<false>(io, buffer); }
    SECTION("write") { result = transfer<true>(io, buffer); }
    REQUIRE(result.transferred == 5);
    REQUIRE(result.end == transfer_end::complete);
    REQUIRE(io.requests.size() == 3);
    REQUIRE(io.requests[1].buffer == io.requests[2].buffer);
    REQUIRE(io.requests[1].offset == io.requests[2].offset);
    REQUIRE(io.requests[1].length == io.requests[2].length);
}

TEST_CASE("file transfers distinguish EOF from zero-progress writes",
          "[io][file_transfer]") {
    std::array<std::byte, 8> buffer{};
    recorded_io io{{3, 0, 5}, {}};
    SECTION("read EOF") {
        const auto result = transfer<false>(io, buffer);
        REQUIRE(result.transferred == 3);
        REQUIRE(result.end == transfer_end::eof);
        REQUIRE_FALSE(result.error);
    }
    SECTION("zero write") {
        const auto result = transfer<true>(io, buffer);
        REQUIRE(result.transferred == 3);
        REQUIRE(result.end == transfer_end::error);
        REQUIRE(result.error.value() == EIO);
    }
    REQUIRE(io.requests.size() == 2);
}

TEST_CASE("some file transfers stop after one positive completion",
          "[io][file_transfer]") {
    std::array<std::byte, 8> buffer{};
    recorded_io io{{-EINTR, 3, 5}, {}};
    file_transfer_result result;
    SECTION("read") { result = transfer<false>(io, buffer, 13, false); }
    SECTION("write") { result = transfer<true>(io, buffer, 13, false); }
    REQUIRE(result.transferred == 3);
    REQUIRE(result.end == transfer_end::complete);
    REQUIRE_FALSE(result.error);
    REQUIRE(io.requests.size() == 2);
    REQUIRE(io.requests.front().offset == io.requests.back().offset);
}

TEST_CASE("file transfer EAGAIN is terminal without busy spinning",
          "[io][file_transfer]") {
    std::array<std::byte, 8> buffer{};
    recorded_io io{{-EAGAIN, 8}, {}};
    const auto result = transfer<false>(io, buffer);
    REQUIRE(result.transferred == 0);
    REQUIRE(result.end == transfer_end::error);
    REQUIRE(result.error.value() == EAGAIN);
    REQUIRE(io.requests.size() == 1);
}

TEST_CASE("empty and overflowing file ranges never submit I/O",
          "[io][file_transfer]") {
    std::array<std::byte, 8> buffer{};
    recorded_io io{{}, {}};
    const auto max_offset = elio::io::detail::file_transfer_max_offset;
    SECTION("empty despite cancellation") {
        cancel_source source;
        source.cancel();
        const auto result = transfer<false>(io, {}, max_offset, true, source.get_token());
        REQUIRE(result.end == transfer_end::complete);
        REQUIRE_FALSE(result.error);
    }
    SECTION("offset outside native range") {
        const auto result = transfer<false>(io, buffer, max_offset + 1);
        REQUIRE(result.end == transfer_end::error);
        REQUIRE(result.error.value() == EOVERFLOW);
    }
    SECTION("end overflows native range") {
        const auto result = transfer<true>(io, buffer, max_offset - 7);
        REQUIRE(result.end == transfer_end::error);
        REQUIRE(result.error.value() == EOVERFLOW);
    }
    REQUIRE(io.requests.empty());
}

TEST_CASE("file transfer cancellation stops only subsequent operations",
          "[io][file_transfer][cancel]") {
    std::array<std::byte, 8> buffer{};
    cancel_source source;
    recorded_io io{{3, 5}, {}, &source, 0};
    SECTION("already cancelled") {
        source.cancel();
        const auto result = transfer<false>(io, buffer, 13, true, source.get_token());
        REQUIRE(result.transferred == 0);
        REQUIRE(result.end == transfer_end::cancelled);
        REQUIRE(result.error.value() == ECANCELED);
        REQUIRE(io.requests.empty());
    }
    SECTION("after progress") {
        const auto result = transfer<true>(io, buffer, 13, true, source.get_token());
        REQUIRE(result.transferred == 3);
        REQUIRE(result.end == transfer_end::cancelled);
        REQUIRE(result.error.value() == ECANCELED);
        REQUIRE(io.requests.size() == 1);
    }
    SECTION("final completion wins") {
        io.results = {8};
        const auto result = transfer<false>(io, buffer, 13, true, source.get_token());
        REQUIRE(result.transferred == 8);
        REQUIRE(result.end == transfer_end::complete);
        REQUIRE_FALSE(result.error);
        REQUIRE(io.requests.size() == 1);
    }
    SECTION("kernel error is not rewritten") {
        io.results = {-EIO};
        const auto result = transfer<true>(io, buffer, 13, true, source.get_token());
        REQUIRE(result.transferred == 0);
        REQUIRE(result.end == transfer_end::error);
        REQUIRE(result.error.value() == EIO);
        REQUIRE(io.requests.size() == 1);
    }
}

TEST_CASE("file transfer requests fit the signed backend result width",
          "[io][file_transfer][large]") {
    constexpr auto chunk = elio::io::detail::file_transfer_max_chunk;
    constexpr auto length = chunk + 17;
    void* storage = ::mmap(nullptr, length, PROT_NONE,
        MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    if (storage == MAP_FAILED) {
        SKIP("Address-space reservation unavailable for large-request test");
    }
    struct mapping_guard {
        void* storage;
        ~mapping_guard() { ::munmap(storage, length); }
    } guard{storage};
    auto buffer = std::span(static_cast<std::byte*>(storage), length);
    recorded_io io{{static_cast<int32_t>(chunk), 17}, {}};
    const auto result = transfer<true>(io, buffer, 0);
    REQUIRE(result.transferred == length);
    REQUIRE(result.end == transfer_end::complete);
    REQUIRE(io.requests.size() == 2);
    REQUIRE(io.requests[0].length == chunk);
    REQUIRE(io.requests[1].length == 17);
    REQUIRE(io.requests[1].offset == static_cast<int64_t>(chunk));
    REQUIRE(io.requests[1].buffer == buffer.data() + chunk);
}

TEST_CASE("malformed file completions cannot corrupt transfer cursors",
          "[io][file_transfer]") {
    std::array<std::byte, 8> buffer{};
    recorded_io io{{}, {}};
    SECTION("count larger than request") { io.results = {9}; }
    SECTION("unrepresentable errno") {
        io.results = {std::numeric_limits<int32_t>::min()};
    }
    const auto result = transfer<false>(io, buffer);
    REQUIRE(result.transferred == 0);
    REQUIRE(result.end == transfer_end::error);
    REQUIRE(result.error.value() == EIO);
    REQUIRE(io.requests.size() == 1);
}

TEST_CASE("file transfer cancellation waits for admitted borrowed-buffer access",
          "[io][file_transfer][cancel][lifetime]") {
    std::array<std::byte, 8> buffer{};
    cancel_source source;
    held_io io{};
    auto operation = elio::io::detail::transfer_file_range<false>(
        42, std::span<std::byte>(buffer), 0, true, source.get_token(), held_operation{io});
    const auto handle = elio::coro::detail::task_access::handle(operation);
    handle.resume();
    REQUIRE(io.calls == 1);
    REQUIRE_FALSE(handle.done());
    source.cancel();
    REQUIRE_FALSE(handle.done());
    REQUIRE(buffer[0] == std::byte{});

    int32_t completion = 3;
    SECTION("short progress stops before another operation") {}
    SECTION("final progress remains successful") { completion = 8; }
    SECTION("terminal kernel error remains visible") { completion = -EIO; }
    io.complete(completion);
    REQUIRE(handle.done());
    const auto result = operation.await_resume();
    REQUIRE(io.calls == 1);
    REQUIRE(result.transferred == static_cast<size_t>(std::max(completion, 0)));
    if (completion == 3) {
        REQUIRE(result.end == transfer_end::cancelled);
        REQUIRE(result.error.value() == ECANCELED);
    } else if (completion == 8) {
        REQUIRE(result.end == transfer_end::complete);
        REQUIRE_FALSE(result.error);
    } else {
        REQUIRE(result.end == transfer_end::error);
        REQUIRE(result.error.value() == EIO);
    }
    if (completion > 0) {
        REQUIRE(buffer[static_cast<size_t>(completion) - 1] == std::byte{0x5a});
    }
}

TEST_CASE("positional file helpers preserve real backend data and outcomes",
          "[io][file_transfer][file][backend]") {
    using backend_type = elio::io::io_context::backend_type;
    auto run = [](backend_type backend) {
        transfer_backend_guard backend_guard(backend);
        char path[] = "/tmp/elio_file_transfer_XXXXXX";
        elio::io::fd_guard fd(::mkstemp(path));
        REQUIRE(fd.get() >= 0);
        struct path_guard {
            const char* path;
            ~path_guard() { ::unlink(path); }
        } cleanup{path};
        elio::io::fd_guard readonly(::open(path, O_RDONLY));
        REQUIRE(readonly.get() >= 0);
        REQUIRE(::lseek(fd.get(), 9, SEEK_SET) == 9);

        std::array<std::byte, 64> payload{};
        for (size_t i = 0; i < payload.size(); ++i) {
            payload[i] = static_cast<std::byte>(i + 1);
        }
        const std::array<std::byte, 4> patch{
            std::byte{0xa1}, std::byte{0xa2}, std::byte{0xa3}, std::byte{0xa4}};
        std::array<std::byte, 64> readback{};
        std::array<std::byte, 128> short_read{};
        std::array<std::byte, 128> eof_read{};
        std::array<file_transfer_result, 8> results{};
        backend_type observed = backend_type::auto_detect;
        bool completed = false;
        elio::runtime::scheduler sched(1);
        sched.start();
        sched.go([&]() -> elio::coro::task<void> {
            observed = elio::io::current_io_context().get_backend_type();
            results[0] = co_await elio::io::pwrite_exactly(fd.get(), payload, 37);
            results[1] = co_await elio::io::pwrite_some(fd.get(), patch, 42);
            results[2] = co_await elio::io::pread_exactly(fd.get(), readback, 37);
            results[3] = co_await elio::io::pread_some(fd.get(), short_read, 95);
            results[4] = co_await elio::io::pread_exactly(fd.get(), eof_read, 37);
            results[5] = co_await elio::io::pread_some(fd.get(), short_read, 101);
            results[6] = co_await elio::io::pread_exactly(-1, readback, 0);
            results[7] = co_await elio::io::pwrite_exactly(readonly.get(), payload, 0);
            completed = true;
        });
        const bool drained = sched.shutdown(elio::test::scaled_ms(5000));
        REQUIRE(drained);
        REQUIRE(completed);
        REQUIRE(observed == backend);
        for (size_t i = 0; i < 4; ++i) {
            REQUIRE(results[i].end == transfer_end::complete);
            REQUIRE_FALSE(results[i].error);
        }
        REQUIRE(results[0].transferred == payload.size());
        REQUIRE(results[1].transferred == patch.size());
        REQUIRE(results[2].transferred == readback.size());
        REQUIRE(results[3].transferred == 6);
        auto expected = payload;
        std::copy(patch.begin(), patch.end(), expected.begin() + 5);
        REQUIRE(readback == expected);
        REQUIRE(std::equal(short_read.begin(), short_read.begin() + 6,
                           expected.end() - 6));
        REQUIRE(results[4].end == transfer_end::eof);
        REQUIRE(results[4].transferred == expected.size());
        REQUIRE_FALSE(results[4].error);
        REQUIRE(std::equal(expected.begin(), expected.end(), eof_read.begin()));
        REQUIRE(results[5].end == transfer_end::eof);
        REQUIRE(results[5].transferred == 0);
        REQUIRE_FALSE(results[5].error);
        for (size_t i = 6; i < 8; ++i) {
            REQUIRE(results[i].end == transfer_end::error);
            REQUIRE(results[i].transferred == 0);
            REQUIRE(results[i].error == std::error_code(EBADF, std::generic_category()));
        }
        REQUIRE(::lseek(fd.get(), 0, SEEK_CUR) == 9);
        struct stat metadata{};
        REQUIRE(::fstat(fd.get(), &metadata) == 0);
        REQUIRE(metadata.st_size == 101);
    };

    SECTION("forced epoll") { run(backend_type::epoll); }
#if ELIO_HAS_IO_URING
    SECTION("forced io_uring when available") {
        if (!elio::io::io_uring_backend::is_available()) SKIP("io_uring unavailable");
        run(backend_type::io_uring);
    }
#endif
}
