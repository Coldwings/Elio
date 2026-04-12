#include <catch2/catch_test_macros.hpp>
#include <elio/io/io_awaitables.hpp>
#include <elio/io/file_helpers.hpp>
#include <elio/coro/task.hpp>
#include <elio/runtime/scheduler.hpp>

#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <cstring>
#include <thread>
#include <atomic>
#include <optional>

using namespace elio::io;
using namespace elio::coro;
using namespace elio::runtime;

// Helper: run a coroutine on a scheduler and wait for completion
template<typename F>
static void run_on_scheduler(F&& coro_factory, int workers = 1) {
    scheduler sched(workers);
    sched.start();
    std::atomic<bool> done{false};
    sched.go([&]() -> task<void> {
        co_await coro_factory();
        done = true;
    });
    for (int i = 0; i < 200 && !done; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    sched.shutdown();
    REQUIRE(done);
}

// ============================================================================
// Batch I/O Tests
// ============================================================================

TEST_CASE("batch_read: read multiple segments from file", "[io][batch][read]") {
    // Create a temp file with known content
    char tmpfile[] = "/tmp/elio_batch_read_XXXXXX";
    int fd = mkstemp(tmpfile);
    REQUIRE(fd >= 0);

    const char* content = "0123456789ABCDEFGHIJ";
    ssize_t written = write(fd, content, strlen(content));
    REQUIRE(written == static_cast<ssize_t>(strlen(content)));

    run_on_scheduler([&]() -> task<void> {
        char buf1[16] = {0};
        char buf2[16] = {0};
        char buf3[16] = {0};

        std::array<batch_read_segment, 3> segments;
        segments[0] = batch_read_segment{0, buf1, 5};     // "01234"
        segments[1] = batch_read_segment{5, buf2, 5};     // "56789"
        segments[2] = batch_read_segment{15, buf3, 5};    // "FGHIJ"

        auto results = co_await batch_read(fd, std::span<const batch_read_segment>(segments));

        REQUIRE(results.size() == 3);
        REQUIRE(results[0] == 5);
        REQUIRE(results[1] == 5);
        REQUIRE(results[2] == 5);

        REQUIRE(std::string(buf1, 5) == "01234");
        REQUIRE(std::string(buf2, 5) == "56789");
        REQUIRE(std::string(buf3, 5) == "FGHIJ");
    });

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

    run_on_scheduler([&]() -> task<void> {
        const char* seg1 = "Hello";
        const char* seg2 = "World";
        const char* seg3 = "Test";

        std::array<batch_write_segment, 3> segments;
        segments[0] = batch_write_segment{0, seg1, 5};    // offset 0
        segments[1] = batch_write_segment{10, seg2, 5};   // offset 10
        segments[2] = batch_write_segment{20, seg3, 4};   // offset 20

        auto results = co_await batch_write(fd, std::span<const batch_write_segment>(segments));

        REQUIRE(results.size() == 3);
        REQUIRE(results[0] == 5);
        REQUIRE(results[1] == 5);
        REQUIRE(results[2] == 4);
    });

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

TEST_CASE("batch_read: empty segments returns empty result", "[io][batch][read]") {
    char tmpfile[] = "/tmp/elio_batch_empty_XXXXXX";
    int fd = mkstemp(tmpfile);
    REQUIRE(fd >= 0);

    run_on_scheduler([&]() -> task<void> {
        auto results = co_await batch_read(fd, std::span<const batch_read_segment>{});
        REQUIRE(results.empty());
    });

    close(fd);
    unlink(tmpfile);
}

TEST_CASE("batch_read: single segment works", "[io][batch][read]") {
    char tmpfile[] = "/tmp/elio_batch_single_XXXXXX";
    int fd = mkstemp(tmpfile);
    REQUIRE(fd >= 0);

    const char* content = "SingleSegmentTest";
    ssize_t written = write(fd, content, strlen(content));
    REQUIRE(written == static_cast<ssize_t>(strlen(content)));

    run_on_scheduler([&]() -> task<void> {
        char buf[32] = {0};

        batch_read_segment seg{0, buf, 13};  // "SingleSegment"

        auto results = co_await batch_read(fd, std::span<const batch_read_segment>(&seg, 1));

        REQUIRE(results.size() == 1);
        REQUIRE(results[0] == 13);
        REQUIRE(std::string(buf, 13) == "SingleSegment");
    });

    close(fd);
    unlink(tmpfile);
}

// ============================================================================
// File Helpers Tests
// ============================================================================

TEST_CASE("file_helpers: write_file and read_file roundtrip", "[io][file_helpers]") {
    std::string path = "/tmp/elio_file_helpers_" + std::to_string(getpid()) + ".txt";

    // Clean up any leftover
    unlink(path.c_str());

    run_on_scheduler([&]() -> task<void> {
        auto written = co_await write_file(path, "Hello, Elio!");
        REQUIRE(written);

        auto content = co_await read_file(path);
        REQUIRE(content.has_value());
        REQUIRE(content.value() == "Hello, Elio!");
    });

    unlink(path.c_str());
}

TEST_CASE("file_helpers: append_file", "[io][file_helpers]") {
    std::string path = "/tmp/elio_append_" + std::to_string(getpid()) + ".txt";
    unlink(path.c_str());

    run_on_scheduler([&]() -> task<void> {
        // First write
        auto w1 = co_await write_file(path, "Hello");
        REQUIRE(w1);

        // Then append
        auto appended = co_await append_file(path, ", World!");
        REQUIRE(appended);

        // Read back and verify
        auto content = co_await read_file(path);
        REQUIRE(content.has_value());
        REQUIRE(content.value() == "Hello, World!");
    });

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

    run_on_scheduler([&]() -> task<void> {
        auto content = co_await read_file(path);
        REQUIRE_FALSE(content.has_value());
    });
}

TEST_CASE("file_helpers: write_file to non-writable path returns false", "[io][file_helpers]") {
    run_on_scheduler([&]() -> task<void> {
        auto result = co_await write_file("/nonexistent/dir/file.txt", "data");
        REQUIRE_FALSE(result);
    });
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
