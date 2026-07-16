#include <catch2/catch_test_macros.hpp>
#include <elio/io/io_context.hpp>
#include <elio/io/io_awaitables.hpp>
#include <elio/coro/task.hpp>
#include <elio/coro/cancel_token.hpp>
#include <elio/runtime/scheduler.hpp>
#include <elio/net/resolve.hpp>
#include <elio/net/tcp.hpp>
#include <elio/time/timer.hpp>
#if defined(ELIO_HAS_TLS) && ELIO_HAS_TLS
#include <elio/tls/tls_stream.hpp>
#include <openssl/evp.h>
#include <openssl/rsa.h>
#include <openssl/x509.h>
#endif
#include "../test_main.cpp"

#include <unistd.h>
#include <fcntl.h>
#ifdef __linux__
#include <dirent.h>
#endif
#include <sys/socket.h>
#include <sys/eventfd.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <cerrno>
#include <csignal>
#include <cstdint>
#include <cstring>
#include <thread>
#include <atomic>
#include <array>
#include <algorithm>
#include <chrono>
#include <limits>
#include <string>
#include <vector>

using namespace elio::io;
using namespace elio::coro;
using namespace elio::runtime;

#ifdef SIGPIPE
namespace {

volatile std::sig_atomic_t observed_sigpipe = 0;

void record_sigpipe(int) {
    observed_sigpipe = 1;
}

class scoped_sigpipe_probe {
public:
    scoped_sigpipe_probe() {
        observed_sigpipe = 0;
        struct sigaction action {};
        action.sa_handler = record_sigpipe;
        sigemptyset(&action.sa_mask);
        installed_ = sigaction(SIGPIPE, &action, &old_) == 0;
    }

    ~scoped_sigpipe_probe() {
        if (installed_) {
            sigaction(SIGPIPE, &old_, nullptr);
        }
    }

    scoped_sigpipe_probe(const scoped_sigpipe_probe&) = delete;
    scoped_sigpipe_probe& operator=(const scoped_sigpipe_probe&) = delete;

    bool installed() const noexcept { return installed_; }
    bool saw_sigpipe() const noexcept { return observed_sigpipe != 0; }

private:
    struct sigaction old_ {};
    bool installed_ = false;
};

template<typename Stream, typename WriteFn>
io_result run_socket_write_after_peer_read_shutdown(WriteFn&& write_fn) {
    int sv[2] = {-1, -1};
    REQUIRE(socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0, sv) == 0);
    REQUIRE(shutdown(sv[1], SHUT_RD) == 0);

    std::optional<Stream> stream(std::in_place, sv[0]);
    sv[0] = -1;
    io_result write_result{};
    std::atomic<bool> completed{false};

    scheduler sched(1);
    sched.start();
    sched.go([&]() -> task<void> {
        write_result = co_await write_fn(*stream);
        completed.store(true, std::memory_order_release);
    });

    for (int i = 0; i < 200 &&
                    !completed.load(std::memory_order_acquire); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    REQUIRE(completed.load(std::memory_order_acquire));
    REQUIRE(sched.shutdown(elio::test::scaled_ms(2000)));

    stream.reset();
    close(sv[1]);
    return write_result;
}

} // namespace
#endif

#if ELIO_HAS_IO_URING
namespace {

template<typename Ring>
void poison_io_uring_submit_fds(Ring* ring,
                                int& original_ring_fd,
                                int& original_enter_ring_fd) {
    original_ring_fd = ring->ring_fd;
    ring->ring_fd = -1;

    original_enter_ring_fd = -1;
    if constexpr (requires(Ring* r) { r->enter_ring_fd; }) {
        original_enter_ring_fd = ring->enter_ring_fd;
        ring->enter_ring_fd = -1;
    }
}

template<typename Ring>
void restore_io_uring_submit_fds(Ring* ring,
                                 int original_ring_fd,
                                 int original_enter_ring_fd) {
    ring->ring_fd = original_ring_fd;
    if constexpr (requires(Ring* r) { r->enter_ring_fd; }) {
        ring->enter_ring_fd = original_enter_ring_fd;
    }
}

} // namespace
#endif

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
        REQUIRE_FALSE(ctx.is_io_uring());
    }

    SECTION("auto-detect falls back when io_uring is unavailable") {
#if ELIO_HAS_IO_URING
        if (!io_uring_backend::is_available()) {
            io_context ctx(io_context::backend_type::auto_detect);
            REQUIRE(ctx.get_backend_type() == io_context::backend_type::epoll);
            REQUIRE(std::string(ctx.get_backend_name()) == "epoll");
            REQUIRE_FALSE(ctx.is_io_uring());
        } else {
            SUCCEED("io_uring is available at runtime");
        }
#else
        io_context ctx(io_context::backend_type::auto_detect);
        REQUIRE(ctx.get_backend_type() == io_context::backend_type::epoll);
        REQUIRE(std::string(ctx.get_backend_name()) == "epoll");
        REQUIRE_FALSE(ctx.is_io_uring());
#endif
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

TEST_CASE("io_uring short submit keeps excess operations pending",
          "[io][io_uring][submit][regression]") {
#if ELIO_HAS_IO_URING
    REQUIRE_FALSE(elio::io::detail::is_io_uring_short_submit(3, 3));
    REQUIRE(elio::io::detail::is_io_uring_short_submit(3, 2));
    REQUIRE(elio::io::detail::is_io_uring_short_submit(3, 0));
    REQUIRE_FALSE(elio::io::detail::is_io_uring_short_submit(3, -EAGAIN));

    SECTION("poll submit retry drains positive short submits before blocking") {
        std::vector<size_t> ready_values{3, 1, 1, 0};
        std::vector<int> submit_values{2, 1};
        size_t ready_index = 0;
        size_t submit_index = 0;
        int short_submit_count = 0;
        int error_count = 0;
        int stalled_count = 0;

        bool can_block = elio::io::detail::submit_queued_io_uring_sqes_for_poll(
            [&]() {
                REQUIRE(ready_index < ready_values.size());
                return ready_values[ready_index++];
            },
            [&]() {
                REQUIRE(submit_index < submit_values.size());
                return submit_values[submit_index++];
            },
            [&](int) { ++error_count; },
            [&](int, size_t) { ++short_submit_count; },
            [&](size_t) { ++stalled_count; });

        REQUIRE(can_block);
        REQUIRE(ready_index == ready_values.size());
        REQUIRE(submit_index == submit_values.size());
        REQUIRE(short_submit_count == 1);
        REQUIRE(error_count == 0);
        REQUIRE(stalled_count == 0);
    }

    SECTION("poll submit retry refuses to block when submit makes no progress") {
        std::vector<size_t> ready_values{2, 2};
        size_t ready_index = 0;
        int error_count = 0;
        int short_submit_count = 0;
        int stalled_count = 0;

        bool can_block = elio::io::detail::submit_queued_io_uring_sqes_for_poll(
            [&]() {
                REQUIRE(ready_index < ready_values.size());
                return ready_values[ready_index++];
            },
            []() { return 0; },
            [&](int) { ++error_count; },
            [&](int, size_t) { ++short_submit_count; },
            [&](size_t) { ++stalled_count; });

        REQUIRE_FALSE(can_block);
        REQUIRE(ready_index == ready_values.size());
        REQUIRE(error_count == 0);
        REQUIRE(short_submit_count == 1);
        REQUIRE(stalled_count == 1);
    }

    SECTION("poll submit retry refuses to block after submit error") {
        int error_count = 0;
        int short_submit_count = 0;
        int stalled_count = 0;

        bool can_block = elio::io::detail::submit_queued_io_uring_sqes_for_poll(
            []() { return size_t{2}; },
            []() { return -EAGAIN; },
            [&](int error) {
                REQUIRE(error == -EAGAIN);
                ++error_count;
            },
            [&](int, size_t) { ++short_submit_count; },
            [&](size_t) { ++stalled_count; });

        REQUIRE_FALSE(can_block);
        REQUIRE(error_count == 1);
        REQUIRE(short_submit_count == 0);
        REQUIRE(stalled_count == 0);
    }

    SECTION("batch submit path keeps short-submitted segments in flight") {
        if (!io_uring_backend::is_available()) {
            SUCCEED("io_uring is not available at runtime");
            return;
        }

        io_uring_backend backend;
        batch_state st(3);
        int submit_calls = 0;

        bool submitted = elio::io::detail::submit_batch_io_uring_with_submitter(
            &backend, st, std::coroutine_handle<>{},
            [](struct io_uring_sqe* sqe, int) {
                io_uring_prep_nop(sqe);
            },
            [&](struct io_uring*) {
                ++submit_calls;
                return 1;
            });

        REQUIRE(submitted);
        REQUIRE(submit_calls == 1);
        REQUIRE(backend.pending_count() == 3);
        REQUIRE(st.completed.load(std::memory_order_acquire) == 0);
        REQUIRE_FALSE(st.all_done());
        REQUIRE(std::all_of(st.results.begin(), st.results.end(),
                            [](int result) { return result == 0; }));

        for (int attempts = 0; backend.pending_count() != 0 && attempts < 20; ++attempts) {
            backend.poll(std::chrono::milliseconds(10));
        }

        REQUIRE(backend.pending_count() == 0);
        REQUIRE(st.completed.load(std::memory_order_acquire) == st.total);
        REQUIRE(st.all_done());
        REQUIRE(std::all_of(st.results.begin(), st.results.end(),
                            [](int result) { return result == 0; }));
    }

    SECTION("batch submit path keeps negative-submit segments retryable") {
        if (!io_uring_backend::is_available()) {
            SUCCEED("io_uring is not available at runtime");
            return;
        }

        io_uring_backend backend;
        batch_state st(2);
        int submit_calls = 0;

        bool submitted = elio::io::detail::submit_batch_io_uring_with_submitter(
            &backend, st, std::coroutine_handle<>{},
            [](struct io_uring_sqe* sqe, int) {
                io_uring_prep_nop(sqe);
            },
            [&](struct io_uring*) {
                ++submit_calls;
                return -EAGAIN;
            });

        REQUIRE(submitted);
        REQUIRE(submit_calls == 1);
        REQUIRE(backend.pending_count() == 2);
        REQUIRE(st.completed.load(std::memory_order_acquire) == 0);
        REQUIRE_FALSE(st.all_done());
        REQUIRE(std::all_of(st.results.begin(), st.results.end(),
                            [](int result) { return result == 0; }));

        for (int attempts = 0; backend.pending_count() != 0 && attempts < 20; ++attempts) {
            backend.poll(std::chrono::milliseconds(10));
        }

        REQUIRE(backend.pending_count() == 0);
        REQUIRE(st.completed.load(std::memory_order_acquire) == st.total);
        REQUIRE(st.all_done());
        REQUIRE(std::all_of(st.results.begin(), st.results.end(),
                            [](int result) { return result == 0; }));
    }
#else
    SUCCEED("io_uring backend is not compiled in");
#endif
}

TEST_CASE("io_uring submit failure keeps staged operation pending",
          "[io][io_uring][submit][regression]") {
#if ELIO_HAS_IO_URING
    if (!io_uring_backend::is_available()) {
        SUCCEED("io_uring is not available at runtime");
        return;
    }

    io_uring_backend backend;
    auto* ring = backend.get_ring();
    REQUIRE(ring != nullptr);

    char buffer = 0;
    io_request req{};
    req.op = io_op::read;
    req.fd = -1;
    req.buffer = &buffer;
    req.length = sizeof(buffer);

    REQUIRE(backend.prepare(req));
    REQUIRE(backend.pending_count() == 1);

    // Force the public submit() path to see a ring-level failure without
    // consuming the staged SQE. Restoring ring_fd lets the same SQE retry.
    int original_ring_fd = -1;
    int original_enter_ring_fd = -1;
    poison_io_uring_submit_fds(ring, original_ring_fd, original_enter_ring_fd);
    REQUIRE(original_ring_fd >= 0);
    int submitted = backend.submit();
    restore_io_uring_submit_fds(ring, original_ring_fd, original_enter_ring_fd);

    REQUIRE(submitted < 0);
    REQUIRE(backend.pending_count() == 1);

    REQUIRE(backend.submit() >= 0);
    for (int attempts = 0; backend.pending_count() != 0 && attempts < 20; ++attempts) {
        backend.poll(std::chrono::milliseconds(10));
    }

    REQUIRE(backend.pending_count() == 0);
#else
    SUCCEED("io_uring backend is not compiled in");
#endif
}

TEST_CASE("io_uring prepare rejection is side-effect free",
          "[io][io_uring][contract][regression]") {
#if ELIO_HAS_IO_URING
    io_request none{};
    none.op = io_op::none;
    REQUIRE_FALSE(elio::io::detail::io_uring_prepare_request_is_valid(none));

    io_request unknown{};
    unknown.op = static_cast<io_op>(255);
    REQUIRE_FALSE(elio::io::detail::io_uring_prepare_request_is_valid(unknown));

    io_request missing_timeout_ts{};
    missing_timeout_ts.op = io_op::timeout;
    missing_timeout_ts.length = 1;
    REQUIRE_FALSE(elio::io::detail::io_uring_prepare_request_is_valid(
        missing_timeout_ts));

    if (!io_uring_backend::is_available()) {
        SUCCEED("io_uring is not available at runtime");
        return;
    }

    io_uring_backend backend;
    auto* ring = backend.get_ring();
    REQUIRE(ring != nullptr);
    REQUIRE(io_uring_sq_ready(ring) == 0);
    REQUIRE(backend.pending_count() == 0);

    auto assert_rejected_without_side_effects = [&](io_request req) {
        REQUIRE_FALSE(backend.prepare(req));
        REQUIRE(io_uring_sq_ready(ring) == 0);
        REQUIRE(backend.pending_count() == 0);
    };

    assert_rejected_without_side_effects(none);
    assert_rejected_without_side_effects(unknown);
    assert_rejected_without_side_effects(missing_timeout_ts);
#else
    SUCCEED("io_uring backend is not compiled in");
#endif
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

TEST_CASE("epoll_backend registration failure does not leave pending op", "[io][epoll][registration]") {
    epoll_backend backend;
    char buffer = 0;

    io_request req{};
    req.op = io_op::read;
    req.fd = -1;
    req.buffer = &buffer;
    req.length = sizeof(buffer);

    SECTION("returns failure to raw prepare caller") {
        REQUIRE_FALSE(backend.prepare(req));

        auto result = epoll_backend::get_last_result();
        REQUIRE(result.result == -EBADF);
    }

    REQUIRE_FALSE(backend.has_pending());
    REQUIRE(backend.pending_count() == 0);
}

TEST_CASE("Pipe read/write with epoll", "[io][epoll][pipe]") {
    int pipefd[2];
    REQUIRE(pipe(pipefd) == 0);
    
    // Make non-blocking
    fcntl(pipefd[0], F_SETFL, O_NONBLOCK);
    fcntl(pipefd[1], F_SETFL, O_NONBLOCK);
    
    // Write some data synchronously first
    const char* test_data = "Hello, Elio!";
    ssize_t written = write(pipefd[1], test_data, strlen(test_data));
    REQUIRE(written == static_cast<ssize_t>(strlen(test_data)));
    
    // Read using scheduler (coroutines run on worker threads with their own io_context)
    char buffer[64] = {0};
    std::atomic<bool> completed{false};
    io_result read_result{};
    
    scheduler sched(1);
    sched.start();
    
    // Create a simple test coroutine
    auto read_coro = [&]() -> task<void> {
        auto result = co_await async_read(pipefd[0], buffer, sizeof(buffer) - 1);
        read_result = result;
        completed = true;
    };
    
    sched.go(read_coro);
    
    // Wait for completion
    for (int i = 0; i < 100 && !completed; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    
    sched.shutdown();
    
    REQUIRE(completed);
    REQUIRE(read_result.success());
    REQUIRE(read_result.bytes_transferred() == static_cast<int>(strlen(test_data)));
    REQUIRE(std::string(buffer) == test_data);
    
    close(pipefd[0]);
    close(pipefd[1]);
}

TEST_CASE("Pipe readv with scheduler", "[io][readv][pipe]") {
    int pipefd[2];
    REQUIRE(pipe(pipefd) == 0);

    fcntl(pipefd[0], F_SETFL, O_NONBLOCK);
    fcntl(pipefd[1], F_SETFL, O_NONBLOCK);

    const char* test_data = "Hello, Elio!";
    ssize_t written = write(pipefd[1], test_data, strlen(test_data));
    REQUIRE(written == static_cast<ssize_t>(strlen(test_data)));

    std::array<char, 7> first{};
    std::array<char, 5> second{};
    struct iovec iovecs[2] = {
        {first.data(), first.size()},
        {second.data(), second.size()},
    };
    std::atomic<bool> completed{false};
    io_result read_result{};

    scheduler sched(1);
    sched.start();

    auto read_coro = [&]() -> task<void> {
        auto result = co_await async_readv(pipefd[0], iovecs, 2);
        read_result = result;
        completed = true;
    };

    sched.go(read_coro);

    for (int i = 0; i < 100 && !completed; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    sched.shutdown();

    REQUIRE(completed);
    REQUIRE(read_result.success());
    REQUIRE(read_result.bytes_transferred() ==
            static_cast<int>(strlen(test_data)));
    auto actual = std::string(first.data(), first.size()) +
                  std::string(second.data(), second.size());
    REQUIRE(actual == test_data);

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
    
    const char* msg = "Socket test message";
    
    // Send on one end
    ssize_t sent = send(sv[0], msg, strlen(msg), 0);
    REQUIRE(sent == static_cast<ssize_t>(strlen(msg)));
    
    // Receive on the other end using scheduler
    char buffer[64] = {0};
    std::atomic<bool> completed{false};
    io_result recv_result{};
    
    scheduler sched(1);
    sched.start();
    
    auto recv_coro = [&]() -> task<void> {
        auto result = co_await async_recv(sv[1], buffer, sizeof(buffer) - 1);
        recv_result = result;
        completed = true;
    };
    
    sched.go(recv_coro);
    
    for (int i = 0; i < 100 && !completed; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    
    sched.shutdown();
    
    REQUIRE(completed);
    REQUIRE(recv_result.success());
    REQUIRE(recv_result.bytes_transferred() == static_cast<int>(strlen(msg)));
    REQUIRE(std::string(buffer) == msg);
    
    close(sv[0]);
    close(sv[1]);
}

TEST_CASE("epoll_backend drains readable data before EOF on hangup",
          "[io][epoll][socket][hup]") {
    int sv[2];
    REQUIRE(socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0, sv) == 0);

    const char* msg = "final frame before close";
    REQUIRE(send(sv[0], msg, strlen(msg), 0) ==
            static_cast<ssize_t>(strlen(msg)));
    REQUIRE(close(sv[0]) == 0);

    epoll_backend backend;
    char buffer[64] = {};

    io_request req{};
    req.op = io_op::recv;
    req.fd = sv[1];
    req.buffer = buffer;
    req.length = sizeof(buffer);
    req.awaiter = std::noop_coroutine();

    REQUIRE(backend.prepare(req));
    REQUIRE(backend.poll(std::chrono::milliseconds(100)) == 1);

    auto result = epoll_backend::get_last_result();
    REQUIRE(result.result == static_cast<int>(strlen(msg)));
    REQUIRE(std::string(buffer, strlen(msg)) == msg);

    char eof_buffer[1] = {};
    io_request eof_req{};
    eof_req.op = io_op::recv;
    eof_req.fd = sv[1];
    eof_req.buffer = eof_buffer;
    eof_req.length = sizeof(eof_buffer);
    eof_req.awaiter = std::noop_coroutine();

    REQUIRE(backend.prepare(eof_req));
    REQUIRE(backend.poll(std::chrono::milliseconds(100)) == 1);

    auto eof_result = epoll_backend::get_last_result();
    REQUIRE(eof_result.result == 0);

    close(sv[1]);
}

TEST_CASE("epoll_backend async connect initiates TCP handshake",
          "[io][epoll][connect][regression]") {
    int listen_fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    REQUIRE(listen_fd >= 0);

    int reuse = 1;
    REQUIRE(setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse)) == 0);

    struct sockaddr_in listen_addr{};
    listen_addr.sin_family = AF_INET;
    listen_addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    listen_addr.sin_port = 0;
    REQUIRE(bind(listen_fd, reinterpret_cast<struct sockaddr*>(&listen_addr),
                 sizeof(listen_addr)) == 0);
    REQUIRE(listen(listen_fd, 1) == 0);

    socklen_t listen_len = sizeof(listen_addr);
    REQUIRE(getsockname(listen_fd, reinterpret_cast<struct sockaddr*>(&listen_addr),
                        &listen_len) == 0);

    int client_fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    REQUIRE(client_fd >= 0);

    epoll_backend backend;
    socklen_t addrlen = sizeof(listen_addr);
    io_request req{};
    req.op = io_op::connect;
    req.fd = client_fd;
    req.addr = reinterpret_cast<struct sockaddr*>(&listen_addr);
    req.addrlen = &addrlen;
    req.awaiter = std::noop_coroutine();

    REQUIRE(backend.prepare(req));
    REQUIRE(backend.has_pending());

    int completions = 0;
    for (int i = 0; i < 50 && completions == 0; ++i) {
        completions += backend.poll(std::chrono::milliseconds(20));
    }
    REQUIRE(completions == 1);

    auto result = epoll_backend::get_last_result();
    INFO("connect result=" << result.result);
    REQUIRE(result.result == 0);

    char byte = 'x';
    REQUIRE(send(client_fd, &byte, sizeof(byte), MSG_NOSIGNAL) ==
            static_cast<ssize_t>(sizeof(byte)));

    int accepted_fd = -1;
    for (int i = 0; i < 50 && accepted_fd < 0; ++i) {
        accepted_fd = accept4(listen_fd, nullptr, nullptr,
                              SOCK_NONBLOCK | SOCK_CLOEXEC);
        if (accepted_fd < 0 && errno == EAGAIN) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }
    REQUIRE(accepted_fd >= 0);

    char received = 0;
    ssize_t received_bytes = -1;
    for (int i = 0; i < 50 && received_bytes < 0; ++i) {
        received_bytes = recv(accepted_fd, &received, sizeof(received), 0);
        if (received_bytes < 0 && errno == EAGAIN) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    }
    REQUIRE(received_bytes == static_cast<ssize_t>(sizeof(received)));
    REQUIRE(received == byte);

    close(accepted_fd);
    close(client_fd);
    close(listen_fd);
}

TEST_CASE("epoll_backend async connect preserves refused SO_ERROR",
          "[io][epoll][connect][regression]") {
    int reserve_fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    REQUIRE(reserve_fd >= 0);

    struct sockaddr_in target{};
    target.sin_family = AF_INET;
    target.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    target.sin_port = 0;
    REQUIRE(bind(reserve_fd, reinterpret_cast<struct sockaddr*>(&target),
                 sizeof(target)) == 0);

    socklen_t target_len = sizeof(target);
    REQUIRE(getsockname(reserve_fd, reinterpret_cast<struct sockaddr*>(&target),
                        &target_len) == 0);
    REQUIRE(close(reserve_fd) == 0);

    int client_fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    REQUIRE(client_fd >= 0);

    epoll_backend backend;
    socklen_t addrlen = sizeof(target);
    io_request req{};
    req.op = io_op::connect;
    req.fd = client_fd;
    req.addr = reinterpret_cast<struct sockaddr*>(&target);
    req.addrlen = &addrlen;
    req.awaiter = std::noop_coroutine();

    REQUIRE(backend.prepare(req));

    int completions = 0;
    for (int i = 0; i < 50 && completions == 0; ++i) {
        completions += backend.poll(std::chrono::milliseconds(20));
    }
    REQUIRE(completions == 1);

    auto result = epoll_backend::get_last_result();
    INFO("connect result=" << result.result);
    REQUIRE(result.result == -ECONNREFUSED);

    close(client_fd);
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
    
    // Start a read that won't complete (no data sent)
    char buffer[64];
    std::atomic<bool> started{false};
    std::atomic<bool> completed{false};
    io_result recv_result{};
    
    scheduler sched(1);
    sched.start();
    
    auto recv_coro = [&]() -> task<void> {
        started = true;
        auto result = co_await async_recv(sv[1], buffer, sizeof(buffer));
        recv_result = result;
        completed = true;
    };
    
    sched.go(recv_coro);
    
    // Wait for coroutine to start
    for (int i = 0; i < 100 && !started; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    
    // Give it a bit more time
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    
    // Note: cancel behavior depends on backend implementation
    // Just verify we don't crash on shutdown with pending operation.
    // Use shutdown_force() — the recv will never complete (no data sent),
    // so graceful shutdown would block forever waiting for it.
    sched.shutdown_force();

    close(sv[0]);
    close(sv[1]);
}

TEST_CASE("Multiple concurrent operations", "[io][epoll][concurrent]") {
    int sv1[2], sv2[2];
    REQUIRE(socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0, sv1) == 0);
    REQUIRE(socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0, sv2) == 0);
    
    const char* msg1 = "Message 1";
    const char* msg2 = "Message 2";
    
    // Send on both pairs
    send(sv1[0], msg1, strlen(msg1), 0);
    send(sv2[0], msg2, strlen(msg2), 0);
    
    char buffer1[64] = {0};
    char buffer2[64] = {0};
    std::atomic<int> completed{0};
    
    scheduler sched(2);  // 2 workers for concurrent operations
    sched.start();
    
    auto recv_coro1 = [&]() -> task<void> {
        co_await async_recv(sv1[1], buffer1, sizeof(buffer1) - 1);
        completed++;
    };
    
    auto recv_coro2 = [&]() -> task<void> {
        co_await async_recv(sv2[1], buffer2, sizeof(buffer2) - 1);
        completed++;
    };
    
    sched.go(recv_coro1);
    sched.go(recv_coro2);
    
    // Wait until both complete
    for (int i = 0; i < 100 && completed < 2; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    
    sched.shutdown();
    
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
    // This test verifies that async operations work correctly when data
    // is not immediately available.
    
    int sv[2];
    REQUIRE(socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0, sv) == 0);
    
    char buffer[64] = {0};
    std::atomic<bool> started{false};
    std::atomic<bool> completed{false};
    io_result recv_result{};
    
    scheduler sched(1);
    sched.start();
    
    auto recv_coro = [&]() -> task<void> {
        started = true;
        auto result = co_await async_recv(sv[1], buffer, sizeof(buffer) - 1);
        recv_result = result;
        completed = true;
    };
    
    sched.go(recv_coro);
    
    // Wait for coroutine to start and register the operation
    for (int i = 0; i < 100 && !started; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    REQUIRE(started);
    
    // Give the I/O operation time to be registered
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    
    // Now send data - this should trigger the read to complete
    const char* msg = "delayed message";
    ssize_t sent = send(sv[0], msg, strlen(msg), 0);
    REQUIRE(sent == static_cast<ssize_t>(strlen(msg)));
    
    // Wait for completion
    for (int i = 0; i < 100 && !completed; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    
    sched.shutdown();
    
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
    
    char buffer1[32] = {0};
    char buffer2[32] = {0};
    std::atomic<int> completed{0};
    
    scheduler sched(1);
    sched.start();
    
    // Start two recv operations on the same fd
    auto recv_coro1 = [&]() -> task<void> {
        co_await async_recv(sv[1], buffer1, sizeof(buffer1) - 1);
        completed++;
    };
    
    auto recv_coro2 = [&]() -> task<void> {
        co_await async_recv(sv[1], buffer2, sizeof(buffer2) - 1);
        completed++;
    };
    
    sched.go(recv_coro1);
    sched.go(recv_coro2);
    
    // Give operations time to be registered
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    
    // Send enough data for both reads
    const char* msg1 = "first";
    const char* msg2 = "second";
    send(sv[0], msg1, strlen(msg1), 0);
    
    // Wait for first read
    for (int i = 0; i < 100 && completed < 1; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    
    send(sv[0], msg2, strlen(msg2), 0);
    
    // Wait for second read
    for (int i = 0; i < 100 && completed < 2; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    
    sched.shutdown();
    
    REQUIRE(completed == 2);
    
    close(sv[0]);
    close(sv[1]);
}

TEST_CASE("epoll_backend write operation registration", "[io][epoll][write]") {
    // Verify write operations work correctly
    
    int sv[2];
    REQUIRE(socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0, sv) == 0);
    
    const char* msg = "write test data";
    std::atomic<bool> completed{false};
    io_result send_result{};
    
    scheduler sched(1);
    sched.start();
    
    auto send_coro = [&]() -> task<void> {
        auto result = co_await async_send(sv[0], msg, strlen(msg));
        send_result = result;
        completed = true;
    };
    
    sched.go(send_coro);
    
    // Wait for completion
    for (int i = 0; i < 100 && !completed; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    
    sched.shutdown();
    
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

namespace {

int count_open_fds_for_uds_test() {
#ifdef __linux__
    DIR* dir = ::opendir("/proc/self/fd");
    if (!dir) {
        return -1;
    }

    int count = 0;
    while (auto* entry = ::readdir(dir)) {
        if (std::strcmp(entry->d_name, ".") != 0 &&
            std::strcmp(entry->d_name, "..") != 0) {
            ++count;
        }
    }
    ::closedir(dir);
    return count;
#else
    return -1;
#endif
}

} // namespace

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

#ifdef SIGPIPE
TEST_CASE("socket stream writes report errors without SIGPIPE",
          "[io][sigpipe][regression]") {
    scoped_sigpipe_probe probe;
    REQUIRE(probe.installed());

    SECTION("tcp_stream write") {
        char byte = 'x';
        auto result =
            run_socket_write_after_peer_read_shutdown<tcp_stream>(
                [&](tcp_stream& stream) -> task<io_result> {
                    co_return co_await stream.write(&byte, sizeof(byte));
                });

        REQUIRE_FALSE(result.success());
        REQUIRE(result.error_code() == EPIPE);
        REQUIRE_FALSE(probe.saw_sigpipe());
    }

    SECTION("uds_stream write") {
        char byte = 'x';
        auto result =
            run_socket_write_after_peer_read_shutdown<uds_stream>(
                [&](uds_stream& stream) -> task<io_result> {
                    co_return co_await stream.write(&byte, sizeof(byte));
                });

        REQUIRE_FALSE(result.success());
        REQUIRE(result.error_code() == EPIPE);
        REQUIRE_FALSE(probe.saw_sigpipe());
    }

    SECTION("tcp_stream writev") {
        auto result =
            run_socket_write_after_peer_read_shutdown<tcp_stream>(
                [](tcp_stream& stream) -> task<io_result> {
                    char first = 'a';
                    char second = 'b';
                    struct iovec iov[2] = {
                        {&first, sizeof(first)},
                        {&second, sizeof(second)}
                    };
                    co_return co_await stream.writev(iov, 2);
                });

        REQUIRE_FALSE(result.success());
        REQUIRE(result.error_code() == EPIPE);
        REQUIRE_FALSE(probe.saw_sigpipe());
    }

    SECTION("uds_stream writev") {
        auto result =
            run_socket_write_after_peer_read_shutdown<uds_stream>(
                [](uds_stream& stream) -> task<io_result> {
                    char first = 'a';
                    char second = 'b';
                    struct iovec iov[2] = {
                        {&first, sizeof(first)},
                        {&second, sizeof(second)}
                    };
                    co_return co_await stream.writev(iov, 2);
                });

        REQUIRE_FALSE(result.success());
        REQUIRE(result.error_code() == EPIPE);
        REQUIRE_FALSE(probe.saw_sigpipe());
    }
}
#endif

TEST_CASE("UDS listener bind and accept", "[uds][listener]") {
    // Use abstract socket to avoid filesystem cleanup issues
    auto addr = unix_address::abstract("elio_test_listener_" + std::to_string(getpid()));
    
    SECTION("bind creates listener") {
        auto listener = uds_listener::bind(addr);
        REQUIRE(listener.has_value());
        REQUIRE(listener->is_valid());
        REQUIRE(listener->fd() >= 0);
        REQUIRE(listener->local_address().to_string() == addr.to_string());
    }

    SECTION("accept returns connection") {
        auto listener = uds_listener::bind(addr);
        REQUIRE(listener.has_value());
        
        // Create a client connection in a separate thread
        std::atomic<bool> client_connected{false};
        std::thread client_thread([&]() {
            // Wait a bit for the accept to be registered
            std::this_thread::sleep_for(std::chrono::milliseconds(50));
            
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
        
        scheduler sched(1);
        sched.start();
        
        auto accept_coro = [&]() -> task<void> {
            auto stream = co_await listener->accept();
            accepted_stream = std::move(stream);
            accepted = true;
            co_return;
        };
        
        sched.go(accept_coro);
        
        // Wait for completion
        for (int i = 0; i < 200 && !accepted; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        
        sched.shutdown();
        
        REQUIRE(accepted);
        REQUIRE(accepted_stream.has_value());
        REQUIRE(accepted_stream->is_valid());
        
        client_thread.join();
    }
}

TEST_CASE("UDS bind validates address before side effects",
          "[uds][listener][contract]") {
    std::string dir = "/tmp/elio_uds_overlong_" + std::to_string(getpid());
    REQUIRE((::mkdir(dir.c_str(), 0700) == 0 || errno == EEXIST));

    std::string path = dir + "/" + std::string(120, 'x');
    struct sockaddr_un sample_addr {};
    REQUIRE(path.size() > sizeof(sample_addr.sun_path) - 1);

    int fd = ::open(path.c_str(), O_CREAT | O_WRONLY | O_TRUNC, 0600);
    REQUIRE(fd >= 0);
    REQUIRE(::write(fd, "sentinel", 8) == 8);
    REQUIRE(::close(fd) == 0);

    const int before_fds = count_open_fds_for_uds_test();

    REQUIRE_THROWS_AS(uds_listener::bind(unix_address(path)), std::invalid_argument);

    struct stat st {};
    REQUIRE(::stat(path.c_str(), &st) == 0);

    const int after_fds = count_open_fds_for_uds_test();
    if (before_fds >= 0 && after_fds >= 0) {
        REQUIRE(after_fds == before_fds);
    }

    REQUIRE(::unlink(path.c_str()) == 0);
    REQUIRE(::rmdir(dir.c_str()) == 0);
}

TEST_CASE("UDS bind rejects overlong abstract address before opening socket",
          "[uds][listener][contract]") {
    struct sockaddr_un sample_addr {};
    auto addr = unix_address::abstract(
        std::string(sizeof(sample_addr.sun_path), 'a'));
    REQUIRE(addr.path.size() > sizeof(sample_addr.sun_path));

    const int before_fds = count_open_fds_for_uds_test();

    REQUIRE_THROWS_AS(uds_listener::bind(addr), std::invalid_argument);

    const int after_fds = count_open_fds_for_uds_test();
    if (before_fds >= 0 && after_fds >= 0) {
        REQUIRE(after_fds == before_fds);
    }
}

TEST_CASE("UDS connect", "[uds][connect]") {
    auto addr = unix_address::abstract("elio_test_connect_" + std::to_string(getpid()));

    // Create server listener
    auto listener = uds_listener::bind(addr);
    REQUIRE(listener.has_value());
    
    // Start accept in background
    std::atomic<bool> server_accepted{false};
    std::optional<uds_stream> server_stream;
    
    std::atomic<bool> client_connected{false};
    std::optional<uds_stream> client_stream;
    
    scheduler sched(2);
    sched.start();
    
    auto accept_coro = [&]() -> task<void> {
        auto stream = co_await listener->accept();
        server_stream = std::move(stream);
        server_accepted = true;
        co_return;
    };

    auto connect_coro = [&]() -> task<void> {
        auto stream = co_await uds_connect(addr);
        client_stream = std::move(stream);
        client_connected = true;
        co_return;
    };

    sched.go(accept_coro);
    sched.go(connect_coro);

    // Wait until both complete
    for (int i = 0; i < 200 && (!server_accepted || !client_connected); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    sched.shutdown();

    REQUIRE(server_accepted);
    REQUIRE(client_connected);
    REQUIRE(server_stream.has_value());
    REQUIRE(client_stream.has_value());
    REQUIRE(server_stream->is_valid());
    REQUIRE(client_stream->is_valid());
}

TEST_CASE("uds_connect observes already-cancelled token",
          "[uds][connect][cancel]") {
    auto addr = unix_address::abstract(
        "elio_test_cancelled_connect_" + std::to_string(getpid()));
    std::optional<uds_stream> stream;
    std::atomic<bool> done{false};
    int observed_errno = 0;

    scheduler sched(1);
    sched.start();

    cancel_source source;
    source.cancel();

    sched.go([&]() -> task<void> {
        errno = 0;
        stream = co_await uds_connect(addr, source.get_token());
        observed_errno = errno;
        done.store(true, std::memory_order_release);
        co_return;
    });

    for (int i = 0; i < 200 && !done.load(std::memory_order_acquire); ++i) {
        std::this_thread::sleep_for(elio::test::scaled_ms(10));
    }
    REQUIRE(sched.shutdown(elio::test::scaled_ms(5000)));

    REQUIRE(done.load(std::memory_order_acquire));
    REQUIRE_FALSE(stream.has_value());
    REQUIRE(observed_errno == ECANCELED);
}

TEST_CASE("UDS stream read/write", "[uds][stream]") {
    auto addr = unix_address::abstract("elio_test_rw_" + std::to_string(getpid()));

    // Create server and client
    auto listener = uds_listener::bind(addr);
    REQUIRE(listener.has_value());
    
    std::optional<uds_stream> server_stream;
    std::optional<uds_stream> client_stream;
    std::atomic<int> setup_complete{0};
    
    scheduler sched(2);
    sched.start();
    
    auto accept_coro = [&]() -> task<void> {
        auto stream = co_await listener->accept();
        server_stream = std::move(stream);
        setup_complete++;
        co_return;
    };

    auto connect_coro = [&]() -> task<void> {
        auto stream = co_await uds_connect(addr);
        client_stream = std::move(stream);
        setup_complete++;
        co_return;
    };
    
    sched.go(accept_coro);
    sched.go(connect_coro);
    
    for (int i = 0; i < 200 && setup_complete < 2; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
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
        
        sched.go(write_coro);
        sched.go(read_coro);
        
        for (int i = 0; i < 200 && (!write_done || !read_done); ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
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
        
        sched.go(write_coro);
        sched.go(read_coro);
        
        for (int i = 0; i < 200 && (!write_done || !read_done); ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        
        REQUIRE(write_done);
        REQUIRE(read_done);
        REQUIRE(write_result.success());
        REQUIRE(read_result.success());
        REQUIRE(write_result.bytes_transferred() == static_cast<int>(strlen(msg)));
        REQUIRE(read_result.bytes_transferred() == static_cast<int>(strlen(msg)));
        REQUIRE(std::string(buffer) == msg);
    }

    sched.shutdown();
}

TEST_CASE("uds_stream read_exactly/write_exactly", "[uds][stream][exact]") {
    // Establish a loopback UDS pair so we exercise the real async read/write
    // path used by the exact-length helpers, mirroring the TCP coverage.
    auto addr = unix_address::abstract("elio_test_exact_" + std::to_string(getpid()));

    auto listener = uds_listener::bind(addr);
    REQUIRE(listener.has_value());

    std::optional<uds_stream> server_stream;
    std::optional<uds_stream> client_stream;
    std::atomic<int> setup_complete{0};

    scheduler sched(2);
    sched.start();

    auto accept_coro = [&]() -> task<void> {
        auto stream = co_await listener->accept();
        server_stream = std::move(stream);
        setup_complete++;
        co_return;
    };

    auto connect_coro = [&]() -> task<void> {
        auto stream = co_await uds_connect(addr);
        client_stream = std::move(stream);
        setup_complete++;
        co_return;
    };

    sched.go(accept_coro);
    sched.go(connect_coro);

    for (int i = 0; i < 200 && setup_complete < 2; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    REQUIRE(setup_complete == 2);
    REQUIRE(server_stream.has_value());
    REQUIRE(client_stream.has_value());

    SECTION("write_exactly then read_exactly transfers full payload") {
        // A payload larger than typical socket buffers so the kernel is
        // likely to split it across several read/write syscalls, exercising
        // the aggregation loop inside the exact helpers.
        constexpr size_t kSize = 256 * 1024;
        std::vector<char> src(kSize);
        for (size_t i = 0; i < kSize; ++i) {
            src[i] = static_cast<char>(i * 31 + 7);
        }
        std::vector<char> dst(kSize, 0);

        std::atomic<bool> write_done{false};
        std::atomic<bool> read_done{false};
        io_result write_result{};
        io_result read_result{};

        auto write_coro = [&]() -> task<void> {
            write_result = co_await client_stream->write_exactly(src.data(), src.size());
            write_done = true;
        };
        auto read_coro = [&]() -> task<void> {
            read_result = co_await server_stream->read_exactly(dst.data(), dst.size());
            read_done = true;
        };

        sched.go(write_coro);
        sched.go(read_coro);

        for (int i = 0; i < 500 && (!write_done || !read_done); ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }

        REQUIRE(write_done);
        REQUIRE(read_done);
        REQUIRE(write_result.success());
        REQUIRE(read_result.success());
        REQUIRE(write_result.bytes_transferred() == static_cast<int>(kSize));
        REQUIRE(read_result.bytes_transferred() == static_cast<int>(kSize));
        REQUIRE(dst == src);
    }

    SECTION("read_exactly aggregates several short writes") {
        // The writer emits the payload in small chunks with a brief pause
        // between them; a single read_exactly on the reader must gather all
        // of them before completing.
        const std::string payload = "The quick brown fox jumps over the lazy dog.";
        std::string received(payload.size(), '\0');

        std::atomic<bool> write_done{false};
        std::atomic<bool> read_done{false};
        io_result read_result{};

        auto write_coro = [&]() -> task<void> {
            size_t offset = 0;
            constexpr size_t chunk = 5;
            while (offset < payload.size()) {
                size_t n = std::min(chunk, payload.size() - offset);
                auto r = co_await server_stream->write(payload.data() + offset, n);
                if (r.result <= 0) break;
                offset += static_cast<size_t>(r.result);
                // Yield so the reader observes a partial buffer and loops.
                co_await elio::time::sleep_for(std::chrono::milliseconds(5));
            }
            write_done = true;
        };
        auto read_coro = [&]() -> task<void> {
            read_result = co_await client_stream->read_exactly(received.data(),
                                                               received.size());
            read_done = true;
        };

        sched.go(read_coro);
        sched.go(write_coro);

        for (int i = 0; i < 500 && (!write_done || !read_done); ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }

        REQUIRE(write_done);
        REQUIRE(read_done);
        REQUIRE(read_result.success());
        REQUIRE(read_result.bytes_transferred() == static_cast<int>(payload.size()));
        REQUIRE(received == payload);
    }

    SECTION("read_exactly returns -ENODATA on EOF before completion") {
        // The peer sends fewer bytes than requested, then closes. read_exactly
        // must surface the truncated stream as -ENODATA rather than a partial
        // success.
        const std::string partial = "half";

        std::atomic<bool> write_done{false};
        std::atomic<bool> read_done{false};
        io_result read_result{};
        char buffer[32] = {0};

        auto write_coro = [&]() -> task<void> {
            co_await server_stream->write(partial.data(), partial.size());
            co_await server_stream->close();
            write_done = true;
        };
        auto read_coro = [&]() -> task<void> {
            // Request more than will ever arrive.
            read_result = co_await client_stream->read_exactly(buffer, sizeof(buffer));
            read_done = true;
        };

        sched.go(read_coro);
        sched.go(write_coro);

        for (int i = 0; i < 500 && (!write_done || !read_done); ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }

        REQUIRE(write_done);
        REQUIRE(read_done);
        REQUIRE_FALSE(read_result.success());
        REQUIRE(read_result.result == -ENODATA);
    }

    SECTION("write_exactly/read_exactly with zero length are no-ops") {
        std::atomic<bool> done{false};
        io_result write_result{100, 0};
        io_result read_result{100, 0};

        auto coro = [&]() -> task<void> {
            write_result = co_await client_stream->write_exactly(nullptr, 0);
            read_result = co_await server_stream->read_exactly(nullptr, 0);
            done = true;
        };

        sched.go(coro);

        for (int i = 0; i < 200 && !done; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }

        REQUIRE(done);
        REQUIRE(write_result.result == 0);
        REQUIRE(read_result.result == 0);
    }

    sched.shutdown();
}

namespace {

struct uds_pair {
    std::optional<uds_stream> server;
    std::optional<uds_stream> client;
};

uds_pair make_small_buffer_uds_pair() {
    int sv[2] = {-1, -1};
    REQUIRE(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0, sv) == 0);

    uds_pair pair{uds_stream(sv[0]), uds_stream(sv[1])};

    const int small = 4096;
    for (int fd : {pair.server->fd(), pair.client->fd()}) {
        REQUIRE(::setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &small,
                             sizeof(small)) == 0);
        REQUIRE(::setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &small,
                             sizeof(small)) == 0);
    }

    return pair;
}

template<typename Predicate>
bool wait_for_uds_stream_test(Predicate&& predicate,
                              std::chrono::milliseconds timeout =
                                  elio::test::scaled_ms(2000)) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!predicate() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(elio::test::scaled_ms(10));
    }
    return predicate();
}

} // namespace

TEST_CASE("uds_stream write awaits writability under backpressure",
          "[uds][stream][eagain][regression]") {
    scheduler sched(2);
    sched.start();

    auto pair = make_small_buffer_uds_pair();

    constexpr size_t kPayload = 1 * 1024 * 1024;
    std::vector<char> to_send(kPayload);
    for (size_t i = 0; i < kPayload; ++i) {
        to_send[i] = static_cast<char>((i * 17) & 0xFF);
    }

    std::atomic<bool> writer_done{false};
    std::atomic<bool> reader_done{false};
    std::atomic<bool> saw_eagain{false};
    std::atomic<size_t> total_written{0};
    std::atomic<size_t> total_read{0};
    std::atomic<bool> data_ok{true};

    auto writer = [&]() -> task<void> {
        size_t off = 0;
        while (off < kPayload) {
            auto r = co_await pair.client->write(to_send.data() + off,
                                                 kPayload - off);
            if (r.result == -EAGAIN || r.result == -EWOULDBLOCK) {
                saw_eagain = true;
                break;
            }
            if (r.result <= 0) {
                break;
            }
            off += static_cast<size_t>(r.result);
        }
        total_written = off;
        writer_done = true;
        co_return;
    };

    auto reader = [&]() -> task<void> {
        std::vector<char> buf(64 * 1024);
        size_t got = 0;
        while (got < kPayload) {
            auto r = co_await pair.server->read(buf.data(), buf.size());
            if (r.result <= 0) {
                break;
            }
            for (int i = 0; i < r.result; ++i) {
                const auto index = got + static_cast<size_t>(i);
                const auto expected = static_cast<char>((index * 17) & 0xFF);
                if (buf[static_cast<size_t>(i)] != expected) {
                    data_ok = false;
                }
            }
            got += static_cast<size_t>(r.result);
        }
        total_read = got;
        reader_done = true;
        co_return;
    };

    sched.go(reader);
    sched.go(writer);

    for (int i = 0; i < 1000 && (!writer_done || !reader_done); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    sched.shutdown();

    REQUIRE(writer_done);
    REQUIRE(reader_done);
    REQUIRE_FALSE(saw_eagain.load());
    REQUIRE(total_written.load() == kPayload);
    REQUIRE(total_read.load() == kPayload);
    REQUIRE(data_ok.load());
}

TEST_CASE("uds_stream read awaits readability for deferred data",
          "[uds][stream][eagain][regression]") {
    scheduler sched(2);
    sched.start();

    auto pair = make_small_buffer_uds_pair();

    const char* msg = "deferred UDS payload after readiness wait";
    const size_t msg_len = strlen(msg);

    std::atomic<bool> reader_done{false};
    std::atomic<bool> writer_done{false};
    std::atomic<size_t> total_written{0};
    io_result read_result{};
    char buffer[128] = {0};

    auto reader = [&]() -> task<void> {
        read_result = co_await pair.server->read(
            std::span<char>(buffer, sizeof(buffer) - 1));
        reader_done = true;
        co_return;
    };

    auto writer = [&]() -> task<void> {
        co_await elio::time::sleep_for(std::chrono::milliseconds(100));
        size_t off = 0;
        while (off < msg_len) {
            auto r = co_await pair.client->write(
                std::string_view(msg + off, msg_len - off));
            if (r.result <= 0) {
                break;
            }
            off += static_cast<size_t>(r.result);
        }
        total_written = off;
        writer_done = off == msg_len;
        co_return;
    };

    sched.go(reader);
    sched.go(writer);

    for (int i = 0; i < 500 && (!reader_done || !writer_done); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    sched.shutdown();

    REQUIRE(reader_done);
    REQUIRE(writer_done);
    REQUIRE(total_written.load() == msg_len);
    REQUIRE(read_result.result != -EAGAIN);
    REQUIRE(read_result.result != -EWOULDBLOCK);
    REQUIRE(read_result.success());
    REQUIRE(read_result.bytes_transferred() == static_cast<int>(msg_len));
    REQUIRE(std::string(buffer) == msg);
}

TEST_CASE("uds_stream read observes cancellation while waiting",
          "[uds][stream][cancel]") {
    scheduler sched(1);
    sched.start();

    auto pair = make_small_buffer_uds_pair();
    cancel_source source;
    std::atomic<bool> started{false};
    std::atomic<bool> completed{false};
    io_result read_result{};
    char buffer[16] = {};

    sched.go([&]() -> task<void> {
        started.store(true, std::memory_order_release);
        read_result = co_await pair.server->read(
            buffer, sizeof(buffer), source.get_token());
        completed.store(true, std::memory_order_release);
        co_return;
    });

    REQUIRE(wait_for_uds_stream_test(
        [&] { return started.load(std::memory_order_acquire); }));

    std::this_thread::sleep_for(elio::test::scaled_ms(50));
    source.cancel();

    REQUIRE(wait_for_uds_stream_test(
        [&] { return completed.load(std::memory_order_acquire); }));
    REQUIRE(sched.shutdown(elio::test::scaled_ms(5000)));
    REQUIRE(read_result.result == -ECANCELED);
}

TEST_CASE("uds_stream read_exactly observes cancellation while waiting",
          "[uds][stream][exact][cancel]") {
    scheduler sched(1);
    sched.start();

    auto pair = make_small_buffer_uds_pair();
    const char first = 'x';
    REQUIRE(::send(pair.client->fd(), &first, 1, MSG_DONTWAIT) == 1);

    cancel_source source;
    std::atomic<bool> started{false};
    std::atomic<bool> completed{false};
    io_result read_result{};
    char buffer[4] = {};

    sched.go([&]() -> task<void> {
        started.store(true, std::memory_order_release);
        read_result = co_await pair.server->read_exactly(
            buffer, sizeof(buffer), source.get_token());
        completed.store(true, std::memory_order_release);
        co_return;
    });

    REQUIRE(wait_for_uds_stream_test(
        [&] { return started.load(std::memory_order_acquire); }));

    std::this_thread::sleep_for(elio::test::scaled_ms(50));
    source.cancel();

    REQUIRE(wait_for_uds_stream_test(
        [&] { return completed.load(std::memory_order_acquire); }));
    REQUIRE(sched.shutdown(elio::test::scaled_ms(5000)));
    REQUIRE(read_result.result == -ECANCELED);
    REQUIRE(buffer[0] == first);
}

TEST_CASE("UDS multiple concurrent connections", "[uds][concurrent]") {
    auto addr = unix_address::abstract("elio_test_concurrent_" + std::to_string(getpid()));

    auto listener = uds_listener::bind(addr);
    REQUIRE(listener.has_value());
    
    constexpr int NUM_CLIENTS = 3;
    std::array<std::optional<uds_stream>, NUM_CLIENTS> server_streams;
    std::array<std::optional<uds_stream>, NUM_CLIENTS> client_streams;
    std::atomic<int> accepts_done{0};
    std::atomic<int> connects_done{0};
    
    scheduler sched(4);
    sched.start();
    
    // Accept coroutines
    auto accept0 = [&]() -> task<void> {
        auto stream = co_await listener->accept();
        server_streams[0] = std::move(stream);
        accepts_done++;
        co_return;
    };
    auto accept1 = [&]() -> task<void> {
        auto stream = co_await listener->accept();
        server_streams[1] = std::move(stream);
        accepts_done++;
        co_return;
    };
    auto accept2 = [&]() -> task<void> {
        auto stream = co_await listener->accept();
        server_streams[2] = std::move(stream);
        accepts_done++;
        co_return;
    };

    // Connect coroutines
    auto connect0 = [&]() -> task<void> {
        auto stream = co_await uds_connect(addr);
        client_streams[0] = std::move(stream);
        connects_done++;
        co_return;
    };
    auto connect1 = [&]() -> task<void> {
        auto stream = co_await uds_connect(addr);
        client_streams[1] = std::move(stream);
        connects_done++;
        co_return;
    };
    auto connect2 = [&]() -> task<void> {
        auto stream = co_await uds_connect(addr);
        client_streams[2] = std::move(stream);
        connects_done++;
        co_return;
    };
    
    // Start all coroutines
    sched.go(accept0);
    sched.go(accept1);
    sched.go(accept2);
    sched.go(connect0);
    sched.go(connect1);
    sched.go(connect2);
    
    // Wait until all connections are made
    for (int i = 0; i < 500 && (accepts_done < NUM_CLIENTS || connects_done < NUM_CLIENTS); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    
    sched.shutdown();
    
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
    // Use filesystem socket
    std::string path = "/tmp/elio_test_fs_" + std::to_string(getpid()) + ".sock";
    unix_address addr(path);
    
    // Ensure socket file doesn't exist
    ::unlink(path.c_str());
    
    auto listener = uds_listener::bind(addr);
    REQUIRE(listener.has_value());

    // Socket file should exist
    struct stat st;
    REQUIRE(stat(path.c_str(), &st) == 0);
    REQUIRE(S_ISSOCK(st.st_mode));

    // Create client connection
    std::atomic<bool> connected{false};
    std::optional<uds_stream> client_stream;

    std::atomic<bool> accepted{false};
    std::optional<uds_stream> server_stream;

    scheduler sched(2);
    sched.start();

    auto connect_coro = [&]() -> task<void> {
        auto stream = co_await uds_connect(addr);
        client_stream = std::move(stream);
        connected = true;
        co_return;
    };

    auto accept_coro = [&]() -> task<void> {
        auto stream = co_await listener->accept();
        server_stream = std::move(stream);
        accepted = true;
        co_return;
    };
    
    sched.go(accept_coro);
    sched.go(connect_coro);
    
    for (int i = 0; i < 200 && (!connected || !accepted); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    
    sched.shutdown();
    
    REQUIRE(connected);
    REQUIRE(accepted);
    
    // Close listener - should unlink socket file
    listener->close();
    REQUIRE(stat(path.c_str(), &st) != 0);  // File should be gone
}

TEST_CASE("UDS echo test", "[uds][echo]") {
    auto addr = unix_address::abstract("elio_test_echo_" + std::to_string(getpid()));

    auto listener = uds_listener::bind(addr);
    REQUIRE(listener.has_value());
    
    // Use a simpler pattern: thread for client, coroutine for server
    std::atomic<bool> server_done{false};
    std::atomic<bool> client_done{false};
    char server_recv[64] = {0};
    char client_recv[64] = {0};
    int server_bytes = 0;
    int client_bytes = 0;
    
    scheduler sched(1);
    sched.start();
    
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
    
    sched.go(server_coro);
    
    // Client in a thread (to avoid coroutine complexity)
    std::thread client_thread([&]() {
        // Wait briefly for server to be ready
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        
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
    
    // Wait until both complete
    for (int i = 0; i < 500 && (!server_done || !client_done); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    
    client_thread.join();
    sched.shutdown();
    
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
#include <elio/net/stream.hpp>
#include <elio/time/timer.hpp>

static task<void> tcp_connect_regression_attempt(
    uint16_t port,
    std::atomic<int>& connected,
    std::atomic<int>& failed,
    std::atomic<int>& first_error) {
    auto stream = co_await tcp_connect(ipv6_address("::1", port));
    if (stream) {
        connected.fetch_add(1, std::memory_order_relaxed);
    } else {
        failed.fetch_add(1, std::memory_order_relaxed);
        int err = errno;
        int expected = 0;
        first_error.compare_exchange_strong(expected, err);
    }
    co_return;
}

static task<void> accept_n_connections(
    tcp_listener& listener,
    int count,
    std::atomic<int>& accepted) {
    for (int i = 0; i < count; ++i) {
        auto stream = co_await listener.accept();
        if (stream) {
            accepted.fetch_add(1, std::memory_order_relaxed);
        }
    }
    co_return;
}

struct tcp_connect_option_observations {
    std::atomic<bool> done{false};
    std::atomic<int> first_error{0};
    std::atomic<int> no_delay{-1};
    std::atomic<int> keep_alive{-1};
    std::atomic<int> reuse_addr{-1};
    std::atomic<int> reuse_port{-1};
    std::atomic<int> recv_buffer{-1};
    std::atomic<int> send_buffer{-1};
};

static int read_socket_option_int(int fd, int level, int optname) {
    int value = 0;
    socklen_t len = sizeof(value);
    if (::getsockopt(fd, level, optname, &value, &len) != 0) {
        return -errno;
    }
    return value;
}

static task<void> tcp_connect_with_options_attempt(
    uint16_t port,
    tcp_connect_option_observations& observed) {
    tcp_options opts;
    opts.reuse_addr = true;
    opts.reuse_port = true;
    opts.no_delay = true;
    opts.keep_alive = true;
    opts.recv_buffer = 4096;
    opts.send_buffer = 8192;

    auto stream = co_await tcp_connect(ipv6_address("::1", port), opts);
    if (!stream) {
        observed.first_error.store(errno, std::memory_order_relaxed);
        observed.done.store(true, std::memory_order_release);
        co_return;
    }

    observed.no_delay.store(
        read_socket_option_int(stream->fd(), IPPROTO_TCP, TCP_NODELAY),
        std::memory_order_relaxed);
    observed.keep_alive.store(
        read_socket_option_int(stream->fd(), SOL_SOCKET, SO_KEEPALIVE),
        std::memory_order_relaxed);
    observed.reuse_addr.store(
        read_socket_option_int(stream->fd(), SOL_SOCKET, SO_REUSEADDR),
        std::memory_order_relaxed);
    observed.reuse_port.store(
        read_socket_option_int(stream->fd(), SOL_SOCKET, SO_REUSEPORT),
        std::memory_order_relaxed);
    observed.recv_buffer.store(
        read_socket_option_int(stream->fd(), SOL_SOCKET, SO_RCVBUF),
        std::memory_order_relaxed);
    observed.send_buffer.store(
        read_socket_option_int(stream->fd(), SOL_SOCKET, SO_SNDBUF),
        std::memory_order_relaxed);
    observed.done.store(true, std::memory_order_release);
    co_return;
}

static task<void> tcp_connect_hostname_attempt(
    std::string host,
    uint16_t port,
    std::atomic<int>& connected,
    std::atomic<int>& failed,
    std::atomic<int>& first_error) {
    resolve_options options;
    options.use_cache = true;

    auto addresses = co_await resolve_all(host, port, options);
    if (addresses.empty()) {
        failed.fetch_add(1, std::memory_order_relaxed);
        int err = errno ? errno : EHOSTUNREACH;
        int expected = 0;
        first_error.compare_exchange_strong(expected, err);
        co_return;
    }

    int last_error = 0;
    for (const auto& addr : addresses) {
        auto stream = co_await tcp_connect(addr);
        if (stream) {
            connected.fetch_add(1, std::memory_order_relaxed);
            co_return;
        }
        last_error = errno;
    }

    failed.fetch_add(1, std::memory_order_relaxed);
    int err = last_error ? last_error : EHOSTUNREACH;
    int expected = 0;
    first_error.compare_exchange_strong(expected, err);
    co_return;
}

static task<void> resolve_hostname_attempt(
    std::string host,
    uint16_t port,
    std::optional<socket_address>& resolved,
    std::atomic<bool>& done) {
    resolved = co_await resolve_hostname(host, port);
    done.store(true, std::memory_order_relaxed);
    co_return;
}

static task<void> resolve_all_attempt_with_options(
    std::string host,
    uint16_t port,
    resolve_options options,
    std::vector<socket_address>& resolved,
    std::atomic<bool>& done) {
    resolved = co_await resolve_all(host, port, options);
    done.store(true, std::memory_order_relaxed);
    co_return;
}

static int count_open_fds_for_tcp_test() {
#ifdef __linux__
    DIR* dir = ::opendir("/proc/self/fd");
    if (!dir) {
        return -1;
    }

    int count = 0;
    while (auto* entry = ::readdir(dir)) {
        if (std::strcmp(entry->d_name, ".") != 0 &&
            std::strcmp(entry->d_name, "..") != 0) {
            ++count;
        }
    }
    ::closedir(dir);
    return count;
#else
    return -1;
#endif
}

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
    SECTION("IPv6 listener binds successfully") {
        // Use IPv6 loopback to avoid network issues
        auto listener = tcp_listener::bind(ipv6_address("::1", 0));
        REQUIRE(listener.has_value());
        REQUIRE(listener->is_valid());
        REQUIRE(listener->local_address().family() == AF_INET6);
        REQUIRE(listener->local_address().port() > 0);
    }

    SECTION("IPv6 accept and connect") {
        // Create listener on IPv6 loopback
        auto listener = tcp_listener::bind(ipv6_address("::1", 0));
        REQUIRE(listener.has_value());
        
        // Get the assigned port
        uint16_t port = listener->local_address().port();
        REQUIRE(port > 0);
        
        std::atomic<bool> accepted{false};
        std::atomic<bool> connected{false};
        std::optional<tcp_stream> server_stream;
        std::optional<tcp_stream> client_stream;
        
        scheduler sched(2);
        sched.start();
        
        auto accept_coro = [&]() -> task<void> {
            auto stream = co_await listener->accept();
            server_stream = std::move(stream);
            accepted = true;
            co_return;
        };

        auto connect_coro = [&]() -> task<void> {
            auto stream = co_await tcp_connect(ipv6_address("::1", port));
            client_stream = std::move(stream);
            connected = true;
            co_return;
        };
        
        sched.go(accept_coro);
        sched.go(connect_coro);
        
        for (int i = 0; i < 200 && (!accepted || !connected); ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        
        sched.shutdown();
        
        REQUIRE(accepted);
        REQUIRE(connected);
        REQUIRE(server_stream.has_value());
        REQUIRE(client_stream.has_value());
    }
}

TEST_CASE("tcp_stream read_exactly/write_exactly", "[tcp][stream][exact]") {
    // Establish a loopback TCP pair on IPv6 (::1) so we exercise the real
    // async read/write path used by the exact-length helpers.
    auto listener = tcp_listener::bind(ipv6_address("::1", 0));
    REQUIRE(listener.has_value());

    uint16_t port = listener->local_address().port();
    REQUIRE(port > 0);

    std::optional<tcp_stream> server_stream;
    std::optional<tcp_stream> client_stream;
    std::atomic<int> setup_complete{0};

    scheduler sched(2);
    sched.start();

    auto accept_coro = [&]() -> task<void> {
        auto stream = co_await listener->accept();
        server_stream = std::move(stream);
        setup_complete++;
        co_return;
    };

    auto connect_coro = [&]() -> task<void> {
        auto stream = co_await tcp_connect(ipv6_address("::1", port));
        client_stream = std::move(stream);
        setup_complete++;
        co_return;
    };

    sched.go(accept_coro);
    sched.go(connect_coro);

    for (int i = 0; i < 200 && setup_complete < 2; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    REQUIRE(setup_complete == 2);
    REQUIRE(server_stream.has_value());
    REQUIRE(client_stream.has_value());

    SECTION("write_exactly then read_exactly transfers full payload") {
        // A payload larger than typical socket buffers so the kernel is
        // likely to split it across several read/write syscalls, exercising
        // the aggregation loop inside the exact helpers.
        constexpr size_t kSize = 256 * 1024;
        std::vector<char> src(kSize);
        for (size_t i = 0; i < kSize; ++i) {
            src[i] = static_cast<char>(i * 31 + 7);
        }
        std::vector<char> dst(kSize, 0);

        std::atomic<bool> write_done{false};
        std::atomic<bool> read_done{false};
        io_result write_result{};
        io_result read_result{};

        auto write_coro = [&]() -> task<void> {
            write_result = co_await client_stream->write_exactly(src.data(), src.size());
            write_done = true;
        };
        auto read_coro = [&]() -> task<void> {
            read_result = co_await server_stream->read_exactly(dst.data(), dst.size());
            read_done = true;
        };

        sched.go(write_coro);
        sched.go(read_coro);

        for (int i = 0; i < 500 && (!write_done || !read_done); ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }

        REQUIRE(write_done);
        REQUIRE(read_done);
        REQUIRE(write_result.success());
        REQUIRE(read_result.success());
        REQUIRE(write_result.bytes_transferred() == static_cast<int>(kSize));
        REQUIRE(read_result.bytes_transferred() == static_cast<int>(kSize));
        REQUIRE(dst == src);
    }

    SECTION("read_exactly aggregates several short writes") {
        // The writer emits the payload in small chunks with a brief pause
        // between them; a single read_exactly on the reader must gather all
        // of them before completing.
        const std::string payload = "The quick brown fox jumps over the lazy dog.";
        std::string received(payload.size(), '\0');

        std::atomic<bool> write_done{false};
        std::atomic<bool> read_done{false};
        io_result read_result{};

        auto write_coro = [&]() -> task<void> {
            size_t offset = 0;
            constexpr size_t chunk = 5;
            while (offset < payload.size()) {
                size_t n = std::min(chunk, payload.size() - offset);
                auto r = co_await server_stream->write(payload.data() + offset, n);
                if (r.result <= 0) break;
                offset += static_cast<size_t>(r.result);
                // Yield so the reader observes a partial buffer and loops.
                co_await elio::time::sleep_for(std::chrono::milliseconds(5));
            }
            write_done = true;
        };
        auto read_coro = [&]() -> task<void> {
            read_result = co_await client_stream->read_exactly(received.data(),
                                                               received.size());
            read_done = true;
        };

        sched.go(read_coro);
        sched.go(write_coro);

        for (int i = 0; i < 500 && (!write_done || !read_done); ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }

        REQUIRE(write_done);
        REQUIRE(read_done);
        REQUIRE(read_result.success());
        REQUIRE(read_result.bytes_transferred() == static_cast<int>(payload.size()));
        REQUIRE(received == payload);
    }

    SECTION("read_exactly returns -ENODATA on EOF before completion") {
        // The peer sends fewer bytes than requested, then closes. read_exactly
        // must surface the truncated stream as -ENODATA rather than a partial
        // success.
        const std::string partial = "half";

        std::atomic<bool> write_done{false};
        std::atomic<bool> read_done{false};
        io_result read_result{};
        char buffer[32] = {0};

        auto write_coro = [&]() -> task<void> {
            co_await server_stream->write(partial.data(), partial.size());
            co_await server_stream->close();
            write_done = true;
        };
        auto read_coro = [&]() -> task<void> {
            // Request more than will ever arrive.
            read_result = co_await client_stream->read_exactly(buffer, sizeof(buffer));
            read_done = true;
        };

        sched.go(read_coro);
        sched.go(write_coro);

        for (int i = 0; i < 500 && (!write_done || !read_done); ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }

        REQUIRE(write_done);
        REQUIRE(read_done);
        REQUIRE_FALSE(read_result.success());
        REQUIRE(read_result.result == -ENODATA);
    }

    SECTION("write_exactly/read_exactly with zero length are no-ops") {
        std::atomic<bool> done{false};
        io_result write_result{100, 0};
        io_result read_result{100, 0};

        auto coro = [&]() -> task<void> {
            write_result = co_await client_stream->write_exactly(nullptr, 0);
            read_result = co_await server_stream->read_exactly(nullptr, 0);
            done = true;
        };

        sched.go(coro);

        for (int i = 0; i < 200 && !done; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }

        REQUIRE(done);
        REQUIRE(write_result.result == 0);
        REQUIRE(read_result.result == 0);
    }

    sched.shutdown();
}

TEST_CASE("net::stream read_exactly/write_exactly delegate to TCP",
          "[tcp][stream][exact][net_stream]") {
    // Wrap plain TCP endpoints in the unified net::stream and confirm the
    // exact-length helpers (and the write_all alias) delegate correctly.
    auto listener = tcp_listener::bind(ipv6_address("::1", 0));
    REQUIRE(listener.has_value());

    uint16_t port = listener->local_address().port();
    REQUIRE(port > 0);

    std::optional<tcp_stream> raw_server;
    std::optional<tcp_stream> raw_client;
    std::atomic<int> setup_complete{0};

    scheduler sched(2);
    sched.start();

    auto accept_coro = [&]() -> task<void> {
        auto s = co_await listener->accept();
        raw_server = std::move(s);
        setup_complete++;
        co_return;
    };
    auto connect_coro = [&]() -> task<void> {
        auto s = co_await tcp_connect(ipv6_address("::1", port));
        raw_client = std::move(s);
        setup_complete++;
        co_return;
    };

    sched.go(accept_coro);
    sched.go(connect_coro);

    for (int i = 0; i < 200 && setup_complete < 2; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    REQUIRE(setup_complete == 2);
    REQUIRE(raw_server.has_value());
    REQUIRE(raw_client.has_value());

    elio::net::stream server_stream(std::move(*raw_server));
    elio::net::stream client_stream(std::move(*raw_client));

    SECTION("write_exactly and read_exactly round-trip") {
        const std::string payload = "unified stream exact transfer";
        std::string received(payload.size(), '\0');

        std::atomic<bool> write_done{false};
        std::atomic<bool> read_done{false};
        io_result write_result{};
        io_result read_result{};

        auto write_coro = [&]() -> task<void> {
            write_result = co_await client_stream.write_exactly(payload);
            write_done = true;
        };
        auto read_coro = [&]() -> task<void> {
            read_result = co_await server_stream.read_exactly(received.data(),
                                                              received.size());
            read_done = true;
        };

        sched.go(write_coro);
        sched.go(read_coro);

        for (int i = 0; i < 500 && (!write_done || !read_done); ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }

        REQUIRE(write_done);
        REQUIRE(read_done);
        REQUIRE(write_result.success());
        REQUIRE(read_result.success());
        REQUIRE(read_result.bytes_transferred() == static_cast<int>(payload.size()));
        REQUIRE(received == payload);
    }

    SECTION("write_all remains a compatibility alias for write_exactly") {
        const std::string payload = "legacy write_all alias";
        std::string received(payload.size(), '\0');

        std::atomic<bool> write_done{false};
        std::atomic<bool> read_done{false};
        io_result write_result{};
        io_result read_result{};

        auto write_coro = [&]() -> task<void> {
            write_result = co_await client_stream.write_all(payload);
            write_done = true;
        };
        auto read_coro = [&]() -> task<void> {
            read_result = co_await server_stream.read_exactly(received.data(),
                                                              received.size());
            read_done = true;
        };

        sched.go(write_coro);
        sched.go(read_coro);

        for (int i = 0; i < 500 && (!write_done || !read_done); ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }

        REQUIRE(write_done);
        REQUIRE(read_done);
        REQUIRE(write_result.success());
        REQUIRE(write_result.bytes_transferred() == static_cast<int>(payload.size()));
        REQUIRE(received == payload);
    }

    sched.shutdown();
}

TEST_CASE("net::stream exact helpers on disconnected stream return -ENOTCONN",
          "[tcp][stream][exact][net_stream]") {
    elio::net::stream disconnected;

    std::atomic<bool> done{false};
    io_result read_result{};
    io_result write_result{};
    char buffer[8] = {0};

    scheduler sched(1);
    sched.start();

    auto coro = [&]() -> task<void> {
        read_result = co_await disconnected.read_exactly(buffer, sizeof(buffer));
        write_result = co_await disconnected.write_exactly("data", 4);
        done = true;
    };

    sched.go(coro);

    for (int i = 0; i < 200 && !done; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    sched.shutdown();

    REQUIRE(done);
    REQUIRE(read_result.result == -ENOTCONN);
    REQUIRE(write_result.result == -ENOTCONN);
}

#if !defined(ELIO_HAS_TLS) || !ELIO_HAS_TLS
TEST_CASE("net::connect secure request without TLS reports unsupported",
          "[tcp][stream][connect][net_stream]") {
    std::optional<elio::net::stream> result;
    std::atomic<bool> done{false};
    int observed_errno = 0;

    scheduler sched(1);
    sched.start();

    auto coro = [&]() -> task<void> {
        errno = 0;
        result = co_await elio::net::connect("localhost", 443, true, nullptr);
        observed_errno = errno;
        done = true;
    };

    sched.go(coro);

    for (int i = 0; i < 200 && !done; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    sched.shutdown();

    REQUIRE(done);
    REQUIRE_FALSE(result.has_value());
    REQUIRE(observed_errno == ENOTSUP);
}
#endif

TEST_CASE("TCP connect regression avoids double connect", "[tcp][connect][regression]") {
    auto listener = tcp_listener::bind(ipv6_address("::1", 0));
    REQUIRE(listener.has_value());

    uint16_t port = listener->local_address().port();
    REQUIRE(port > 0);

    std::atomic<int> connected{0};
    std::atomic<int> failed{0};
    std::atomic<int> first_error{0};

    scheduler sched(2);
    sched.start();

    constexpr int kAttempts = 64;
    for (int i = 0; i < kAttempts; ++i) {
        sched.go([&]() { return tcp_connect_regression_attempt(port, connected, failed, first_error); });
    }

    for (int i = 0; i < 500 && (connected + failed) < kAttempts; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    sched.shutdown();

    INFO("connect failures=" << failed.load() << ", first errno=" << first_error.load());
    REQUIRE(connected == kAttempts);
    REQUIRE(failed == 0);
}

TEST_CASE("tcp_connect braced options remain source-compatible",
          "[tcp][connect][api]") {
    [[maybe_unused]] auto v4 =
        tcp_connect(ipv4_address("127.0.0.1", 9), {});
    [[maybe_unused]] auto v6 =
        tcp_connect(ipv6_address("::1", 9), {});
    [[maybe_unused]] auto generic =
        tcp_connect(socket_address(ipv4_address("127.0.0.1", 9)), {});
    SUCCEED("braced tcp_options overloads remain unambiguous");
}

TEST_CASE("tcp_connect applies client socket options",
          "[tcp][connect][options]") {
    auto listener = tcp_listener::bind(ipv6_address("::1", 0));
    REQUIRE(listener.has_value());

    const uint16_t port = listener->local_address().port();
    REQUIRE(port > 0);

    std::atomic<int> accepted{0};
    tcp_connect_option_observations observed;

    scheduler sched(2);
    sched.start();

    sched.go([&]() { return accept_n_connections(*listener, 1, accepted); });
    sched.go([&]() { return tcp_connect_with_options_attempt(port, observed); });

    for (int i = 0; i < 300 &&
            (!observed.done.load(std::memory_order_acquire) ||
             accepted.load(std::memory_order_relaxed) < 1); ++i) {
        std::this_thread::sleep_for(elio::test::scaled_ms(10));
    }

    REQUIRE(sched.shutdown(elio::test::scaled_ms(5000)));

    INFO("connect errno=" << observed.first_error.load(std::memory_order_relaxed));
    REQUIRE(observed.done.load(std::memory_order_acquire));
    REQUIRE(accepted.load(std::memory_order_relaxed) == 1);
    REQUIRE(observed.first_error.load(std::memory_order_relaxed) == 0);
    REQUIRE(observed.no_delay.load(std::memory_order_relaxed) != 0);
    REQUIRE(observed.keep_alive.load(std::memory_order_relaxed) != 0);
    REQUIRE(observed.reuse_addr.load(std::memory_order_relaxed) != 0);
    REQUIRE(observed.reuse_port.load(std::memory_order_relaxed) != 0);
    REQUIRE(observed.recv_buffer.load(std::memory_order_relaxed) >= 4096);
    REQUIRE(observed.send_buffer.load(std::memory_order_relaxed) >= 8192);
}

TEST_CASE("tcp_connect observes already-cancelled token",
          "[tcp][connect][cancel]") {
    std::optional<tcp_stream> stream;
    std::atomic<bool> done{false};
    int observed_errno = 0;

    scheduler sched(1);
    sched.start();

    cancel_source source;
    source.cancel();

    sched.go([&]() -> task<void> {
        errno = 0;
        stream = co_await tcp_connect(
            ipv4_address("127.0.0.1", 9), source.get_token());
        observed_errno = errno;
        done = true;
        co_return;
    });

    for (int i = 0; i < 200 && !done; ++i) {
        std::this_thread::sleep_for(elio::test::scaled_ms(10));
    }

    REQUIRE(sched.shutdown(elio::test::scaled_ms(5000)));

    REQUIRE(done);
    REQUIRE_FALSE(stream.has_value());
    REQUIRE(observed_errno == ECANCELED);
}

TEST_CASE("tcp_connect cancellation during pending connect cleans up fd",
          "[tcp][connect][cancel]") {
    std::optional<tcp_stream> stream;
    std::atomic<bool> started{false};
    std::atomic<bool> done{false};
    int observed_errno = 0;

    scheduler sched(1);
    sched.start();

    cancel_source source;
    sched.go([&]() -> task<void> {
        started = true;
        errno = 0;
        stream = co_await tcp_connect(
            ipv4_address("192.0.2.1", 9), source.get_token());
        observed_errno = errno;
        done = true;
        co_return;
    });

    for (int i = 0; i < 200 && !started; ++i) {
        std::this_thread::sleep_for(elio::test::scaled_ms(10));
    }
    REQUIRE(started);

    std::this_thread::sleep_for(elio::test::scaled_ms(50));
    if (done) {
        REQUIRE(sched.shutdown(elio::test::scaled_ms(5000)));
        SUCCEED("platform completed the connect before cancellation");
        return;
    }

    const int pending_fds = count_open_fds_for_tcp_test();
    source.cancel();

    for (int i = 0; i < 500 && !done; ++i) {
        std::this_thread::sleep_for(elio::test::scaled_ms(10));
    }

    REQUIRE(sched.shutdown(elio::test::scaled_ms(5000)));

    REQUIRE(done);
    REQUIRE_FALSE(stream.has_value());
    REQUIRE(observed_errno == ECANCELED);

    const int after_fds = count_open_fds_for_tcp_test();
    if (pending_fds >= 0 && after_fds >= 0) {
        REQUIRE(after_fds < pending_fds);
    }
}

TEST_CASE("tcp_connect completion wins over late cancellation",
          "[tcp][connect][cancel][regression]") {
    auto listener = tcp_listener::bind(ipv6_address("::1", 0));
    REQUIRE(listener.has_value());

    cancel_source source;
    auto connect_op = tcp_connect(
        ipv6_address("::1", listener->local_address().port()),
        source.get_token());

    const bool suspended = connect_op.await_suspend(std::noop_coroutine());
    if (suspended) {
        default_io_context().run_for(elio::test::scaled_ms(1000));
        REQUIRE_FALSE(default_io_context().has_pending());
    }

    source.cancel();

    errno = 0;
    auto stream = connect_op.await_resume();

    INFO("late-cancel errno=" << errno);
    REQUIRE(stream.has_value());
    REQUIRE(stream->is_valid());
}

TEST_CASE("explicit hostname resolution", "[tcp][address][dns]") {
    SECTION("localhost resolves asynchronously") {
        scheduler sched(1);
        sched.start();

        std::optional<socket_address> resolved;
        std::atomic<bool> done{false};

        sched.go([&]() { return resolve_hostname_attempt("localhost", 80, resolved, done); });

        for (int i = 0; i < 200 && !done.load(std::memory_order_relaxed); ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }

        sched.shutdown();

        REQUIRE(done.load(std::memory_order_relaxed));
        REQUIRE(resolved.has_value());
        REQUIRE((resolved->is_v4() || resolved->is_v6()));
        REQUIRE(resolved->port() == 80);
    }
}

TEST_CASE("tcp_connect hostname resolution uses cache", "[tcp][connect][dns][cache]") {
    default_resolve_cache().clear();

    auto listener = tcp_listener::bind(socket_address(0));
    REQUIRE(listener.has_value());

    const uint16_t port = listener->local_address().port();
    REQUIRE(port > 0);

    std::atomic<int> accepted{0};
    std::atomic<int> connected{0};
    std::atomic<int> failed{0};
    std::atomic<int> first_error{0};

    scheduler sched(2);
    sched.start();

    auto stats_before = default_resolve_cache().stats();

    sched.go([&]() { return accept_n_connections(*listener, 2, accepted); });

    sched.go([&]() { return tcp_connect_hostname_attempt("localhost", port, connected, failed, first_error); });

    for (int i = 0; i < 300 && connected.load(std::memory_order_relaxed) < 1; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    auto stats_after_first = default_resolve_cache().stats();

    sched.go([&]() { return tcp_connect_hostname_attempt("localhost", port, connected, failed, first_error); });

    for (int i = 0; i < 300 && (accepted.load(std::memory_order_relaxed) < 2
            || connected.load(std::memory_order_relaxed) < 2
            || failed.load(std::memory_order_relaxed) != 0); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    sched.shutdown();

    auto stats_after_second = default_resolve_cache().stats();

    INFO("connected=" << connected.load() << ", failed=" << failed.load()
         << ", first errno=" << first_error.load());
    INFO("before stats: hits=" << stats_before.cache_hits
         << ", misses=" << stats_before.cache_misses
         << ", stores=" << stats_before.cache_stores
         << ", invalidations=" << stats_before.cache_invalidations);
    INFO("first stats: hits=" << stats_after_first.cache_hits
         << ", misses=" << stats_after_first.cache_misses
         << ", stores=" << stats_after_first.cache_stores
         << ", invalidations=" << stats_after_first.cache_invalidations);
    INFO("second stats: hits=" << stats_after_second.cache_hits
         << ", misses=" << stats_after_second.cache_misses
         << ", stores=" << stats_after_second.cache_stores
         << ", invalidations=" << stats_after_second.cache_invalidations);

    REQUIRE(accepted == 2);
    REQUIRE(connected == 2);
    REQUIRE(failed == 0);
    REQUIRE(stats_after_first.cache_misses >= (stats_before.cache_misses + 1));
    REQUIRE(stats_after_first.cache_stores >= (stats_before.cache_stores + 1));
    REQUIRE(stats_after_second.cache_hits >= (stats_after_first.cache_hits + 1));
    REQUIRE(stats_after_second.cache_misses == stats_after_first.cache_misses);
}

TEST_CASE("resolve_options can disable cache", "[tcp][dns][cache][config]") {
    default_resolve_cache().clear();
    auto stats_before = default_resolve_cache().stats();

    resolve_options options;
    options.use_cache = false;

    scheduler sched(1);
    sched.start();

    std::vector<socket_address> resolved_first;
    std::vector<socket_address> resolved_second;
    std::atomic<bool> done_first{false};
    std::atomic<bool> done_second{false};

    sched.go([&]() { return resolve_all_attempt_with_options(
        "localhost", 80, options, resolved_first, done_first); });

    for (int i = 0; i < 200 && !done_first.load(std::memory_order_relaxed); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    sched.go([&]() { return resolve_all_attempt_with_options(
        "localhost", 80, options, resolved_second, done_second); });

    for (int i = 0; i < 200 && !done_second.load(std::memory_order_relaxed); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    sched.shutdown();

    auto stats_after = default_resolve_cache().stats();
    REQUIRE(done_first.load(std::memory_order_relaxed));
    REQUIRE(done_second.load(std::memory_order_relaxed));
    REQUIRE_FALSE(resolved_first.empty());
    REQUIRE_FALSE(resolved_second.empty());
    REQUIRE(stats_after.cache_hits == stats_before.cache_hits);
    REQUIRE(stats_after.cache_misses == stats_before.cache_misses);
    REQUIRE(stats_after.cache_stores == stats_before.cache_stores);
}

TEST_CASE("resolve_options can use custom cache instance", "[tcp][dns][cache][config]") {
    default_resolve_cache().clear();
    auto default_before = default_resolve_cache().stats();

    resolve_cache custom_cache;
    resolve_options options;
    options.use_cache = true;
    options.cache = &custom_cache;

    scheduler sched(1);
    sched.start();

    std::vector<socket_address> resolved_first;
    std::vector<socket_address> resolved_second;
    std::atomic<bool> done_first{false};
    std::atomic<bool> done_second{false};

    sched.go([&]() { return resolve_all_attempt_with_options(
        "localhost", 80, options, resolved_first, done_first); });

    for (int i = 0; i < 200 && !done_first.load(std::memory_order_relaxed); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    sched.go([&]() { return resolve_all_attempt_with_options(
        "localhost", 80, options, resolved_second, done_second); });

    for (int i = 0; i < 200 && !done_second.load(std::memory_order_relaxed); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    sched.shutdown();

    auto custom_after = custom_cache.stats();
    auto default_after = default_resolve_cache().stats();

    REQUIRE(done_first.load(std::memory_order_relaxed));
    REQUIRE(done_second.load(std::memory_order_relaxed));
    REQUIRE_FALSE(resolved_first.empty());
    REQUIRE_FALSE(resolved_second.empty());
    REQUIRE(custom_after.cache_misses >= 1);
    REQUIRE(custom_after.cache_stores >= 1);
    REQUIRE(custom_after.cache_hits >= 1);
    REQUIRE(default_after.cache_hits == default_before.cache_hits);
    REQUIRE(default_after.cache_misses == default_before.cache_misses);
    REQUIRE(default_after.cache_stores == default_before.cache_stores);
}

TEST_CASE("resolve_options ttl controls cache expiry", "[tcp][dns][cache][config]") {
    resolve_cache cache;
    resolve_options options;
    options.use_cache = true;
    options.cache = &cache;
    options.positive_ttl = std::chrono::seconds(0);

    scheduler sched(1);
    sched.start();

    std::vector<socket_address> resolved_first;
    std::vector<socket_address> resolved_second;
    std::atomic<bool> done_first{false};
    std::atomic<bool> done_second{false};

    sched.go([&]() { return resolve_all_attempt_with_options(
        "localhost", 80, options, resolved_first, done_first); });

    for (int i = 0; i < 200 && !done_first.load(std::memory_order_relaxed); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    sched.go([&]() { return resolve_all_attempt_with_options(
        "localhost", 80, options, resolved_second, done_second); });

    for (int i = 0; i < 200 && !done_second.load(std::memory_order_relaxed); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    sched.shutdown();

    auto stats = cache.stats();
    REQUIRE(done_first.load(std::memory_order_relaxed));
    REQUIRE(done_second.load(std::memory_order_relaxed));
    REQUIRE_FALSE(resolved_first.empty());
    REQUIRE_FALSE(resolved_second.empty());
    REQUIRE(stats.cache_misses >= 2);
    REQUIRE(stats.cache_hits == 0);
}

// ============================================================================
// Cancellable I/O tests
// ============================================================================

namespace {

template<typename Predicate>
bool wait_for_io_cancel_test(Predicate&& predicate,
                             std::chrono::milliseconds timeout =
                                 elio::test::scaled_ms(2000)) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (!predicate() && std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(elio::test::scaled_ms(10));
    }
    return predicate();
}

void fill_socket_send_buffer(int fd) {
    int sndbuf = 4096;
    (void)setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));

    std::array<char, 4096> chunk{};
    size_t total_written = 0;
    for (int i = 0; i < 10000; ++i) {
        ssize_t n = send(fd, chunk.data(), chunk.size(), MSG_DONTWAIT);
        if (n > 0) {
            total_written += static_cast<size_t>(n);
            continue;
        }
        REQUIRE(n == -1);
        REQUIRE((errno == EAGAIN || errno == EWOULDBLOCK));
        REQUIRE(total_written > 0);
        return;
    }

    FAIL("socket send buffer did not fill");
}

#if defined(ELIO_HAS_TLS) && ELIO_HAS_TLS
bool install_test_certificate(elio::tls::tls_context& ctx) {
    using key_ctx_ptr = std::unique_ptr<EVP_PKEY_CTX, decltype(&EVP_PKEY_CTX_free)>;
    using key_ptr = std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)>;
    using cert_ptr = std::unique_ptr<X509, decltype(&X509_free)>;

    key_ctx_ptr key_ctx(EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, nullptr),
                        EVP_PKEY_CTX_free);
    if (!key_ctx) {
        return false;
    }
    if (EVP_PKEY_keygen_init(key_ctx.get()) <= 0 ||
        EVP_PKEY_CTX_set_rsa_keygen_bits(key_ctx.get(), 2048) <= 0) {
        return false;
    }

    EVP_PKEY* raw_key = nullptr;
    if (EVP_PKEY_keygen(key_ctx.get(), &raw_key) <= 0) {
        return false;
    }
    key_ptr pkey(raw_key, EVP_PKEY_free);

    cert_ptr cert(X509_new(), X509_free);
    if (!cert) {
        return false;
    }

    if (X509_set_version(cert.get(), 2) != 1 ||
        ASN1_INTEGER_set(X509_get_serialNumber(cert.get()), 1) != 1 ||
        X509_gmtime_adj(X509_getm_notBefore(cert.get()), 0) == nullptr ||
        X509_gmtime_adj(X509_getm_notAfter(cert.get()), 3600) == nullptr ||
        X509_set_pubkey(cert.get(), pkey.get()) != 1) {
        return false;
    }

    auto* name = X509_get_subject_name(cert.get());
    if (!name ||
        X509_NAME_add_entry_by_txt(
            name, "CN", MBSTRING_ASC,
            reinterpret_cast<const unsigned char*>("localhost"),
            -1, -1, 0) != 1 ||
        X509_set_issuer_name(cert.get(), name) != 1 ||
        X509_sign(cert.get(), pkey.get(), EVP_sha256()) <= 0) {
        return false;
    }

    return SSL_CTX_use_certificate(ctx.native_handle(), cert.get()) == 1 &&
           SSL_CTX_use_PrivateKey(ctx.native_handle(), pkey.get()) == 1 &&
           SSL_CTX_check_private_key(ctx.native_handle()) == 1;
}
#endif

} // namespace

TEST_CASE("Cancellable recv completes normally", "[io][cancel]") {
    int sv[2];
    REQUIRE(socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0, sv) == 0);

    const char* msg = "cancel test data";
    ssize_t sent = send(sv[0], msg, strlen(msg), 0);
    REQUIRE(sent == static_cast<ssize_t>(strlen(msg)));

    char buffer[64] = {0};
    std::atomic<bool> completed{false};
    cancellable_io_result recv_result{};

    scheduler sched(1);
    sched.start();

    cancel_source source;

    auto recv_coro = [&]() -> task<void> {
        auto result = co_await async_recv(sv[1], buffer, sizeof(buffer) - 1, 0,
                                          source.get_token());
        recv_result = result;
        completed = true;
        co_return;
    };

    sched.go(recv_coro);

    for (int i = 0; i < 100 && !completed; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    sched.shutdown();

    REQUIRE(completed);
    REQUIRE(recv_result.success());
    REQUIRE_FALSE(recv_result.was_cancelled());
    REQUIRE(recv_result.bytes_transferred() == static_cast<int>(strlen(msg)));
    REQUIRE(std::string(buffer) == msg);

    close(sv[0]);
    close(sv[1]);
}

TEST_CASE("Cancellable recv completion wins over late cancellation",
          "[io][cancel][regression]") {
    int sv[2];
    REQUIRE(socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0, sv) == 0);

    const char* msg = "late cancel data";
    REQUIRE(send(sv[0], msg, strlen(msg), 0) ==
            static_cast<ssize_t>(strlen(msg)));

    char buffer[64] = {};
    cancel_source source;
    auto recv_op = async_recv(sv[1], buffer, sizeof(buffer) - 1, 0,
                              source.get_token());

    REQUIRE_FALSE(recv_op.await_ready());
    recv_op.await_suspend(std::noop_coroutine());
    default_io_context().run_for(elio::test::scaled_ms(1000));
    REQUIRE_FALSE(default_io_context().has_pending());

    source.cancel();

    auto recv_result = recv_op.await_resume();

    REQUIRE(recv_result.success());
    REQUIRE_FALSE(recv_result.was_cancelled());
    REQUIRE(recv_result.bytes_transferred() == static_cast<int>(strlen(msg)));
    REQUIRE(std::string(buffer) == msg);

    close(sv[0]);
    close(sv[1]);
}

TEST_CASE("Cancellable recv already cancelled", "[io][cancel]") {
    int sv[2];
    REQUIRE(socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0, sv) == 0);

    char buffer[64] = {0};
    std::atomic<bool> completed{false};
    cancellable_io_result recv_result{};

    scheduler sched(1);
    sched.start();

    cancel_source source;
    source.cancel();

    auto recv_coro = [&]() -> task<void> {
        auto result = co_await async_recv(sv[1], buffer, sizeof(buffer) - 1, 0,
                                          source.get_token());
        recv_result = result;
        completed = true;
        co_return;
    };

    sched.go(recv_coro);

    for (int i = 0; i < 100 && !completed; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    sched.shutdown();

    REQUIRE(completed);
    REQUIRE(recv_result.was_cancelled());
    REQUIRE_FALSE(recv_result.success());
    REQUIRE(recv_result.error_code() == ECANCELED);

    close(sv[0]);
    close(sv[1]);
}

TEST_CASE("Cancellable recv cancelled during wait",
          "[io][cancel][epoll-cancel-regression]") {
    int sv[2];
    REQUIRE(socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0, sv) == 0);

    char buffer[64] = {0};
    std::atomic<bool> started{false};
    std::atomic<bool> completed{false};
    cancellable_io_result recv_result{};

    scheduler sched(1);
    sched.start();

    cancel_source source;

    auto recv_coro = [&]() -> task<void> {
        started = true;
        auto result = co_await async_recv(sv[1], buffer, sizeof(buffer) - 1, 0,
                                          source.get_token());
        recv_result = result;
        completed = true;
        co_return;
    };

    sched.go(recv_coro);

    REQUIRE(wait_for_io_cancel_test([&] { return started.load(); }));

    std::this_thread::sleep_for(elio::test::scaled_ms(50));
    source.cancel();

    REQUIRE(wait_for_io_cancel_test([&] { return completed.load(); }));

    REQUIRE(sched.shutdown(elio::test::scaled_ms(5000)));

    REQUIRE(completed);
    REQUIRE(recv_result.was_cancelled());
    REQUIRE_FALSE(recv_result.success());
    REQUIRE(recv_result.error_code() == ECANCELED);

    close(sv[0]);
    close(sv[1]);
}

TEST_CASE("Cancellable poll_read cancelled during wait without readiness",
          "[io][cancel][poll][poll-cancel-regression]") {
    int efd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    REQUIRE(efd >= 0);

    std::atomic<bool> started{false};
    std::atomic<bool> completed{false};
    cancellable_io_result poll_result{};

    scheduler sched(1);
    sched.start();

    cancel_source source;

    auto poll_coro = [&]() -> task<void> {
        started = true;
        auto result = co_await async_poll_read(efd, source.get_token());
        poll_result = result;
        completed = true;
        co_return;
    };

    sched.go(poll_coro);

    for (int i = 0; i < 100 && !started; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    REQUIRE(started);

    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    REQUIRE_FALSE(completed);

    source.cancel();

    for (int i = 0; i < 200 && !completed; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    REQUIRE(sched.shutdown(std::chrono::milliseconds(5000)));

    REQUIRE(completed);
    REQUIRE(poll_result.was_cancelled());
    REQUIRE_FALSE(poll_result.success());
    REQUIRE(poll_result.error_code() == ECANCELED);

    std::uint64_t value = 0;
    errno = 0;
    REQUIRE(::read(efd, &value, sizeof(value)) == -1);
    REQUIRE(errno == EAGAIN);

    close(efd);
}

TEST_CASE("Cancellable poll_read readiness wins over late cancellation",
          "[io][cancel][poll][regression]") {
    int efd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    REQUIRE(efd >= 0);

    std::uint64_t value = 1;
    REQUIRE(::write(efd, &value, sizeof(value)) ==
            static_cast<ssize_t>(sizeof(value)));

    cancel_source source;
    auto poll_op = async_poll_read(efd, source.get_token());

    REQUIRE_FALSE(poll_op.await_ready());
    poll_op.await_suspend(std::noop_coroutine());
    default_io_context().run_for(elio::test::scaled_ms(1000));
    REQUIRE_FALSE(default_io_context().has_pending());

    source.cancel();

    auto poll_result = poll_op.await_resume();

    REQUIRE(poll_result.success());
    REQUIRE_FALSE(poll_result.was_cancelled());
    REQUIRE(poll_result.error_code() == 0);

    REQUIRE(::read(efd, &value, sizeof(value)) ==
            static_cast<ssize_t>(sizeof(value)));
    close(efd);
}

TEST_CASE("Cancellable poll_write cancelled during wait without writability",
          "[io][cancel][poll][poll-cancel-regression]") {
    int efd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    REQUIRE(efd >= 0);

    std::uint64_t saturated =
        std::numeric_limits<std::uint64_t>::max() - 1;
    REQUIRE(::write(efd, &saturated, sizeof(saturated)) ==
            static_cast<ssize_t>(sizeof(saturated)));

    std::atomic<bool> started{false};
    std::atomic<bool> completed{false};
    cancellable_io_result poll_result{};

    scheduler sched(1);
    sched.start();

    cancel_source source;

    auto poll_coro = [&]() -> task<void> {
        started = true;
        auto result = co_await async_poll_write(efd, source.get_token());
        poll_result = result;
        completed = true;
        co_return;
    };

    sched.go(poll_coro);

    REQUIRE(wait_for_io_cancel_test([&] { return started.load(); }));

    std::this_thread::sleep_for(elio::test::scaled_ms(50));
    REQUIRE_FALSE(completed);

    source.cancel();

    REQUIRE(wait_for_io_cancel_test([&] { return completed.load(); }));
    REQUIRE(sched.shutdown(elio::test::scaled_ms(5000)));

    REQUIRE(poll_result.was_cancelled());
    REQUIRE_FALSE(poll_result.success());
    REQUIRE(poll_result.error_code() == ECANCELED);

    close(efd);
}

TEST_CASE("Cancellable send completes normally", "[io][cancel]") {
    int sv[2];
    REQUIRE(socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0, sv) == 0);

    const char* msg = "cancel send test";
    std::atomic<bool> completed{false};
    cancellable_io_result send_result{};

    scheduler sched(1);
    sched.start();

    cancel_source source;

    auto send_coro = [&]() -> task<void> {
        auto result = co_await async_send(sv[0], msg, strlen(msg), 0,
                                          source.get_token());
        send_result = result;
        completed = true;
        co_return;
    };

    sched.go(send_coro);

    for (int i = 0; i < 100 && !completed; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    sched.shutdown();

    REQUIRE(completed);
    REQUIRE(send_result.success());
    REQUIRE_FALSE(send_result.was_cancelled());
    REQUIRE(send_result.bytes_transferred() == static_cast<int>(strlen(msg)));

    char buffer[64] = {0};
    ssize_t received = recv(sv[1], buffer, sizeof(buffer) - 1, 0);
    REQUIRE(received == static_cast<ssize_t>(strlen(msg)));
    REQUIRE(std::string(buffer) == msg);

    close(sv[0]);
    close(sv[1]);
}

TEST_CASE("Cancellable send already cancelled", "[io][cancel]") {
    int sv[2];
    REQUIRE(socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0, sv) == 0);

    const char* msg = "should not send";
    std::atomic<bool> completed{false};
    cancellable_io_result send_result{};

    scheduler sched(1);
    sched.start();

    cancel_source source;
    source.cancel();

    auto send_coro = [&]() -> task<void> {
        auto result = co_await async_send(sv[0], msg, strlen(msg), 0,
                                          source.get_token());
        send_result = result;
        completed = true;
        co_return;
    };

    sched.go(send_coro);

    for (int i = 0; i < 100 && !completed; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    sched.shutdown();

    REQUIRE(completed);
    REQUIRE(send_result.was_cancelled());
    REQUIRE_FALSE(send_result.success());
    REQUIRE(send_result.error_code() == ECANCELED);

    close(sv[0]);
    close(sv[1]);
}

TEST_CASE("Cancellable send cancelled during wait without writability",
          "[io][cancel][epoll-cancel-regression]") {
    int sv[2];
    REQUIRE(socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0, sv) == 0);
    fill_socket_send_buffer(sv[0]);

    const char byte = 'x';
    std::atomic<bool> started{false};
    std::atomic<bool> completed{false};
    cancellable_io_result send_result{};

    scheduler sched(1);
    sched.start();

    cancel_source source;

    auto send_coro = [&]() -> task<void> {
        started = true;
        auto result = co_await async_send(sv[0], &byte, sizeof(byte), 0,
                                          source.get_token());
        send_result = result;
        completed = true;
        co_return;
    };

    sched.go(send_coro);

    REQUIRE(wait_for_io_cancel_test([&] { return started.load(); }));

    std::this_thread::sleep_for(elio::test::scaled_ms(50));
    REQUIRE_FALSE(completed);

    source.cancel();

    REQUIRE(wait_for_io_cancel_test([&] { return completed.load(); }));
    REQUIRE(sched.shutdown(elio::test::scaled_ms(5000)));

    REQUIRE(send_result.was_cancelled());
    REQUIRE_FALSE(send_result.success());
    REQUIRE(send_result.error_code() == ECANCELED);

    close(sv[0]);
    close(sv[1]);
}

TEST_CASE("Cancellable connect already cancelled", "[io][cancel]") {
    int fd = -1;
    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(12345);
    inet_pton(AF_INET, "127.0.0.1", &addr.sin_addr);

    std::atomic<bool> completed{false};
    cancellable_io_result connect_result{};

    scheduler sched(1);
    sched.start();

    cancel_source source;
    source.cancel();

    auto connect_coro = [&]() -> task<void> {
        auto result = co_await async_connect(fd,
            reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr),
            source.get_token());
        connect_result = result;
        completed = true;
    };

    sched.go(connect_coro);

    for (int i = 0; i < 100 && !completed; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    sched.shutdown();

    REQUIRE(completed);
    REQUIRE(connect_result.was_cancelled());
    REQUIRE_FALSE(connect_result.success());
    REQUIRE(connect_result.error_code() == ECANCELED);
}

TEST_CASE("Cancellable connect cancelled during epoll wait",
          "[io][cancel][epoll-cancel-regression]") {
    int fd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    REQUIRE(fd >= 0);

    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(9);
    inet_pton(AF_INET, "192.0.2.1", &addr.sin_addr);

    std::atomic<bool> started{false};
    std::atomic<bool> completed{false};
    std::atomic<bool> used_epoll{false};
    cancellable_io_result connect_result{};

    scheduler sched(1);
    sched.start();

    cancel_source source;

    auto connect_coro = [&]() -> task<void> {
        if (current_io_context().get_backend_type() !=
            io_context::backend_type::epoll) {
            completed = true;
            co_return;
        }
        used_epoll = true;
        started = true;
        auto result = co_await async_connect(
            fd, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr),
            source.get_token());
        connect_result = result;
        completed = true;
        co_return;
    };

    sched.go(connect_coro);

    REQUIRE(wait_for_io_cancel_test([&] { return completed.load() || started.load(); }));

    if (!used_epoll) {
        REQUIRE(sched.shutdown(elio::test::scaled_ms(5000)));
        SUCCEED("connect epoll cancellation regression is skipped on io_uring backend");
        close(fd);
        return;
    }

    if (completed) {
        REQUIRE(sched.shutdown(elio::test::scaled_ms(5000)));
        SUCCEED("connect completed before cancellation could be exercised");
        close(fd);
        return;
    }

    std::this_thread::sleep_for(elio::test::scaled_ms(50));
    if (completed) {
        REQUIRE(sched.shutdown(elio::test::scaled_ms(5000)));
        SUCCEED("connect completed before cancellation could be exercised");
        close(fd);
        return;
    }

    source.cancel();

    REQUIRE(wait_for_io_cancel_test([&] { return completed.load(); }));
    REQUIRE(sched.shutdown(elio::test::scaled_ms(5000)));

    REQUIRE(connect_result.was_cancelled());
    REQUIRE_FALSE(connect_result.success());
    REQUIRE(connect_result.error_code() == ECANCELED);

    close(fd);
}

#if defined(ELIO_HAS_TLS) && ELIO_HAS_TLS
TEST_CASE("TLS shutdown timeout cancels silent peer readiness wait",
          "[tls][shutdown][timeout][poll-cancel-regression]") {
    int sv[2];
    REQUIRE(socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0, sv) == 0);

    elio::tls::tls_context server_ctx(elio::tls::tls_mode::server);
    REQUIRE(install_test_certificate(server_ctx));

    elio::tls::tls_context client_ctx(elio::tls::tls_mode::client);
    client_ctx.set_verify_mode(elio::tls::verify_mode::none);

    const auto shutdown_timeout = elio::test::scaled_ms(100);
    const auto client_wait = shutdown_timeout + elio::test::scaled_ms(500);
    const auto server_hold = elio::test::scaled_ms(2000);

    std::atomic<bool> client_handshake{false};
    std::atomic<bool> server_handshake{false};
    std::atomic<bool> client_done{false};
    std::atomic<bool> server_done{false};
    std::atomic<bool> release_server{false};
    std::atomic<int64_t> shutdown_elapsed_ms{-1};

    scheduler sched(2);
    sched.start();

    auto client_task = [&]() -> task<void> {
        {
            elio::tls::tls_stream client{elio::net::tcp_stream(sv[0]), client_ctx};
            bool ok = co_await client.handshake();
            client_handshake = ok;
            if (ok) {
                const auto start = std::chrono::steady_clock::now();
                co_await client.shutdown(shutdown_timeout);
                shutdown_elapsed_ms = std::chrono::duration_cast<
                    std::chrono::milliseconds>(
                        std::chrono::steady_clock::now() - start).count();
            }
            client_done = true;
            release_server = true;
            while (!server_done) {
                co_await elio::time::sleep_for(elio::test::scaled_ms(10));
            }
        }
        co_return;
    };

    auto server_task = [&]() -> task<void> {
        {
            elio::tls::tls_stream server{elio::net::tcp_stream(sv[1]), server_ctx};
            bool ok = co_await server.handshake();
            server_handshake = ok;

            const auto deadline = std::chrono::steady_clock::now() + server_hold;
            while (ok && !release_server &&
                   std::chrono::steady_clock::now() < deadline) {
                co_await elio::time::sleep_for(elio::test::scaled_ms(10));
            }
        }

        server_done = true;
        co_return;
    };

    sched.go(client_task);
    sched.go(server_task);

    const bool client_finished_before_peer_release =
        wait_for_io_cancel_test([&] { return client_done.load(); }, client_wait);
    release_server = true;

    REQUIRE(wait_for_io_cancel_test([&] { return server_done.load(); },
                                    elio::test::scaled_ms(5000)));
    REQUIRE(sched.shutdown(elio::test::scaled_ms(5000)));

    REQUIRE(client_handshake);
    REQUIRE(server_handshake);
    REQUIRE(client_finished_before_peer_release);
    REQUIRE(shutdown_elapsed_ms.load() >= 0);
    REQUIRE(shutdown_elapsed_ms.load() < server_hold.count());
}

TEST_CASE("tls_stream base read and write reject oversized lengths",
          "[tls][stream][overflow]") {
    int sv[2];
    REQUIRE(socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0, sv) == 0);

    elio::tls::tls_context client_ctx(elio::tls::tls_mode::client);
    client_ctx.set_verify_mode(elio::tls::verify_mode::none);
    elio::tls::tls_stream stream{elio::net::tcp_stream(sv[0]), client_ctx};

    close(sv[1]);

    constexpr size_t oversized = static_cast<size_t>(INT32_MAX) + 1;
    char byte = 0;
    io_result read_result{};
    io_result write_result{};
    io_result read_token_result{};
    io_result write_token_result{};
    std::atomic<bool> done{false};

    scheduler sched(1);
    sched.start();

    sched.go([&]() -> task<void> {
        cancel_source source;
        read_result = co_await stream.read(&byte, oversized);
        write_result = co_await stream.write(&byte, oversized);
        read_token_result =
            co_await stream.read(&byte, oversized, source.get_token());
        write_token_result =
            co_await stream.write(&byte, oversized, source.get_token());
        done = true;
        co_return;
    });

    for (int i = 0; i < 200 && !done; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    REQUIRE(sched.shutdown(elio::test::scaled_ms(5000)));

    REQUIRE(done);
    REQUIRE(read_result.result == -EOVERFLOW);
    REQUIRE(write_result.result == -EOVERFLOW);
    REQUIRE(read_token_result.result == -EOVERFLOW);
    REQUIRE(write_token_result.result == -EOVERFLOW);
}

TEST_CASE("tls_stream base zero-length read and write are no-ops",
          "[tls][stream][zero-length]") {
    int sv[2];
    REQUIRE(socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0, sv) == 0);

    elio::tls::tls_context client_ctx(elio::tls::tls_mode::client);
    client_ctx.set_verify_mode(elio::tls::verify_mode::none);
    elio::tls::tls_stream stream{elio::net::tcp_stream(sv[0]), client_ctx};

    close(sv[1]);

    cancel_source source;
    source.cancel();

    io_result read_result{-1, 0};
    io_result write_result{-1, 0};
    io_result read_token_result{-1, 0};
    io_result write_token_result{-1, 0};
    std::atomic<bool> done{false};

    scheduler sched(1);
    sched.start();

    sched.go([&]() -> task<void> {
        read_result = co_await stream.read(nullptr, 0);
        write_result = co_await stream.write(nullptr, 0);
        read_token_result = co_await stream.read(nullptr, 0, source.get_token());
        write_token_result =
            co_await stream.write(nullptr, 0, source.get_token());
        done = true;
        co_return;
    });

    for (int i = 0; i < 200 && !done; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    REQUIRE(sched.shutdown(elio::test::scaled_ms(5000)));

    REQUIRE(done);
    REQUIRE(read_result.result == 0);
    REQUIRE(write_result.result == 0);
    REQUIRE(read_token_result.result == 0);
    REQUIRE(write_token_result.result == 0);
}
#endif

// ============================================================================
// Regression tests for #247: tcp_stream read/write must await readiness and
// retry on transient -EAGAIN/-EWOULDBLOCK instead of surfacing them.
// ============================================================================

namespace {

// Establish a connected TCP loopback pair on ::1 with small socket buffers so
// that a large transfer is guaranteed to saturate the kernel send buffer and
// exercise the internal writability-retry loop. Returns the accepted (server)
// and connected (client) streams.
struct tcp_pair {
    std::optional<tcp_stream> server;
    std::optional<tcp_stream> client;
};

tcp_pair make_small_buffer_tcp_pair(scheduler& sched) {
    auto listener = tcp_listener::bind(ipv6_address("::1", 0));
    REQUIRE(listener.has_value());
    uint16_t port = listener->local_address().port();
    REQUIRE(port > 0);

    tcp_pair pair;
    std::atomic<bool> accepted{false};
    std::atomic<bool> connected{false};

    auto accept_coro = [&]() -> task<void> {
        auto stream = co_await listener->accept();
        pair.server = std::move(stream);
        accepted = true;
        co_return;
    };
    auto connect_coro = [&]() -> task<void> {
        auto stream = co_await tcp_connect(ipv6_address("::1", port));
        pair.client = std::move(stream);
        connected = true;
        co_return;
    };

    sched.go(accept_coro);
    sched.go(connect_coro);

    for (int i = 0; i < 300 && (!accepted || !connected); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    REQUIRE(accepted);
    REQUIRE(connected);
    REQUIRE(pair.server.has_value());
    REQUIRE(pair.client.has_value());

    // Shrink both send and receive buffers so the transfer cannot complete in
    // a single syscall; this forces the send side to hit EAGAIN internally.
    const int small = 4096;
    for (int fd : {pair.server->fd(), pair.client->fd()}) {
        setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &small, sizeof(small));
        setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &small, sizeof(small));
    }

    return pair;
}

} // namespace

TEST_CASE("tcp_stream write awaits writability under backpressure",
          "[tcp][stream][eagain][regression]") {
    scheduler sched(2);
    sched.start();

    auto pair = make_small_buffer_tcp_pair(sched);

    // Payload far larger than the (shrunk) socket buffers so the send side is
    // guaranteed to saturate the kernel buffer and receive EAGAIN internally.
    constexpr size_t kPayload = 1 * 1024 * 1024;  // 1 MiB
    std::vector<char> to_send(kPayload);
    for (size_t i = 0; i < kPayload; ++i) {
        to_send[i] = static_cast<char>(i & 0xFF);
    }

    std::atomic<bool> writer_done{false};
    std::atomic<bool> reader_done{false};
    std::atomic<bool> saw_eagain{false};
    std::atomic<size_t> total_written{0};
    std::atomic<size_t> total_read{0};
    std::atomic<bool> data_ok{true};

    // Writer: push the whole payload through, retrying short writes at the
    // application level. Every individual write() must return a positive count
    // or a terminal error -- it must NEVER surface -EAGAIN/-EWOULDBLOCK, since
    // tcp_stream::write() now awaits writability internally.
    auto writer = [&]() -> task<void> {
        size_t off = 0;
        while (off < kPayload) {
            auto r = co_await pair.client->write(to_send.data() + off,
                                                 kPayload - off);
            if (r.result == -EAGAIN || r.result == -EWOULDBLOCK) {
                saw_eagain = true;
                break;
            }
            if (r.result <= 0) {
                break;  // terminal error / peer closed
            }
            off += static_cast<size_t>(r.result);
        }
        total_written = off;
        writer_done = true;
        co_return;
    };

    // Reader: drain slowly so the writer is forced to wait on writability.
    auto reader = [&]() -> task<void> {
        std::vector<char> buf(64 * 1024);
        size_t got = 0;
        while (got < kPayload) {
            auto r = co_await pair.server->read(buf.data(), buf.size());
            if (r.result <= 0) {
                break;
            }
            // Verify byte integrity as we go.
            for (int i = 0; i < r.result; ++i) {
                if (buf[static_cast<size_t>(i)] !=
                    static_cast<char>((got + static_cast<size_t>(i)) & 0xFF)) {
                    data_ok = false;
                }
            }
            got += static_cast<size_t>(r.result);
        }
        total_read = got;
        reader_done = true;
        co_return;
    };

    sched.go(reader);
    sched.go(writer);

    for (int i = 0; i < 1000 && (!writer_done || !reader_done); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    sched.shutdown();

    REQUIRE(writer_done);
    REQUIRE(reader_done);
    // The core assertion: write() never leaked a transient EAGAIN to the app.
    REQUIRE_FALSE(saw_eagain.load());
    REQUIRE(total_written.load() == kPayload);
    REQUIRE(total_read.load() == kPayload);
    REQUIRE(data_ok.load());
}

TEST_CASE("tcp_stream read awaits readability for deferred data",
          "[tcp][stream][eagain][regression]") {
    scheduler sched(2);
    sched.start();

    auto pair = make_small_buffer_tcp_pair(sched);

    const char* msg = "deferred payload after readiness wait";
    const size_t msg_len = strlen(msg);

    std::atomic<bool> reader_done{false};
    std::atomic<bool> writer_done{false};
    io_result read_result{};
    char buffer[128] = {0};

    // Reader starts immediately against an empty socket. A non-blocking recv
    // would return EAGAIN here; read() must instead await readability and only
    // complete once the writer sends data below.
    auto reader = [&]() -> task<void> {
        read_result = co_await pair.server->read(buffer, sizeof(buffer) - 1);
        reader_done = true;
        co_return;
    };

    // Writer delays before sending so the reader is guaranteed to have entered
    // the readiness wait first.
    auto writer = [&]() -> task<void> {
        co_await elio::time::sleep_for(std::chrono::milliseconds(100));
        co_await pair.client->write(msg, msg_len);
        writer_done = true;
        co_return;
    };

    sched.go(reader);
    sched.go(writer);

    for (int i = 0; i < 500 && (!reader_done || !writer_done); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    sched.shutdown();

    REQUIRE(reader_done);
    REQUIRE(writer_done);
    // read() must not surface EAGAIN; it must return the real payload.
    REQUIRE(read_result.result != -EAGAIN);
    REQUIRE(read_result.result != -EWOULDBLOCK);
    REQUIRE(read_result.success());
    REQUIRE(read_result.bytes_transferred() == static_cast<int>(msg_len));
    REQUIRE(std::string(buffer) == msg);
}
