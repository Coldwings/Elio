#include <catch2/catch_test_macros.hpp>
#include <elio/http/http_server.hpp>
#include <elio/runtime/scheduler.hpp>
#include "../test_main.cpp"  // For scaled timeouts

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <cerrno>
#include <cstring>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

using namespace elio::coro;
using namespace elio::http;
using namespace elio::runtime;
using namespace std::chrono_literals;

namespace {

class unique_fd {
public:
    explicit unique_fd(int fd = -1) noexcept : fd_(fd) {}
    ~unique_fd() { reset(); }

    unique_fd(unique_fd&& other) noexcept
        : fd_(std::exchange(other.fd_, -1)) {}

    unique_fd& operator=(unique_fd&& other) noexcept {
        if (this != &other) {
            reset();
            fd_ = std::exchange(other.fd_, -1);
        }
        return *this;
    }

    unique_fd(const unique_fd&) = delete;
    unique_fd& operator=(const unique_fd&) = delete;

    int get() const noexcept { return fd_; }
    explicit operator bool() const noexcept { return fd_ >= 0; }

    void reset(int fd = -1) noexcept {
        if (fd_ >= 0) {
            ::close(fd_);
        }
        fd_ = fd;
    }

private:
    int fd_ = -1;
};

template<typename Predicate>
bool wait_until(Predicate pred, std::chrono::milliseconds timeout) {
    auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        if (pred()) {
            return true;
        }
        std::this_thread::sleep_for(elio::test::scaled_ms(10));
    }
    return pred();
}

sockaddr_in loopback_addr(uint16_t port) {
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    addr.sin_port = htons(port);
    return addr;
}

uint16_t reserve_loopback_port() {
    unique_fd fd(::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0));
    REQUIRE(fd);

    int one = 1;
    REQUIRE(::setsockopt(fd.get(), SOL_SOCKET, SO_REUSEADDR, &one,
                         sizeof(one)) == 0);

    auto addr = loopback_addr(0);
    REQUIRE(::bind(fd.get(), reinterpret_cast<sockaddr*>(&addr),
                   sizeof(addr)) == 0);

    sockaddr_in bound{};
    socklen_t len = sizeof(bound);
    REQUIRE(::getsockname(fd.get(), reinterpret_cast<sockaddr*>(&bound),
                          &len) == 0);
    return ntohs(bound.sin_port);
}

unique_fd connect_loopback(uint16_t port) {
    auto addr = loopback_addr(port);
    auto deadline = std::chrono::steady_clock::now() + elio::test::scaled_sec(2);
    while (std::chrono::steady_clock::now() < deadline) {
        unique_fd fd(::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0));
        REQUIRE(fd);

        const auto socket_timeout = elio::test::scaled_sec(2);
        timeval timeout{};
        timeout.tv_sec = static_cast<decltype(timeout.tv_sec)>(
            socket_timeout.count());
        REQUIRE(::setsockopt(fd.get(), SOL_SOCKET, SO_RCVTIMEO, &timeout,
                             sizeof(timeout)) == 0);
        REQUIRE(::setsockopt(fd.get(), SOL_SOCKET, SO_SNDTIMEO, &timeout,
                             sizeof(timeout)) == 0);

        if (::connect(fd.get(), reinterpret_cast<sockaddr*>(&addr),
                      sizeof(addr)) == 0) {
            return fd;
        }

        if (errno != ECONNREFUSED && errno != EINTR) {
            INFO("connect failed: " << std::strerror(errno));
            REQUIRE(false);
        }
        std::this_thread::sleep_for(elio::test::scaled_ms(10));
    }

    FAIL("timed out connecting to loopback HTTP server");
    return unique_fd();
}

void try_wake_listener(uint16_t port) {
    auto addr = loopback_addr(port);
    auto deadline = std::chrono::steady_clock::now() + elio::test::scaled_sec(1);
    while (std::chrono::steady_clock::now() < deadline) {
        unique_fd fd(::socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0));
        if (!fd) {
            return;
        }
        if (::connect(fd.get(), reinterpret_cast<sockaddr*>(&addr),
                      sizeof(addr)) == 0) {
            return;
        }
        if (errno != ECONNREFUSED && errno != EINTR) {
            return;
        }
        std::this_thread::sleep_for(elio::test::scaled_ms(10));
    }
}

void send_all(int fd, std::string_view data) {
    while (!data.empty()) {
        ssize_t n = ::send(fd, data.data(), data.size(), MSG_NOSIGNAL);
        if (n < 0 && errno == EINTR) {
            continue;
        }
        INFO("send failed: " << (n < 0 ? std::strerror(errno) : "closed"));
        REQUIRE(n > 0);
        data.remove_prefix(static_cast<size_t>(n));
    }
}

size_t count_occurrences(std::string_view haystack, std::string_view needle) {
    size_t count = 0;
    size_t pos = 0;
    while ((pos = haystack.find(needle, pos)) != std::string_view::npos) {
        ++count;
        pos += needle.size();
    }
    return count;
}

std::string read_two_ok_responses(int fd) {
    std::string out;
    char buf[1024];
    auto deadline = std::chrono::steady_clock::now() + elio::test::scaled_sec(3);

    while (std::chrono::steady_clock::now() < deadline &&
           count_occurrences(out, "HTTP/1.1 200 OK") < 2) {
        ssize_t n = ::recv(fd, buf, sizeof(buf), 0);
        if (n > 0) {
            out.append(buf, static_cast<size_t>(n));
            continue;
        }
        if (n == 0) {
            break;
        }
        if (errno == EINTR) {
            continue;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            break;
        }
        INFO("recv failed: " << std::strerror(errno));
        REQUIRE(false);
    }

    return out;
}

struct pipeline_result {
    std::string response_bytes;
    std::vector<std::string> paths;
};

pipeline_result run_pipelined_exchange(std::string_view first_write,
                                       std::string_view second_write = {}) {
    router routes;
    std::mutex paths_mutex;
    std::vector<std::string> paths;

    auto record = [&](context& ctx, std::string_view body) {
        {
            std::lock_guard lock(paths_mutex);
            paths.emplace_back(ctx.req().path());
        }
        return response::ok(body);
    };

    routes.get("/first", [&](context& ctx) {
        return record(ctx, "first");
    });
    routes.get("/second", [&](context& ctx) {
        return record(ctx, "second");
    });

    server_config config;
    config.enable_logging = false;
    config.keep_alive_timeout = elio::test::scaled_sec(1);
    config.max_keep_alive_requests = 4;

    server srv(std::move(routes), config);
    const uint16_t port = reserve_loopback_port();

    scheduler sched(2);
    sched.start();

    std::atomic<bool> listen_done{false};
    sched.go([&]() -> task<void> {
        co_await srv.listen(elio::net::ipv4_address("127.0.0.1", port));
        listen_done.store(true, std::memory_order_release);
        co_return;
    });

    const bool started = wait_until([&] { return srv.is_running(); },
                                    elio::test::scaled_sec(2));
    std::string response_bytes;
    bool saw_both_paths = false;

    if (started) {
        auto client = connect_loopback(port);
        send_all(client.get(), first_write);
        if (!second_write.empty()) {
            std::this_thread::sleep_for(elio::test::scaled_ms(100));
            send_all(client.get(), second_write);
        }

        response_bytes = read_two_ok_responses(client.get());
        client.reset();

        saw_both_paths = wait_until([&] {
            std::lock_guard lock(paths_mutex);
            return paths.size() == 2;
        }, elio::test::scaled_sec(2));
    }

    srv.stop();
    try_wake_listener(port);

    const bool stopped = wait_until([&] {
        return listen_done.load(std::memory_order_acquire) &&
               srv.active_connections() == 0;
    }, elio::test::scaled_sec(2));

    sched.shutdown();

    REQUIRE(started);
    REQUIRE(saw_both_paths);
    REQUIRE(stopped);

    std::vector<std::string> paths_copy;
    {
        std::lock_guard lock(paths_mutex);
        paths_copy = paths;
    }
    return {std::move(response_bytes), std::move(paths_copy)};
}

void require_two_ordered_responses(const pipeline_result& result) {
    const std::vector<std::string> expected_paths{"/first", "/second"};
    REQUIRE(result.paths == expected_paths);
    REQUIRE(count_occurrences(result.response_bytes, "HTTP/1.1 200 OK") == 2);

    auto first_pos = result.response_bytes.find("first");
    auto second_pos = result.response_bytes.find("second");
    REQUIRE(first_pos != std::string::npos);
    REQUIRE(second_pos != std::string::npos);
    REQUIRE(first_pos < second_pos);
}

} // namespace

TEST_CASE("HTTP server consumes fully buffered pipelined keep-alive requests",
          "[http][server][pipeline][regression]") {
    const std::string request =
        "GET /first HTTP/1.1\r\n"
        "Host: test\r\n"
        "\r\n"
        "GET /second HTTP/1.1\r\n"
        "Host: test\r\n"
        "Connection: close\r\n"
        "\r\n";

    auto result = run_pipelined_exchange(request);
    require_two_ordered_responses(result);
}

TEST_CASE("HTTP server reads after partial buffered pipelined request",
          "[http][server][pipeline][regression]") {
    const std::string first_write =
        "GET /first HTTP/1.1\r\n"
        "Host: test\r\n"
        "\r\n"
        "GET /second HTTP/1.1\r\n"
        "Host: test\r\n"
        "Connection: ";
    const std::string second_write = "close\r\n\r\n";

    auto result = run_pipelined_exchange(first_write, second_write);
    require_two_ordered_responses(result);
}
