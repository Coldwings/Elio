#include <elio/elio.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdlib>
#include <iostream>
#include <span>
#include <unistd.h>

elio::coro::task<int> main_task() {
    char path[] = "/tmp/elio_positional_example_XXXXXX";
    elio::io::fd_guard fd(::mkstemp(path));
    if (fd.get() < 0) co_return 1;
    ::unlink(path);

    const std::array<char, 5> payload{'E', 'l', 'i', 'o', '!'};
    std::array<char, 8> output{};
    // Both spans and the descriptor remain alive across all awaited operations.
    const auto written = co_await elio::io::pwrite_exactly(
        fd.get(), std::as_bytes(std::span(payload)), 16);
    const auto read = co_await elio::io::pread_exactly(
        fd.get(), std::as_writable_bytes(std::span(output)), 16);
    const bool expected = written.end == elio::io::transfer_end::complete &&
        written.transferred == payload.size() &&
        read.end == elio::io::transfer_end::eof &&
        read.transferred == payload.size() && !read.error &&
        std::equal(payload.begin(), payload.end(), output.begin());
    std::cout << "Written: " << written.transferred
              << ", read before EOF: " << read.transferred << '\n';
    co_return expected ? 0 : 1;
}

int main() {
    return elio::run(main_task);
}
