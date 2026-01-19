#include <catch2/catch_test_macros.hpp>
#include <elio/io/io_context.hpp>
#include <elio/io/io_awaitables.hpp>
#include <elio/coro/task.hpp>

#include <unistd.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <cstring>
#include <thread>
#include <atomic>

using namespace elio::io;
using namespace elio::coro;

TEST_CASE("io_context creation", "[io][context]") {
    SECTION("default constructor uses auto-detection") {
        io_context ctx;
        // Should use epoll since io_uring may not be available
        auto type = ctx.get_backend_type();
        REQUIRE((type == io_context::backend_type::epoll || 
                 type == io_context::backend_type::io_uring));
    }
    
    SECTION("explicit epoll backend") {
        io_context ctx(io_context::backend_type::epoll);
        REQUIRE(ctx.get_backend_type() == io_context::backend_type::epoll);
        REQUIRE(std::string(ctx.get_backend_name()) == "epoll");
    }
    
    SECTION("no pending operations initially") {
        io_context ctx;
        REQUIRE_FALSE(ctx.has_pending());
        REQUIRE(ctx.pending_count() == 0);
    }
}

TEST_CASE("io_result basic operations", "[io][result]") {
    SECTION("successful result") {
        io_result result{100, 0};
        REQUIRE(result.success());
        REQUIRE(result.bytes_transferred() == 100);
        REQUIRE(result.error_code() == 0);
    }
    
    SECTION("error result") {
        io_result result{-ENOENT, 0};
        REQUIRE_FALSE(result.success());
        REQUIRE(result.bytes_transferred() == 0);
        REQUIRE(result.error_code() == ENOENT);
    }
    
    SECTION("zero bytes transferred") {
        io_result result{0, 0};
        REQUIRE(result.success());
        REQUIRE(result.bytes_transferred() == 0);
    }
}

TEST_CASE("epoll_backend basic operations", "[io][epoll]") {
    epoll_backend backend;
    
    SECTION("initial state") {
        REQUIRE_FALSE(backend.has_pending());
        REQUIRE(backend.pending_count() == 0);
    }
    
    SECTION("is_available returns true") {
        REQUIRE(epoll_backend::is_available());
    }
}

TEST_CASE("Pipe read/write with epoll", "[io][epoll][pipe]") {
    int pipefd[2];
    REQUIRE(pipe(pipefd) == 0);
    
    // Make non-blocking
    fcntl(pipefd[0], F_SETFL, O_NONBLOCK);
    fcntl(pipefd[1], F_SETFL, O_NONBLOCK);
    
    io_context ctx(io_context::backend_type::epoll);
    
    // Write some data synchronously first
    const char* test_data = "Hello, Elio!";
    ssize_t written = write(pipefd[1], test_data, strlen(test_data));
    REQUIRE(written == static_cast<ssize_t>(strlen(test_data)));
    
    // Read using epoll backend
    char buffer[64] = {0};
    std::atomic<bool> completed{false};
    io_result read_result{};
    
    // Create a simple test coroutine
    auto read_coro = [&]() -> task<void> {
        auto result = co_await async_read(ctx, pipefd[0], buffer, sizeof(buffer) - 1);
        read_result = result;
        completed = true;
    };
    
    auto t = read_coro();
    auto handle = t.handle();
    
    // Start the coroutine
    handle.resume();
    
    // Poll for completion
    for (int i = 0; i < 100 && !completed; ++i) {
        ctx.poll(std::chrono::milliseconds(10));
    }
    
    REQUIRE(completed);
    REQUIRE(read_result.success());
    REQUIRE(read_result.bytes_transferred() == static_cast<int>(strlen(test_data)));
    REQUIRE(std::string(buffer) == test_data);
    
    close(pipefd[0]);
    close(pipefd[1]);
}

TEST_CASE("File operations with epoll", "[io][epoll][file]") {
    // Create a temp file
    char tmpfile[] = "/tmp/elio_test_XXXXXX";
    int fd = mkstemp(tmpfile);
    REQUIRE(fd >= 0);
    
    // Make non-blocking
    fcntl(fd, F_SETFL, O_NONBLOCK);
    
    io_context ctx(io_context::backend_type::epoll);
    
    SECTION("write to file") {
        const char* data = "Test data for Elio I/O";
        
        // Write synchronously (files are always "ready" for epoll)
        ssize_t written = write(fd, data, strlen(data));
        REQUIRE(written == static_cast<ssize_t>(strlen(data)));
        
        // Seek back to beginning
        lseek(fd, 0, SEEK_SET);
        
        // Read and verify
        char buffer[64] = {0};
        ssize_t readn = read(fd, buffer, sizeof(buffer) - 1);
        REQUIRE(readn == static_cast<ssize_t>(strlen(data)));
        REQUIRE(std::string(buffer) == data);
    }
    
    close(fd);
    unlink(tmpfile);
}

TEST_CASE("Socket pair with epoll", "[io][epoll][socket]") {
    int sv[2];
    REQUIRE(socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0, sv) == 0);
    
    io_context ctx(io_context::backend_type::epoll);
    
    const char* msg = "Socket test message";
    
    // Send on one end
    ssize_t sent = send(sv[0], msg, strlen(msg), 0);
    REQUIRE(sent == static_cast<ssize_t>(strlen(msg)));
    
    // Receive on the other end using async
    char buffer[64] = {0};
    std::atomic<bool> completed{false};
    io_result recv_result{};
    
    auto recv_coro = [&]() -> task<void> {
        auto result = co_await async_recv(ctx, sv[1], buffer, sizeof(buffer) - 1);
        recv_result = result;
        completed = true;
    };
    
    auto t = recv_coro();
    t.handle().resume();
    
    for (int i = 0; i < 100 && !completed; ++i) {
        ctx.poll(std::chrono::milliseconds(10));
    }
    
    REQUIRE(completed);
    REQUIRE(recv_result.success());
    REQUIRE(recv_result.bytes_transferred() == static_cast<int>(strlen(msg)));
    REQUIRE(std::string(buffer) == msg);
    
    close(sv[0]);
    close(sv[1]);
}

TEST_CASE("io_context run methods", "[io][context][run]") {
    io_context ctx;
    
    SECTION("run_until_complete with no pending") {
        // Should return immediately
        ctx.run_until_complete();
        REQUIRE_FALSE(ctx.has_pending());
    }
    
    SECTION("run_for with duration") {
        auto start = std::chrono::steady_clock::now();
        ctx.run_for(std::chrono::milliseconds(50));
        auto elapsed = std::chrono::steady_clock::now() - start;
        
        // Should have run for approximately 50ms (allow some tolerance)
        // But since no pending ops, should return quickly
        REQUIRE(elapsed < std::chrono::milliseconds(100));
    }
}

TEST_CASE("Cancel operation", "[io][epoll][cancel]") {
    int sv[2];
    REQUIRE(socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0, sv) == 0);
    
    io_context ctx(io_context::backend_type::epoll);
    
    // Start a read that won't complete (no data sent)
    char buffer[64];
    std::atomic<bool> completed{false};
    io_result recv_result{};
    void* cancel_key = nullptr;
    
    auto recv_coro = [&]() -> task<void> {
        auto result = co_await async_recv(ctx, sv[1], buffer, sizeof(buffer));
        recv_result = result;
        completed = true;
    };
    
    auto t = recv_coro();
    auto handle = t.handle();
    cancel_key = handle.address();
    
    // Start the coroutine
    handle.resume();
    
    // Poll once to register
    ctx.poll(std::chrono::milliseconds(1));
    
    // Cancel the operation
    bool cancelled = ctx.cancel(cancel_key);
    (void)cancelled;  // May or may not succeed depending on timing
    
    // Poll to process cancellation
    ctx.poll(std::chrono::milliseconds(10));
    
    // Note: cancel behavior depends on backend implementation
    // Just verify we don't crash
    
    close(sv[0]);
    close(sv[1]);
}

TEST_CASE("Multiple concurrent operations", "[io][epoll][concurrent]") {
    int sv1[2], sv2[2];
    REQUIRE(socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0, sv1) == 0);
    REQUIRE(socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0, sv2) == 0);
    
    io_context ctx(io_context::backend_type::epoll);
    
    const char* msg1 = "Message 1";
    const char* msg2 = "Message 2";
    
    // Send on both pairs
    send(sv1[0], msg1, strlen(msg1), 0);
    send(sv2[0], msg2, strlen(msg2), 0);
    
    char buffer1[64] = {0};
    char buffer2[64] = {0};
    std::atomic<int> completed{0};
    
    auto recv_coro1 = [&]() -> task<void> {
        co_await async_recv(ctx, sv1[1], buffer1, sizeof(buffer1) - 1);
        completed++;
    };
    
    auto recv_coro2 = [&]() -> task<void> {
        co_await async_recv(ctx, sv2[1], buffer2, sizeof(buffer2) - 1);
        completed++;
    };
    
    auto t1 = recv_coro1();
    auto t2 = recv_coro2();
    
    t1.handle().resume();
    t2.handle().resume();
    
    // Poll until both complete
    for (int i = 0; i < 100 && completed < 2; ++i) {
        ctx.poll(std::chrono::milliseconds(10));
    }
    
    REQUIRE(completed == 2);
    REQUIRE(std::string(buffer1) == msg1);
    REQUIRE(std::string(buffer2) == msg2);
    
    close(sv1[0]);
    close(sv1[1]);
    close(sv2[0]);
    close(sv2[1]);
}

TEST_CASE("Default io_context singleton", "[io][context][singleton]") {
    auto& ctx1 = default_io_context();
    auto& ctx2 = default_io_context();
    
    // Should be the same instance
    REQUIRE(&ctx1 == &ctx2);
}

TEST_CASE("epoll_backend registers fd before data available", "[io][epoll][registration]") {
    // This test verifies that async operations are properly registered with epoll
    // even when no data is immediately available. This catches use-after-move bugs
    // in the prepare() function.
    
    int sv[2];
    REQUIRE(socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0, sv) == 0);
    
    io_context ctx(io_context::backend_type::epoll);
    
    char buffer[64] = {0};
    std::atomic<bool> completed{false};
    io_result recv_result{};
    
    auto recv_coro = [&]() -> task<void> {
        auto result = co_await async_recv(ctx, sv[1], buffer, sizeof(buffer) - 1);
        recv_result = result;
        completed = true;
    };
    
    auto t = recv_coro();
    t.handle().resume();
    
    // Poll once to ensure the operation is registered
    ctx.poll(std::chrono::milliseconds(1));
    
    // Verify the operation is pending (fd should be registered with epoll)
    REQUIRE(ctx.has_pending());
    REQUIRE(ctx.pending_count() >= 1);
    
    // Now send data - this should trigger the read to complete
    const char* msg = "delayed message";
    ssize_t sent = send(sv[0], msg, strlen(msg), 0);
    REQUIRE(sent == static_cast<ssize_t>(strlen(msg)));
    
    // Poll until completion
    for (int i = 0; i < 100 && !completed; ++i) {
        ctx.poll(std::chrono::milliseconds(10));
    }
    
    // The read should have completed successfully
    REQUIRE(completed);
    REQUIRE(recv_result.success());
    REQUIRE(recv_result.bytes_transferred() == static_cast<int>(strlen(msg)));
    REQUIRE(std::string(buffer) == msg);
    
    close(sv[0]);
    close(sv[1]);
}

TEST_CASE("epoll_backend handles multiple pending ops on same fd", "[io][epoll][multi-op]") {
    // Test that multiple operations on the same fd are properly tracked
    
    int sv[2];
    REQUIRE(socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0, sv) == 0);
    
    io_context ctx(io_context::backend_type::epoll);
    
    char buffer1[32] = {0};
    char buffer2[32] = {0};
    std::atomic<int> completed{0};
    
    // Start two recv operations on the same fd
    auto recv_coro1 = [&]() -> task<void> {
        co_await async_recv(ctx, sv[1], buffer1, sizeof(buffer1) - 1);
        completed++;
    };
    
    auto recv_coro2 = [&]() -> task<void> {
        co_await async_recv(ctx, sv[1], buffer2, sizeof(buffer2) - 1);
        completed++;
    };
    
    auto t1 = recv_coro1();
    auto t2 = recv_coro2();
    
    t1.handle().resume();
    t2.handle().resume();
    
    // Register both
    ctx.poll(std::chrono::milliseconds(1));
    
    REQUIRE(ctx.pending_count() >= 2);
    
    // Send enough data for both reads
    const char* msg1 = "first";
    const char* msg2 = "second";
    send(sv[0], msg1, strlen(msg1), 0);
    
    // Poll to complete first read
    for (int i = 0; i < 100 && completed < 1; ++i) {
        ctx.poll(std::chrono::milliseconds(10));
    }
    
    send(sv[0], msg2, strlen(msg2), 0);
    
    // Poll to complete second read
    for (int i = 0; i < 100 && completed < 2; ++i) {
        ctx.poll(std::chrono::milliseconds(10));
    }
    
    REQUIRE(completed == 2);
    
    close(sv[0]);
    close(sv[1]);
}

TEST_CASE("epoll_backend write operation registration", "[io][epoll][write]") {
    // Verify write operations are properly registered
    
    int sv[2];
    REQUIRE(socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0, sv) == 0);
    
    io_context ctx(io_context::backend_type::epoll);
    
    const char* msg = "write test data";
    std::atomic<bool> completed{false};
    io_result send_result{};
    
    auto send_coro = [&]() -> task<void> {
        auto result = co_await async_send(ctx, sv[0], msg, strlen(msg));
        send_result = result;
        completed = true;
    };
    
    auto t = send_coro();
    t.handle().resume();
    
    // Poll for completion
    for (int i = 0; i < 100 && !completed; ++i) {
        ctx.poll(std::chrono::milliseconds(10));
    }
    
    REQUIRE(completed);
    REQUIRE(send_result.success());
    REQUIRE(send_result.bytes_transferred() == static_cast<int>(strlen(msg)));
    
    // Verify data was actually sent
    char buffer[64] = {0};
    ssize_t received = recv(sv[1], buffer, sizeof(buffer) - 1, 0);
    REQUIRE(received == static_cast<ssize_t>(strlen(msg)));
    REQUIRE(std::string(buffer) == msg);
    
    close(sv[0]);
    close(sv[1]);
}
