#include <catch2/catch_test_macros.hpp>
#include <elio/io/file_transfer.hpp>
#include <elio/runtime/scheduler.hpp>

#include <array>
#include <cstdint>
#include <limits>
#include <vector>
#include <sys/mman.h>

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
