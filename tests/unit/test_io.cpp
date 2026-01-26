#include <catch2/catch_test_macros.hpp>
#include <elio/io/io_context.hpp>
#include <elio/io/io_awaitables.hpp>
#include <elio/coro/task.hpp>

#include <unistd.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <cstring>
#include <thread>
#include <atomic>
#include <array>

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

// ============================================================================
// Unix Domain Socket (UDS) Tests
// ============================================================================

#include <elio/net/uds.hpp>

using namespace elio::net;

TEST_CASE("unix_address basic operations", "[uds][address]") {
    SECTION("default constructor") {
        unix_address addr;
        REQUIRE(addr.path.empty());
        REQUIRE(addr.to_string() == "(unnamed)");
    }
    
    SECTION("filesystem path") {
        unix_address addr("/tmp/test.sock");
        REQUIRE(addr.path == "/tmp/test.sock");
        REQUIRE(addr.to_string() == "/tmp/test.sock");
        REQUIRE_FALSE(addr.is_abstract());
        
        auto sa = addr.to_sockaddr();
        REQUIRE(sa.sun_family == AF_UNIX);
        REQUIRE(std::string(sa.sun_path) == "/tmp/test.sock");
    }
    
    SECTION("abstract socket") {
        auto addr = unix_address::abstract("test_socket");
        REQUIRE(addr.is_abstract());
        REQUIRE(addr.to_string() == "@test_socket");
        
        auto sa = addr.to_sockaddr();
        REQUIRE(sa.sun_family == AF_UNIX);
        REQUIRE(sa.sun_path[0] == '\0');
    }
    
    SECTION("sockaddr_len calculation") {
        unix_address empty;
        REQUIRE(empty.sockaddr_len() == sizeof(sa_family_t));
        
        unix_address fs("/tmp/x.sock");
        // offsetof(sockaddr_un, sun_path) + path.size() + 1 (null terminator)
        REQUIRE(fs.sockaddr_len() == static_cast<socklen_t>(
            offsetof(struct sockaddr_un, sun_path) + fs.path.size() + 1));
        
        auto abstract = unix_address::abstract("test");
        // offsetof(sockaddr_un, sun_path) + path.size() (includes leading null)
        REQUIRE(abstract.sockaddr_len() == static_cast<socklen_t>(
            offsetof(struct sockaddr_un, sun_path) + abstract.path.size()));
    }
}

TEST_CASE("UDS listener bind and accept", "[uds][listener]") {
    io_context ctx(io_context::backend_type::epoll);
    
    // Use abstract socket to avoid filesystem cleanup issues
    auto addr = unix_address::abstract("elio_test_listener_" + std::to_string(getpid()));
    
    SECTION("bind creates listener") {
        auto listener = uds_listener::bind(addr, ctx);
        REQUIRE(listener.has_value());
        REQUIRE(listener->is_valid());
        REQUIRE(listener->fd() >= 0);
        REQUIRE(listener->local_address().to_string() == addr.to_string());
    }
    
    SECTION("accept returns connection") {
        auto listener = uds_listener::bind(addr, ctx);
        REQUIRE(listener.has_value());
        
        // Create a client connection in a separate thread
        std::atomic<bool> client_connected{false};
        std::thread client_thread([&]() {
            // Wait a bit for the accept to be registered
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            
            int client_fd = socket(AF_UNIX, SOCK_STREAM, 0);
            REQUIRE(client_fd >= 0);
            
            auto sa = addr.to_sockaddr();
            int ret = connect(client_fd, reinterpret_cast<struct sockaddr*>(&sa), 
                             addr.sockaddr_len());
            REQUIRE(ret == 0);
            client_connected = true;
            
            // Keep connection open briefly
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            close(client_fd);
        });
        
        std::atomic<bool> accepted{false};
        std::optional<uds_stream> accepted_stream;
        
        auto accept_coro = [&]() -> task<void> {
            auto stream = co_await listener->accept();
            accepted_stream = std::move(stream);
            accepted = true;
        };
        
        auto t = accept_coro();
        t.handle().resume();
        
        // Poll for completion
        for (int i = 0; i < 200 && !accepted; ++i) {
            ctx.poll(std::chrono::milliseconds(10));
        }
        
        REQUIRE(accepted);
        REQUIRE(accepted_stream.has_value());
        REQUIRE(accepted_stream->is_valid());
        
        client_thread.join();
    }
}

TEST_CASE("UDS connect", "[uds][connect]") {
    io_context ctx(io_context::backend_type::epoll);
    
    auto addr = unix_address::abstract("elio_test_connect_" + std::to_string(getpid()));
    
    // Create server listener
    auto listener = uds_listener::bind(addr, ctx);
    REQUIRE(listener.has_value());
    
    // Start accept in background
    std::atomic<bool> server_accepted{false};
    std::optional<uds_stream> server_stream;
    
    auto accept_coro = [&]() -> task<void> {
        auto stream = co_await listener->accept();
        server_stream = std::move(stream);
        server_accepted = true;
    };
    
    auto accept_task = accept_coro();
    accept_task.handle().resume();
    
    // Connect client
    std::atomic<bool> client_connected{false};
    std::optional<uds_stream> client_stream;
    
    auto connect_coro = [&]() -> task<void> {
        auto stream = co_await uds_connect(ctx, addr);
        client_stream = std::move(stream);
        client_connected = true;
    };
    
    auto connect_task = connect_coro();
    connect_task.handle().resume();
    
    // Poll until both complete
    for (int i = 0; i < 200 && (!server_accepted || !client_connected); ++i) {
        ctx.poll(std::chrono::milliseconds(10));
    }
    
    REQUIRE(server_accepted);
    REQUIRE(client_connected);
    REQUIRE(server_stream.has_value());
    REQUIRE(client_stream.has_value());
    REQUIRE(server_stream->is_valid());
    REQUIRE(client_stream->is_valid());
}

TEST_CASE("UDS stream read/write", "[uds][stream]") {
    io_context ctx(io_context::backend_type::epoll);
    
    auto addr = unix_address::abstract("elio_test_rw_" + std::to_string(getpid()));
    
    // Create server and client
    auto listener = uds_listener::bind(addr, ctx);
    REQUIRE(listener.has_value());
    
    std::optional<uds_stream> server_stream;
    std::optional<uds_stream> client_stream;
    std::atomic<int> setup_complete{0};
    
    auto accept_coro = [&]() -> task<void> {
        auto stream = co_await listener->accept();
        server_stream = std::move(stream);
        setup_complete++;
    };
    
    auto connect_coro = [&]() -> task<void> {
        auto stream = co_await uds_connect(ctx, addr);
        client_stream = std::move(stream);
        setup_complete++;
    };
    
    auto accept_task = accept_coro();
    auto connect_task = connect_coro();
    accept_task.handle().resume();
    connect_task.handle().resume();
    
    for (int i = 0; i < 200 && setup_complete < 2; ++i) {
        ctx.poll(std::chrono::milliseconds(10));
    }
    
    REQUIRE(setup_complete == 2);
    REQUIRE(server_stream.has_value());
    REQUIRE(client_stream.has_value());
    
    SECTION("client to server") {
        const char* msg = "Hello from client!";
        char buffer[64] = {0};
        std::atomic<bool> write_done{false};
        std::atomic<bool> read_done{false};
        io_result write_result{};
        io_result read_result{};
        
        auto write_coro = [&]() -> task<void> {
            write_result = co_await client_stream->write(msg, strlen(msg));
            write_done = true;
        };
        
        auto read_coro = [&]() -> task<void> {
            read_result = co_await server_stream->read(buffer, sizeof(buffer) - 1);
            read_done = true;
        };
        
        auto write_task = write_coro();
        auto read_task = read_coro();
        write_task.handle().resume();
        read_task.handle().resume();
        
        for (int i = 0; i < 200 && (!write_done || !read_done); ++i) {
            ctx.poll(std::chrono::milliseconds(10));
        }
        
        REQUIRE(write_done);
        REQUIRE(read_done);
        REQUIRE(write_result.success());
        REQUIRE(read_result.success());
        REQUIRE(write_result.bytes_transferred() == static_cast<int>(strlen(msg)));
        REQUIRE(read_result.bytes_transferred() == static_cast<int>(strlen(msg)));
        REQUIRE(std::string(buffer) == msg);
    }
    
    SECTION("server to client") {
        const char* msg = "Hello from server!";
        char buffer[64] = {0};
        std::atomic<bool> write_done{false};
        std::atomic<bool> read_done{false};
        io_result write_result{};
        io_result read_result{};
        
        auto write_coro = [&]() -> task<void> {
            write_result = co_await server_stream->write(msg, strlen(msg));
            write_done = true;
        };
        
        auto read_coro = [&]() -> task<void> {
            read_result = co_await client_stream->read(buffer, sizeof(buffer) - 1);
            read_done = true;
        };
        
        auto write_task = write_coro();
        auto read_task = read_coro();
        write_task.handle().resume();
        read_task.handle().resume();
        
        for (int i = 0; i < 200 && (!write_done || !read_done); ++i) {
            ctx.poll(std::chrono::milliseconds(10));
        }
        
        REQUIRE(write_done);
        REQUIRE(read_done);
        REQUIRE(write_result.success());
        REQUIRE(read_result.success());
        REQUIRE(write_result.bytes_transferred() == static_cast<int>(strlen(msg)));
        REQUIRE(read_result.bytes_transferred() == static_cast<int>(strlen(msg)));
        REQUIRE(std::string(buffer) == msg);
    }
}

TEST_CASE("UDS multiple concurrent connections", "[uds][concurrent]") {
    io_context ctx(io_context::backend_type::epoll);
    
    auto addr = unix_address::abstract("elio_test_concurrent_" + std::to_string(getpid()));
    
    auto listener = uds_listener::bind(addr, ctx);
    REQUIRE(listener.has_value());
    
    constexpr int NUM_CLIENTS = 3;
    std::array<std::optional<uds_stream>, NUM_CLIENTS> server_streams;
    std::array<std::optional<uds_stream>, NUM_CLIENTS> client_streams;
    std::atomic<int> accepts_done{0};
    std::atomic<int> connects_done{0};
    
    // Accept coroutines - use array to avoid vector reallocation issues
    auto accept0 = [&]() -> task<void> {
        auto stream = co_await listener->accept();
        server_streams[0] = std::move(stream);
        accepts_done++;
    };
    auto accept1 = [&]() -> task<void> {
        auto stream = co_await listener->accept();
        server_streams[1] = std::move(stream);
        accepts_done++;
    };
    auto accept2 = [&]() -> task<void> {
        auto stream = co_await listener->accept();
        server_streams[2] = std::move(stream);
        accepts_done++;
    };
    
    // Connect coroutines
    auto connect0 = [&]() -> task<void> {
        auto stream = co_await uds_connect(ctx, addr);
        client_streams[0] = std::move(stream);
        connects_done++;
    };
    auto connect1 = [&]() -> task<void> {
        auto stream = co_await uds_connect(ctx, addr);
        client_streams[1] = std::move(stream);
        connects_done++;
    };
    auto connect2 = [&]() -> task<void> {
        auto stream = co_await uds_connect(ctx, addr);
        client_streams[2] = std::move(stream);
        connects_done++;
    };
    
    auto a0 = accept0(); auto a1 = accept1(); auto a2 = accept2();
    auto c0 = connect0(); auto c1 = connect1(); auto c2 = connect2();
    
    // Start all coroutines
    a0.handle().resume();
    a1.handle().resume();
    a2.handle().resume();
    c0.handle().resume();
    c1.handle().resume();
    c2.handle().resume();
    
    // Poll until all connections are made
    for (int i = 0; i < 500 && (accepts_done < NUM_CLIENTS || connects_done < NUM_CLIENTS); ++i) {
        ctx.poll(std::chrono::milliseconds(10));
    }
    
    REQUIRE(accepts_done == NUM_CLIENTS);
    REQUIRE(connects_done == NUM_CLIENTS);
    
    for (int i = 0; i < NUM_CLIENTS; ++i) {
        REQUIRE(server_streams[i].has_value());
        REQUIRE(client_streams[i].has_value());
        REQUIRE(server_streams[i]->is_valid());
        REQUIRE(client_streams[i]->is_valid());
    }
}

TEST_CASE("UDS filesystem socket", "[uds][filesystem]") {
    io_context ctx(io_context::backend_type::epoll);
    
    // Use filesystem socket
    std::string path = "/tmp/elio_test_fs_" + std::to_string(getpid()) + ".sock";
    unix_address addr(path);
    
    // Ensure socket file doesn't exist
    ::unlink(path.c_str());
    
    auto listener = uds_listener::bind(addr, ctx);
    REQUIRE(listener.has_value());
    
    // Socket file should exist
    struct stat st;
    REQUIRE(stat(path.c_str(), &st) == 0);
    REQUIRE(S_ISSOCK(st.st_mode));
    
    // Create client connection
    std::atomic<bool> connected{false};
    std::optional<uds_stream> client_stream;
    
    auto connect_coro = [&]() -> task<void> {
        auto stream = co_await uds_connect(ctx, addr);
        client_stream = std::move(stream);
        connected = true;
    };
    
    // Accept on server
    std::atomic<bool> accepted{false};
    std::optional<uds_stream> server_stream;
    
    auto accept_coro = [&]() -> task<void> {
        auto stream = co_await listener->accept();
        server_stream = std::move(stream);
        accepted = true;
    };
    
    auto accept_task = accept_coro();
    auto connect_task = connect_coro();
    accept_task.handle().resume();
    connect_task.handle().resume();
    
    for (int i = 0; i < 200 && (!connected || !accepted); ++i) {
        ctx.poll(std::chrono::milliseconds(10));
    }
    
    REQUIRE(connected);
    REQUIRE(accepted);
    
    // Close listener - should unlink socket file
    listener->close();
    REQUIRE(stat(path.c_str(), &st) != 0);  // File should be gone
}

TEST_CASE("UDS echo test", "[uds][echo]") {
    io_context ctx(io_context::backend_type::epoll);
    
    auto addr = unix_address::abstract("elio_test_echo_" + std::to_string(getpid()));
    
    auto listener = uds_listener::bind(addr, ctx);
    REQUIRE(listener.has_value());
    
    // Use a simpler pattern: thread for client, coroutine for server
    std::atomic<bool> server_done{false};
    std::atomic<bool> client_done{false};
    char server_recv[64] = {0};
    char client_recv[64] = {0};
    int server_bytes = 0;
    int client_bytes = 0;
    
    // Server coroutine
    auto server_coro = [&]() -> task<void> {
        auto stream = co_await listener->accept();
        if (!stream) {
            server_done = true;
            co_return;
        }
        
        // Read
        auto r = co_await stream->read(server_recv, sizeof(server_recv) - 1);
        if (r.result > 0) {
            server_bytes = r.result;
            // Echo back
            co_await stream->write(server_recv, r.result);
        }
        server_done = true;
    };
    
    auto server_task = server_coro();
    server_task.handle().resume();
    
    // Client in a thread (to avoid coroutine complexity)
    std::thread client_thread([&]() {
        // Wait briefly for server to be ready
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        
        int fd = socket(AF_UNIX, SOCK_STREAM, 0);
        if (fd < 0) {
            client_done = true;
            return;
        }
        
        auto sa = addr.to_sockaddr();
        if (connect(fd, reinterpret_cast<struct sockaddr*>(&sa), addr.sockaddr_len()) < 0) {
            close(fd);
            client_done = true;
            return;
        }
        
        const char* msg = "Hello!";
        if (send(fd, msg, strlen(msg), 0) <= 0) {
            close(fd);
            client_done = true;
            return;
        }
        
        client_bytes = static_cast<int>(recv(fd, client_recv, sizeof(client_recv) - 1, 0));
        close(fd);
        client_done = true;
    });
    
    // Poll until both complete
    for (int i = 0; i < 500 && (!server_done || !client_done); ++i) {
        ctx.poll(std::chrono::milliseconds(10));
    }
    
    client_thread.join();
    
    REQUIRE(server_done);
    REQUIRE(client_done);
    REQUIRE(server_bytes == 6);  // "Hello!"
    REQUIRE(client_bytes == 6);
    REQUIRE(std::string(client_recv) == "Hello!");
}

// ============================================================================
// TCP/IP Address and IPv6 Tests
// ============================================================================

#include <elio/net/tcp.hpp>

TEST_CASE("ipv4_address basic operations", "[tcp][address][ipv4]") {
    SECTION("default constructor") {
        ipv4_address addr;
        REQUIRE(addr.addr == INADDR_ANY);
        REQUIRE(addr.port == 0);
    }
    
    SECTION("port-only constructor") {
        ipv4_address addr(8080);
        REQUIRE(addr.addr == INADDR_ANY);
        REQUIRE(addr.port == 8080);
    }
    
    SECTION("ip and port constructor") {
        ipv4_address addr("127.0.0.1", 8080);
        REQUIRE(addr.port == 8080);
        REQUIRE(addr.to_string() == "127.0.0.1:8080");
    }
    
    SECTION("family returns AF_INET") {
        ipv4_address addr;
        REQUIRE(addr.family() == AF_INET);
    }
    
    SECTION("to_sockaddr conversion") {
        ipv4_address addr("192.168.1.1", 443);
        auto sa = addr.to_sockaddr();
        REQUIRE(sa.sin_family == AF_INET);
        REQUIRE(ntohs(sa.sin_port) == 443);
    }
}

TEST_CASE("ipv6_address basic operations", "[tcp][address][ipv6]") {
    SECTION("default constructor") {
        ipv6_address addr;
        REQUIRE(addr.port == 0);
        REQUIRE(addr.scope_id == 0);
    }
    
    SECTION("port-only constructor") {
        ipv6_address addr(8080);
        REQUIRE(addr.port == 8080);
    }
    
    SECTION("ipv6 loopback address") {
        ipv6_address addr("::1", 8080);
        REQUIRE(addr.port == 8080);
        REQUIRE(addr.to_string() == "[::1]:8080");
    }
    
    SECTION("ipv6 any address") {
        ipv6_address addr("::", 80);
        REQUIRE(addr.port == 80);
        // to_string may vary, just check it contains "::" and port
        auto str = addr.to_string();
        REQUIRE(str.find("::") != std::string::npos);
        REQUIRE(str.find(":80") != std::string::npos);
    }
    
    SECTION("family returns AF_INET6") {
        ipv6_address addr;
        REQUIRE(addr.family() == AF_INET6);
    }
    
    SECTION("to_sockaddr conversion") {
        ipv6_address addr("::1", 443);
        auto sa = addr.to_sockaddr();
        REQUIRE(sa.sin6_family == AF_INET6);
        REQUIRE(ntohs(sa.sin6_port) == 443);
    }
    
    SECTION("v4-mapped detection") {
        // Create an IPv4-mapped IPv6 address (::ffff:127.0.0.1)
        ipv6_address addr;
        addr.port = 80;
        // Manually set IPv4-mapped address
        std::memset(&addr.addr, 0, sizeof(addr.addr));
        addr.addr.s6_addr[10] = 0xff;
        addr.addr.s6_addr[11] = 0xff;
        addr.addr.s6_addr[12] = 127;
        addr.addr.s6_addr[13] = 0;
        addr.addr.s6_addr[14] = 0;
        addr.addr.s6_addr[15] = 1;
        REQUIRE(addr.is_v4_mapped());
    }
}

TEST_CASE("socket_address variant operations", "[tcp][address][socket_address]") {
    SECTION("construct from ipv4_address") {
        ipv4_address v4("127.0.0.1", 8080);
        socket_address addr(v4);
        REQUIRE(addr.is_v4());
        REQUIRE_FALSE(addr.is_v6());
        REQUIRE(addr.family() == AF_INET);
        REQUIRE(addr.port() == 8080);
    }
    
    SECTION("construct from ipv6_address") {
        ipv6_address v6("::1", 8080);
        socket_address addr(v6);
        REQUIRE(addr.is_v6());
        REQUIRE_FALSE(addr.is_v4());
        REQUIRE(addr.family() == AF_INET6);
        REQUIRE(addr.port() == 8080);
    }
    
    SECTION("port-only defaults to IPv6") {
        socket_address addr(8080);
        REQUIRE(addr.is_v6());
        REQUIRE(addr.port() == 8080);
    }
    
    SECTION("host with colon parsed as IPv6") {
        socket_address addr("::1", 443);
        REQUIRE(addr.is_v6());
        REQUIRE(addr.port() == 443);
    }
    
    SECTION("dotted quad parsed as IPv4") {
        socket_address addr("192.168.1.1", 80);
        REQUIRE(addr.is_v4());
        REQUIRE(addr.port() == 80);
    }
    
    SECTION("to_sockaddr fills storage correctly") {
        socket_address addr_v4(ipv4_address("10.0.0.1", 80));
        struct sockaddr_storage ss;
        socklen_t len = addr_v4.to_sockaddr(ss);
        REQUIRE(ss.ss_family == AF_INET);
        REQUIRE(len == sizeof(struct sockaddr_in));
        
        socket_address addr_v6(ipv6_address("::1", 443));
        len = addr_v6.to_sockaddr(ss);
        REQUIRE(ss.ss_family == AF_INET6);
        REQUIRE(len == sizeof(struct sockaddr_in6));
    }
    
    SECTION("to_string works for both families") {
        socket_address v4(ipv4_address("1.2.3.4", 80));
        REQUIRE(v4.to_string() == "1.2.3.4:80");
        
        socket_address v6(ipv6_address("::1", 443));
        REQUIRE(v6.to_string() == "[::1]:443");
    }
}

TEST_CASE("TCP IPv6 listener and connect", "[tcp][ipv6][integration]") {
    io_context ctx(io_context::backend_type::epoll);
    
    SECTION("IPv6 listener binds successfully") {
        // Use IPv6 loopback to avoid network issues
        auto listener = tcp_listener::bind(ipv6_address("::1", 0), ctx);
        REQUIRE(listener.has_value());
        REQUIRE(listener->is_valid());
        REQUIRE(listener->local_address().family() == AF_INET6);
        REQUIRE(listener->local_address().port() > 0);
    }
    
    SECTION("IPv6 accept and connect") {
        // Create listener on IPv6 loopback
        auto listener = tcp_listener::bind(ipv6_address("::1", 0), ctx);
        REQUIRE(listener.has_value());
        
        // Get the assigned port
        uint16_t port = listener->local_address().port();
        REQUIRE(port > 0);
        
        std::atomic<bool> accepted{false};
        std::atomic<bool> connected{false};
        std::optional<tcp_stream> server_stream;
        std::optional<tcp_stream> client_stream;
        
        auto accept_coro = [&]() -> task<void> {
            auto stream = co_await listener->accept();
            server_stream = std::move(stream);
            accepted = true;
        };
        
        auto connect_coro = [&]() -> task<void> {
            auto stream = co_await tcp_connect(ctx, ipv6_address("::1", port));
            client_stream = std::move(stream);
            connected = true;
        };
        
        auto accept_task = accept_coro();
        auto connect_task = connect_coro();
        accept_task.handle().resume();
        connect_task.handle().resume();
        
        for (int i = 0; i < 200 && (!accepted || !connected); ++i) {
            ctx.poll(std::chrono::milliseconds(10));
        }
        
        REQUIRE(accepted);
        REQUIRE(connected);
        REQUIRE(server_stream.has_value());
        REQUIRE(client_stream.has_value());
    }
}

TEST_CASE("socket_address with hostname resolution", "[tcp][address][dns]") {
    // Test that socket_address can be constructed from "localhost"
    // This tests the DNS resolution path
    SECTION("localhost resolves") {
        socket_address addr("localhost", 80);
        // Should resolve to either IPv4 or IPv6
        REQUIRE((addr.is_v4() || addr.is_v6()));
        REQUIRE(addr.port() == 80);
    }
}
