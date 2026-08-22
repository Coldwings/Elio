#include <catch2/catch_test_macros.hpp>
#include <elio/io/io_awaitables.hpp>
#include <elio/io/file_helpers.hpp>
#include <elio/coro/task.hpp>
#include <elio/runtime/scheduler.hpp>

#include <array>
#include <chrono>
#include <exception>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <cstring>
#include <thread>
#include <atomic>
#include <cerrno>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "../test_main.cpp"

using namespace elio::io;
using namespace elio::coro;
using namespace elio::runtime;

template<typename T>
struct scheduler_completion {
    bool drained = false;
    bool ready = false;
    bool destroyed = false;
    std::optional<T> value;
    std::exception_ptr exception;
};

// Helper: run a coroutine on a scheduler and collect completion on the test
// thread so worker coroutines do not call Catch2 runtime macros.
template<typename T, typename F>
static scheduler_completion<T> run_on_scheduler(F&& coro_factory,
                                                int workers = 1) {
    scheduler sched(workers);
    sched.start();
    auto handle = sched.go_joinable(std::forward<F>(coro_factory));

    scheduler_completion<T> result;
    result.drained = sched.shutdown(elio::test::scaled_sec(5));
    result.ready = handle.is_ready();
    result.destroyed = handle.is_destroyed();
    if (!result.ready || !result.destroyed) return result;

    try {
        result.value.emplace(handle.await_resume());
    } catch (...) {
        result.exception = std::current_exception();
    }
    return result;
}

static void rethrow_scheduler_exception(const std::exception_ptr& exception) {
    if (exception) std::rethrow_exception(exception);
}

template<typename T>
static const T& require_scheduler_value(
    const scheduler_completion<T>& completion) {
    REQUIRE(completion.drained);
    REQUIRE(completion.ready);
    REQUIRE(completion.destroyed);
    REQUIRE_NOTHROW(rethrow_scheduler_exception(completion.exception));
    REQUIRE(completion.value.has_value());
    return *completion.value;
}

// ============================================================================
// Batch I/O Tests
// ============================================================================

TEST_CASE("batch_read: read multiple segments from file", "[io][batch][read]") {
    struct observation {
        std::vector<int> results;
        std::string first;
        std::string second;
        std::string third;
    };

    // Create a temp file with known content
    char tmpfile[] = "/tmp/elio_batch_read_XXXXXX";
    int fd = mkstemp(tmpfile);
    REQUIRE(fd >= 0);

    const char* content = "0123456789ABCDEFGHIJ";
    ssize_t written = write(fd, content, strlen(content));
    REQUIRE(written == static_cast<ssize_t>(strlen(content)));

    const auto completion = run_on_scheduler<observation>([&]() -> task<observation> {
        char buf1[16] = {0};
        char buf2[16] = {0};
        char buf3[16] = {0};

        std::array<batch_read_segment, 3> segments;
        segments[0] = batch_read_segment{0, buf1, 5};     // "01234"
        segments[1] = batch_read_segment{5, buf2, 5};     // "56789"
        segments[2] = batch_read_segment{15, buf3, 5};    // "FGHIJ"

        auto results = co_await batch_read(fd, std::span<const batch_read_segment>(segments));

        co_return observation{
            results,
            std::string(buf1, 5),
            std::string(buf2, 5),
            std::string(buf3, 5),
        };
    });

    const auto& observed = require_scheduler_value(completion);
    REQUIRE(observed.results.size() == 3);
    REQUIRE(observed.results[0] == 5);
    REQUIRE(observed.results[1] == 5);
    REQUIRE(observed.results[2] == 5);
    REQUIRE(observed.first == "01234");
    REQUIRE(observed.second == "56789");
    REQUIRE(observed.third == "FGHIJ");

    close(fd);
    unlink(tmpfile);
}

TEST_CASE("batch_read: negative offset uses current file position",
          "[io][batch][read][regression]") {
    struct observation {
        std::vector<int> results;
        std::string explicit_read;
        std::string current_first;
        std::string current_second;
        off_t position = -1;
    };

    char tmpfile[] = "/tmp/elio_batch_read_current_XXXXXX";
    int fd = mkstemp(tmpfile);
    REQUIRE(fd >= 0);

    const char* content = "0123456789ABCDEFGHIJ";
    ssize_t written = write(fd, content, strlen(content));
    REQUIRE(written == static_cast<ssize_t>(strlen(content)));
    REQUIRE(lseek(fd, 10, SEEK_SET) == 10);

    const auto completion = run_on_scheduler<observation>([&]() -> task<observation> {
        char explicit_buf[8] = {0};
        char current_buf1[8] = {0};
        char current_buf2[8] = {0};

        std::array<batch_read_segment, 3> segments;
        segments[0] = batch_read_segment{0, explicit_buf, 2};
        segments[1] = batch_read_segment{-1, current_buf1, 3};
        segments[2] = batch_read_segment{-1, current_buf2, 3};

        auto results = co_await batch_read(fd, std::span<const batch_read_segment>(segments));

        co_return observation{
            results,
            std::string(explicit_buf, 2),
            std::string(current_buf1, 3),
            std::string(current_buf2, 3),
            lseek(fd, 0, SEEK_CUR),
        };
    });

    const auto& observed = require_scheduler_value(completion);
    const std::vector<int> expected{2, 3, 3};
    REQUIRE(observed.results == expected);
    REQUIRE(observed.explicit_read == "01");
    REQUIRE(observed.current_first == "ABC");
    REQUIRE(observed.current_second == "DEF");
    REQUIRE(observed.position == 16);

    close(fd);
    unlink(tmpfile);
}

TEST_CASE("batch_write: write multiple segments to file", "[io][batch][write]") {
    // Create a temp file (pre-sized to hold all segments)
    char tmpfile[] = "/tmp/elio_batch_write_XXXXXX";
    int fd = mkstemp(tmpfile);
    REQUIRE(fd >= 0);

    // Pre-allocate space with zeros
    char zeroes[32] = {0};
    REQUIRE(write(fd, zeroes, sizeof(zeroes)) == static_cast<ssize_t>(sizeof(zeroes)));

    const auto completion = run_on_scheduler<std::vector<int>>([&]() -> task<std::vector<int>> {
        const char* seg1 = "Hello";
        const char* seg2 = "World";
        const char* seg3 = "Test";

        std::array<batch_write_segment, 3> segments;
        segments[0] = batch_write_segment{0, seg1, 5};    // offset 0
        segments[1] = batch_write_segment{10, seg2, 5};   // offset 10
        segments[2] = batch_write_segment{20, seg3, 4};   // offset 20

        auto results = co_await batch_write(fd, std::span<const batch_write_segment>(segments));

        co_return results;
    });

    const auto& results = require_scheduler_value(completion);
    REQUIRE(results.size() == 3);
    REQUIRE(results[0] == 5);
    REQUIRE(results[1] == 5);
    REQUIRE(results[2] == 4);

    // Read back and verify content
    char buffer[32] = {0};
    lseek(fd, 0, SEEK_SET);
    ssize_t readn = read(fd, buffer, sizeof(buffer) - 1);
    REQUIRE(readn > 0);

    REQUIRE(std::string(buffer, 5) == "Hello");
    REQUIRE(std::string(buffer + 10, 5) == "World");
    REQUIRE(std::string(buffer + 20, 4) == "Test");

    close(fd);
    unlink(tmpfile);
}

TEST_CASE("batch_write: negative offset uses current file position",
          "[io][batch][write][regression]") {
    struct observation {
        std::vector<int> results;
        off_t position = -1;
    };

    char tmpfile[] = "/tmp/elio_batch_write_current_XXXXXX";
    int fd = mkstemp(tmpfile);
    REQUIRE(fd >= 0);

    const char* content = "0123456789";
    REQUIRE(write(fd, content, strlen(content)) == static_cast<ssize_t>(strlen(content)));
    REQUIRE(lseek(fd, 5, SEEK_SET) == 5);

    const auto completion = run_on_scheduler<observation>([&]() -> task<observation> {
        const char* replacement = "XYZ";
        batch_write_segment segment{-1, replacement, 3};

        auto results = co_await batch_write(
            fd, std::span<const batch_write_segment>(&segment, 1));

        co_return observation{results, lseek(fd, 0, SEEK_CUR)};
    });

    const auto& observed = require_scheduler_value(completion);
    REQUIRE(observed.results == std::vector<int>{3});
    REQUIRE(observed.position == 8);

    char buffer[16] = {0};
    REQUIRE(lseek(fd, 0, SEEK_SET) == 0);
    REQUIRE(read(fd, buffer, 10) == 10);
    REQUIRE(std::string(buffer, 10) == "01234XYZ89");

    close(fd);
    unlink(tmpfile);
}

TEST_CASE("batch_read: empty segments returns empty result", "[io][batch][read]") {
    char tmpfile[] = "/tmp/elio_batch_empty_XXXXXX";
    int fd = mkstemp(tmpfile);
    REQUIRE(fd >= 0);

    const auto completion = run_on_scheduler<bool>([&]() -> task<bool> {
        auto results = co_await batch_read(fd, std::span<const batch_read_segment>{});
        co_return results.empty();
    });

    REQUIRE(require_scheduler_value(completion));

    close(fd);
    unlink(tmpfile);
}

TEST_CASE("batch_read: single segment works", "[io][batch][read]") {
    struct observation {
        std::vector<int> results;
        std::string content;
    };

    char tmpfile[] = "/tmp/elio_batch_single_XXXXXX";
    int fd = mkstemp(tmpfile);
    REQUIRE(fd >= 0);

    const char* content = "SingleSegmentTest";
    ssize_t written = write(fd, content, strlen(content));
    REQUIRE(written == static_cast<ssize_t>(strlen(content)));

    const auto completion = run_on_scheduler<observation>([&]() -> task<observation> {
        char buf[32] = {0};

        batch_read_segment seg{0, buf, 13};  // "SingleSegment"

        auto results = co_await batch_read(fd, std::span<const batch_read_segment>(&seg, 1));

        co_return observation{results, std::string(buf, 13)};
    });

    const auto& observed = require_scheduler_value(completion);
    REQUIRE(observed.results.size() == 1);
    REQUIRE(observed.results[0] == 13);
    REQUIRE(observed.content == "SingleSegment");

    close(fd);
    unlink(tmpfile);
}

TEST_CASE("batch I/O reports negative errno for failed segments",
          "[io][batch][error][regression]") {
    struct observation {
        std::vector<int> read_results;
        std::vector<int> write_results;
    };

    const auto completion = run_on_scheduler<observation>([]() -> task<observation> {
        char read_buffer{};
        batch_read_segment read_segment{0, &read_buffer, 1};
        auto read_results = co_await batch_read(
            -1, std::span<const batch_read_segment>(&read_segment, 1));

        const char write_buffer = 'x';
        batch_write_segment write_segment{0, &write_buffer, 1};
        auto write_results = co_await batch_write(
            -1, std::span<const batch_write_segment>(&write_segment, 1));

        co_return observation{read_results, write_results};
    });

    const auto& observed = require_scheduler_value(completion);
    REQUIRE(observed.read_results == std::vector<int>{-EBADF});
    REQUIRE(observed.write_results == std::vector<int>{-EBADF});
}

// ============================================================================
// File Helpers Tests
// ============================================================================

TEST_CASE("file_helpers: write_file and read_file roundtrip", "[io][file_helpers]") {
    struct observation {
        bool written = false;
        std::optional<std::string> content;
    };

    std::string path = "/tmp/elio_file_helpers_" + std::to_string(getpid()) + ".txt";

    // Clean up any leftover
    unlink(path.c_str());

    const auto completion = run_on_scheduler<observation>([&]() -> task<observation> {
        auto written = co_await write_file(path, "Hello, Elio!");
        auto content = co_await read_file(path);
        co_return observation{written, content};
    });

    const auto& observed = require_scheduler_value(completion);
    REQUIRE(observed.written);
    REQUIRE(observed.content.has_value());
    REQUIRE(observed.content.value() == "Hello, Elio!");

    unlink(path.c_str());
}

TEST_CASE("file_helpers: read_file handles empty files", "[io][file_helpers]") {
    std::string path = "/tmp/elio_file_helpers_empty_" + std::to_string(getpid()) + ".txt";
    unlink(path.c_str());

    int fd = open(path.c_str(), O_CREAT | O_WRONLY | O_TRUNC, 0644);
    REQUIRE(fd >= 0);
    close(fd);

    const auto completion = run_on_scheduler<std::optional<std::string>>(
        [&]() -> task<std::optional<std::string>> {
        auto content = co_await read_file(path);
        co_return content;
    });

    const auto& content = require_scheduler_value(completion);
    REQUIRE(content.has_value());
    REQUIRE(content->empty());

    unlink(path.c_str());
}

TEST_CASE("file_helpers: read_file handles multi-chunk reads", "[io][file_helpers]") {
    struct observation {
        bool written = false;
        bool has_content = false;
        bool matches_expected = false;
    };

    std::string path = "/tmp/elio_file_helpers_large_" + std::to_string(getpid()) + ".txt";
    unlink(path.c_str());

    std::string expected((1024 * 1024 * 2) + 123, 'x');
    expected.back() = 'z';

    const auto completion = run_on_scheduler<observation>([&]() -> task<observation> {
        auto written = co_await write_file(path, expected);
        auto content = co_await read_file(path);
        co_return observation{
            written,
            content.has_value(),
            content.has_value() && *content == expected,
        };
    });

    const auto& observed = require_scheduler_value(completion);
    REQUIRE(observed.written);
    REQUIRE(observed.has_content);
    REQUIRE(observed.matches_expected);

    unlink(path.c_str());
}

TEST_CASE("file_helpers: append_file", "[io][file_helpers]") {
    struct observation {
        bool first_written = false;
        bool appended = false;
        std::optional<std::string> content;
    };

    std::string path = "/tmp/elio_append_" + std::to_string(getpid()) + ".txt";
    unlink(path.c_str());

    const auto completion = run_on_scheduler<observation>([&]() -> task<observation> {
        // First write
        auto w1 = co_await write_file(path, "Hello");

        // Then append
        auto appended = co_await append_file(path, ", World!");

        // Read back and verify
        auto content = co_await read_file(path);
        co_return observation{w1, appended, content};
    });

    const auto& observed = require_scheduler_value(completion);
    REQUIRE(observed.first_written);
    REQUIRE(observed.appended);
    REQUIRE(observed.content.has_value());
    REQUIRE(observed.content.value() == "Hello, World!");

    unlink(path.c_str());
}

TEST_CASE("file_helpers: file_exists", "[io][file_helpers]") {
    std::string path = "/tmp/elio_exists_" + std::to_string(getpid()) + ".txt";
    std::string nonexist = "/tmp/elio_nonexist_" + std::to_string(getpid()) + ".txt";

    // Create the file
    int fd = open(path.c_str(), O_CREAT | O_WRONLY, 0644);
    REQUIRE(fd >= 0);
    close(fd);

    // Ensure nonexist does not exist
    unlink(nonexist.c_str());

    REQUIRE(file_exists(path));
    REQUIRE_FALSE(file_exists(nonexist));

    unlink(path.c_str());
}

TEST_CASE("file_helpers: file_size", "[io][file_helpers]") {
    std::string path = "/tmp/elio_fsize_" + std::to_string(getpid()) + ".txt";
    std::string nonexist = "/tmp/elio_fsize_nonexist_" + std::to_string(getpid()) + ".txt";

    // Create file with known content
    int fd = open(path.c_str(), O_CREAT | O_WRONLY | O_TRUNC, 0644);
    REQUIRE(fd >= 0);
    const char* data = "1234567890";
    REQUIRE(write(fd, data, 10) == 10);
    close(fd);

    unlink(nonexist.c_str());

    auto size = file_size(path);
    REQUIRE(size.has_value());
    REQUIRE(size.value() == 10);

    auto missing = file_size(nonexist);
    REQUIRE_FALSE(missing.has_value());

    unlink(path.c_str());
}

TEST_CASE("file_helpers: read_file non-existent returns nullopt", "[io][file_helpers]") {
    std::string path = "/tmp/elio_nofile_" + std::to_string(getpid()) + ".txt";
    unlink(path.c_str());

    const auto completion = run_on_scheduler<bool>([&]() -> task<bool> {
        auto content = co_await read_file(path);
        co_return content.has_value();
    });

    REQUIRE_FALSE(require_scheduler_value(completion));
}

TEST_CASE("file_helpers: write_file to non-writable path returns false", "[io][file_helpers]") {
    const auto completion = run_on_scheduler<bool>([&]() -> task<bool> {
        auto result = co_await write_file("/nonexistent/dir/file.txt", "data");
        co_return result;
    });

    REQUIRE_FALSE(require_scheduler_value(completion));
}

TEST_CASE("file_helpers: read_dir", "[io][file_helpers]") {
    std::string dirpath = "/tmp/elio_readdir_" + std::to_string(getpid());

    // Create temp directory
    mkdir(dirpath.c_str(), 0755);

    // Create some files inside
    int fd1 = open((dirpath + "/file1.txt").c_str(), O_CREAT | O_WRONLY, 0644);
    REQUIRE(fd1 >= 0);
    close(fd1);

    int fd2 = open((dirpath + "/file2.txt").c_str(), O_CREAT | O_WRONLY, 0644);
    REQUIRE(fd2 >= 0);
    close(fd2);

    // Read directory
    auto entries = read_dir(dirpath);
    REQUIRE(entries.has_value());
    REQUIRE(entries->size() >= 2);

    // Verify . and .. are excluded
    for (const auto& entry : *entries) {
        REQUIRE(entry.name != ".");
        REQUIRE(entry.name != "..");
    }

    // Verify our files are present
    bool found1 = false, found2 = false;
    for (const auto& entry : *entries) {
        if (entry.name == "file1.txt") found1 = true;
        if (entry.name == "file2.txt") found2 = true;
    }
    REQUIRE(found1);
    REQUIRE(found2);

    // Clean up
    unlink((dirpath + "/file1.txt").c_str());
    unlink((dirpath + "/file2.txt").c_str());
    rmdir(dirpath.c_str());
}

TEST_CASE("file_helpers: read_dir on non-existent path returns nullopt", "[io][file_helpers]") {
    std::string path = "/tmp/elio_nodir_" + std::to_string(getpid());
    rmdir(path.c_str());  // ensure it doesn't exist

    auto entries = read_dir(path);
    REQUIRE_FALSE(entries.has_value());
}
