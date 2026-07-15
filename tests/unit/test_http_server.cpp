#include <catch2/catch_test_macros.hpp>
#include <elio/http/http_server.hpp>
#include <elio/http/websocket_server.hpp>
#include <elio/runtime/serve.hpp>
#include <elio/runtime/scheduler.hpp>
#include <elio/sync/primitives.hpp>
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
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#if defined(ELIO_HAS_TLS) && ELIO_HAS_TLS
#include <openssl/evp.h>
#include <openssl/x509.h>
#endif

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

template<typename Rep, typename Period>
bool wait_for_peer_close(int fd, std::chrono::duration<Rep, Period> timeout) {
    auto deadline = std::chrono::steady_clock::now() + timeout;
    char byte = 0;
    while (std::chrono::steady_clock::now() < deadline) {
        ssize_t n = ::recv(fd, &byte, sizeof(byte), MSG_DONTWAIT);
        if (n == 0) {
            return true;
        }
        if (n < 0) {
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) {
                std::this_thread::sleep_for(elio::test::scaled_ms(10));
                continue;
            }
            return true;
        }
        std::this_thread::sleep_for(elio::test::scaled_ms(10));
    }
    return false;
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

std::string read_until_close(int fd) {
    std::string out;
    char buf[1024];
    auto deadline = std::chrono::steady_clock::now() + elio::test::scaled_sec(3);

    while (std::chrono::steady_clock::now() < deadline) {
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

std::string response_block(std::string_view bytes,
                           std::string_view status_line) {
    const auto start = bytes.find(status_line);
    REQUIRE(start != std::string_view::npos);

    const auto next = bytes.find("HTTP/1.1 ", start + status_line.size());
    const auto size = next == std::string_view::npos
        ? std::string_view::npos
        : next - start;
    return std::string(bytes.substr(start, size));
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

std::string run_single_connection_exchange(router routes, std::string_view request_bytes) {
    server_config config;
    config.enable_logging = false;
    config.keep_alive_timeout = elio::test::scaled_sec(1);
    config.max_keep_alive_requests = 8;

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

    if (started) {
        auto client = connect_loopback(port);
        send_all(client.get(), request_bytes);
        response_bytes = read_until_close(client.get());
        client.reset();
    }

    srv.stop();
    try_wake_listener(port);

    const bool stopped = wait_until([&] {
        return listen_done.load(std::memory_order_acquire) &&
               srv.active_connections() == 0;
    }, elio::test::scaled_sec(2));

    sched.shutdown();

    REQUIRE(started);
    REQUIRE(stopped);
    return response_bytes;
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

TEST_CASE("serve drain helper waits for active connection count",
          "[serve][shutdown][active-connections]") {
    struct tracked_server {
        std::atomic<size_t> active{1};

        size_t active_connections() const noexcept {
            return active.load(std::memory_order_acquire);
        }
    } srv;

    scheduler sched(1);
    sched.start();

    std::atomic<bool> drained{false};
    auto handle = sched.go_joinable([&]() -> task<void> {
        co_await elio::detail::wait_active_connections_drained(srv);
        drained.store(true, std::memory_order_release);
        co_return;
    });

    REQUIRE_FALSE(wait_until([&] { return drained.load(std::memory_order_acquire); },
                             elio::test::scaled_ms(100)));

    srv.active.store(0, std::memory_order_release);

    const bool completed = wait_until(
        [&] { return drained.load(std::memory_order_acquire); },
        elio::test::scaled_sec(2));

    REQUIRE(completed);
    handle.wait_destroyed();
    sched.shutdown();
}

TEST_CASE("HTTP router rejects non-terminal wildcard route patterns",
          "[http][router][wildcard]") {
    router routes;
    auto handler = [](context&) {
        return response::ok("matched");
    };

    REQUIRE_THROWS_AS(routes.get("/admin/*/delete", handler),
                      std::invalid_argument);
    REQUIRE_THROWS_AS(routes.get("*/suffix", handler),
                      std::invalid_argument);
}

TEST_CASE("HTTP router preserves terminal wildcard route semantics",
          "[http][router][wildcard]") {
    router routes;
    routes.get("/static/*", [](context&) {
        return response::ok("matched");
    });

    std::unordered_map<std::string, std::string> params;
    REQUIRE(routes.find_route(method::GET, "/static/", params) != nullptr);

    params.clear();
    REQUIRE(routes.find_route(method::GET, "/static/css/app.css", params) != nullptr);

    params.clear();
    REQUIRE(routes.find_route(method::GET, "/static", params) == nullptr);
}

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

TEST_CASE("HTTP server suppresses bodies for HEAD and no-body statuses",
          "[http][server][response][regression]") {
    router routes;
    routes.add_route(method::HEAD, "/head", [](context&) {
        return response(status::ok, "head-body");
    });
    routes.get("/no-content", [](context&) {
        return response(status::no_content, "no-content-body");
    });
    routes.get("/reset-content", [](context&) {
        return response(status::reset_content, "reset-content-body");
    });
    routes.get("/not-modified", [](context&) {
        return response(status::not_modified, "not-modified-body");
    });

    const std::string request =
        "HEAD /head HTTP/1.1\r\n"
        "Host: test\r\n"
        "\r\n"
        "GET /no-content HTTP/1.1\r\n"
        "Host: test\r\n"
        "\r\n"
        "GET /reset-content HTTP/1.1\r\n"
        "Host: test\r\n"
        "\r\n"
        "GET /not-modified HTTP/1.1\r\n"
        "Host: test\r\n"
        "Connection: close\r\n"
        "\r\n";

    auto response_bytes = run_single_connection_exchange(std::move(routes), request);

    auto head = response_block(response_bytes, "HTTP/1.1 200 OK\r\n");
    auto no_content =
        response_block(response_bytes, "HTTP/1.1 204 No Content\r\n");
    auto reset_content =
        response_block(response_bytes, "HTTP/1.1 205 Reset Content\r\n");
    auto not_modified =
        response_block(response_bytes, "HTTP/1.1 304 Not Modified\r\n");

    REQUIRE(head.find("Content-Length: 9\r\n") != std::string::npos);
    REQUIRE(head.find("head-body") == std::string::npos);

    REQUIRE(no_content.find("Content-Length: ") == std::string::npos);
    REQUIRE(no_content.find("no-content-body") == std::string::npos);

    REQUIRE(reset_content.find("Content-Length: ") == std::string::npos);
    REQUIRE(reset_content.find("reset-content-body") == std::string::npos);

    REQUIRE(not_modified.find("Content-Length: ") == std::string::npos);
    REQUIRE(not_modified.find("not-modified-body") == std::string::npos);
}

TEST_CASE("HTTP server stop cancels idle accept without a wake connection",
          "[http][server][stop][accept-cancel-regression]") {
    router routes;
    server_config config;
    config.enable_logging = false;

    server srv(std::move(routes), config);
    const uint16_t port = reserve_loopback_port();

    scheduler sched(1);
    sched.start();

    std::atomic<bool> listen_done{false};
    sched.go([&]() -> task<void> {
        co_await srv.listen(elio::net::ipv4_address("127.0.0.1", port));
        listen_done.store(true, std::memory_order_release);
        co_return;
    });

    const bool started = wait_until([&] { return srv.is_running(); },
                                    elio::test::scaled_sec(2));
    if (started) {
        srv.stop();
    }

    const bool stopped = wait_until(
        [&] { return listen_done.load(std::memory_order_acquire); },
        elio::test::scaled_sec(2));
    const bool drained = sched.shutdown(elio::test::scaled_sec(5));

    REQUIRE(started);
    REQUIRE(stopped);
    REQUIRE(drained);
}

TEST_CASE("WebSocket server stop cancels idle accept without a wake connection",
          "[websocket][server][stop][accept-cancel-regression]") {
    elio::http::websocket::ws_router routes;
    server_config config;
    config.enable_logging = false;

    elio::http::websocket::ws_server srv(std::move(routes), config);
    const uint16_t port = reserve_loopback_port();

    scheduler sched(1);
    sched.start();

    std::atomic<bool> listen_done{false};
    sched.go([&]() -> task<void> {
        co_await srv.listen(elio::net::ipv4_address("127.0.0.1", port));
        listen_done.store(true, std::memory_order_release);
        co_return;
    });

    const bool started = wait_until([&] { return srv.is_running(); },
                                    elio::test::scaled_sec(2));
    if (started) {
        srv.stop();
    }

    const bool stopped = wait_until(
        [&] { return listen_done.load(std::memory_order_acquire); },
        elio::test::scaled_sec(2));
    const bool drained = sched.shutdown(elio::test::scaled_sec(5));

    REQUIRE(started);
    REQUIRE(stopped);
    REQUIRE(drained);
}

TEST_CASE("WebSocket server times out a silent HTTP upgrade request",
          "[websocket][server][timeout][regression]") {
    elio::http::websocket::ws_router routes;
    server_config config;
    config.enable_logging = false;
    config.keep_alive_timeout = elio::test::scaled_sec(1);

    elio::http::websocket::ws_server srv(std::move(routes), config);
    const uint16_t port = reserve_loopback_port();

    scheduler sched(2);
    sched.start();

    std::atomic<bool> listen_done{false};
    sched.go([&]() -> task<void> {
        co_await srv.listen(elio::net::ipv4_address("127.0.0.1", port));
        listen_done.store(true, std::memory_order_release);
        co_return;
    });

    REQUIRE(wait_until([&] { return srv.is_running(); },
                       elio::test::scaled_sec(2)));

    auto client = connect_loopback(port);
    const bool peer_closed = wait_for_peer_close(
        client.get(), config.keep_alive_timeout + elio::test::scaled_sec(2));
    client.reset();

    srv.stop();
    try_wake_listener(port);

    const bool stopped = wait_until(
        [&] { return listen_done.load(std::memory_order_acquire); },
        elio::test::scaled_sec(2));
    const bool drained = sched.shutdown(elio::test::scaled_sec(5));

    REQUIRE(peer_closed);
    REQUIRE(stopped);
    REQUIRE(drained);
}

#if defined(ELIO_HAS_TLS) && ELIO_HAS_TLS
TEST_CASE("HTTPS server times out a silent inbound TLS handshake",
          "[http][server][tls][timeout][regression]") {
    router routes;
    server_config config;
    config.enable_logging = false;
    config.keep_alive_timeout = elio::test::scaled_sec(1);

    server srv(std::move(routes), config);
    elio::tls::tls_context tls_ctx(elio::tls::tls_mode::server);
    REQUIRE(install_test_certificate(tls_ctx));

    const uint16_t port = reserve_loopback_port();
    scheduler sched(2);
    sched.start();

    std::atomic<bool> listen_done{false};
    sched.go([&]() -> task<void> {
        co_await srv.listen_tls(elio::net::ipv4_address("127.0.0.1", port),
                                tls_ctx);
        listen_done.store(true, std::memory_order_release);
        co_return;
    });

    REQUIRE(wait_until([&] { return srv.is_running(); },
                       elio::test::scaled_sec(2)));

    auto client = connect_loopback(port);
    const bool handler_started = wait_until(
        [&] { return srv.active_connections() > 0; },
        elio::test::scaled_sec(2));
    const bool handler_stopped = wait_until(
        [&] { return srv.active_connections() == 0; },
        config.keep_alive_timeout + elio::test::scaled_sec(2));
    const bool peer_closed = wait_for_peer_close(
        client.get(), config.keep_alive_timeout + elio::test::scaled_sec(2));
    client.reset();

    srv.stop();
    try_wake_listener(port);

    const bool stopped = wait_until(
        [&] { return listen_done.load(std::memory_order_acquire); },
        elio::test::scaled_sec(2));
    const bool drained = sched.shutdown(elio::test::scaled_sec(5));

    REQUIRE(handler_started);
    REQUIRE(handler_stopped);
    REQUIRE(peer_closed);
    REQUIRE(stopped);
    REQUIRE(drained);
}

TEST_CASE("WSS server times out a silent inbound TLS handshake",
          "[websocket][server][tls][timeout][regression]") {
    elio::http::websocket::ws_router routes;
    server_config config;
    config.enable_logging = false;
    config.keep_alive_timeout = elio::test::scaled_sec(1);

    elio::http::websocket::ws_server srv(std::move(routes), config);
    elio::tls::tls_context tls_ctx(elio::tls::tls_mode::server);
    REQUIRE(install_test_certificate(tls_ctx));

    const uint16_t port = reserve_loopback_port();
    scheduler sched(2);
    sched.start();

    std::atomic<bool> listen_done{false};
    sched.go([&]() -> task<void> {
        co_await srv.listen_tls(elio::net::ipv4_address("127.0.0.1", port),
                                tls_ctx);
        listen_done.store(true, std::memory_order_release);
        co_return;
    });

    REQUIRE(wait_until([&] { return srv.is_running(); },
                       elio::test::scaled_sec(2)));

    auto client = connect_loopback(port);
    const bool peer_closed = wait_for_peer_close(
        client.get(), config.keep_alive_timeout + elio::test::scaled_sec(2));
    client.reset();

    srv.stop();
    try_wake_listener(port);

    const bool stopped = wait_until(
        [&] { return listen_done.load(std::memory_order_acquire); },
        elio::test::scaled_sec(2));
    const bool drained = sched.shutdown(elio::test::scaled_sec(5));

    REQUIRE(peer_closed);
    REQUIRE(stopped);
    REQUIRE(drained);
}
#endif

TEST_CASE("HTTP server stop cancels overlapping idle accepts",
          "[http][server][stop][accept-cancel-regression]") {
    router routes;
    server_config config;
    config.enable_logging = false;

    server srv(std::move(routes), config);
    const uint16_t first_port = reserve_loopback_port();
    const uint16_t second_port = reserve_loopback_port();

    scheduler sched(2);
    sched.start();

    std::atomic<bool> first_done{false};
    std::atomic<bool> second_done{false};
    sched.go([&]() -> task<void> {
        co_await srv.listen(elio::net::ipv4_address("127.0.0.1", first_port));
        first_done.store(true, std::memory_order_release);
        co_return;
    });

    auto first_client = connect_loopback(first_port);
    first_client.reset();
    REQUIRE(wait_until([&] { return srv.active_connections() == 0; },
                       elio::test::scaled_sec(2)));

    sched.go([&]() -> task<void> {
        co_await srv.listen(elio::net::ipv4_address("127.0.0.1", second_port));
        second_done.store(true, std::memory_order_release);
        co_return;
    });

    auto second_client = connect_loopback(second_port);
    second_client.reset();
    REQUIRE(wait_until([&] { return srv.active_connections() == 0; },
                       elio::test::scaled_sec(2)));

    srv.stop();

    const bool stopped = wait_until([&] {
        return first_done.load(std::memory_order_acquire) &&
               second_done.load(std::memory_order_acquire);
    }, elio::test::scaled_sec(2));
    const bool drained = sched.shutdown(elio::test::scaled_sec(5));

    REQUIRE(stopped);
    REQUIRE(drained);
}

TEST_CASE("WebSocket server stop cancels overlapping idle accepts",
          "[websocket][server][stop][accept-cancel-regression]") {
    elio::http::websocket::ws_router routes;
    server_config config;
    config.enable_logging = false;

    elio::http::websocket::ws_server srv(std::move(routes), config);
    const uint16_t first_port = reserve_loopback_port();
    const uint16_t second_port = reserve_loopback_port();

    scheduler sched(2);
    sched.start();

    std::atomic<bool> first_done{false};
    std::atomic<bool> second_done{false};
    sched.go([&]() -> task<void> {
        co_await srv.listen(elio::net::ipv4_address("127.0.0.1", first_port));
        first_done.store(true, std::memory_order_release);
        co_return;
    });

    auto first_client = connect_loopback(first_port);
    first_client.reset();
    std::this_thread::sleep_for(elio::test::scaled_ms(50));

    sched.go([&]() -> task<void> {
        co_await srv.listen(elio::net::ipv4_address("127.0.0.1", second_port));
        second_done.store(true, std::memory_order_release);
        co_return;
    });

    auto second_client = connect_loopback(second_port);
    second_client.reset();
    std::this_thread::sleep_for(elio::test::scaled_ms(50));

    srv.stop();

    const bool stopped = wait_until([&] {
        return first_done.load(std::memory_order_acquire) &&
               second_done.load(std::memory_order_acquire);
    }, elio::test::scaled_sec(2));
    const bool drained = sched.shutdown(elio::test::scaled_sec(5));

    REQUIRE(stopped);
    REQUIRE(drained);
}

TEST_CASE("WebSocket server tracks active upgraded handlers",
          "[websocket][server][shutdown][active-connections]") {
    elio::http::websocket::ws_router routes;
    server_config config;
    config.enable_logging = false;

    elio::sync::event release_handler;
    std::atomic<bool> handler_started{false};

    routes.websocket("/ws", [&](elio::http::websocket::ws_connection&) -> task<void> {
        handler_started.store(true, std::memory_order_release);
        co_await release_handler.wait();
        co_return;
    });

    elio::http::websocket::ws_server srv(std::move(routes), config);
    const uint16_t port = reserve_loopback_port();

    scheduler sched(2);
    sched.start();

    std::atomic<bool> listen_done{false};
    sched.go([&]() -> task<void> {
        co_await srv.listen(elio::net::ipv4_address("127.0.0.1", port));
        listen_done.store(true, std::memory_order_release);
        co_return;
    });

    REQUIRE(wait_until([&] { return srv.is_running(); },
                       elio::test::scaled_sec(2)));

    auto client = connect_loopback(port);
    send_all(client.get(),
             "GET /ws HTTP/1.1\r\n"
             "Host: localhost\r\n"
             "Upgrade: websocket\r\n"
             "Connection: Upgrade\r\n"
             "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
             "Sec-WebSocket-Version: 13\r\n"
             "\r\n");

    REQUIRE(wait_until(
        [&] {
            return handler_started.load(std::memory_order_acquire) &&
                   srv.active_connections() == 1;
        },
        elio::test::scaled_sec(2)));

    srv.stop();
    try_wake_listener(port);

    REQUIRE(wait_until([&] { return listen_done.load(std::memory_order_acquire); },
                       elio::test::scaled_sec(2)));
    REQUIRE(srv.active_connections() == 1);

    release_handler.set();
    client.reset();

    REQUIRE(wait_until([&] { return srv.active_connections() == 0; },
                       elio::test::scaled_sec(2)));
    REQUIRE(sched.shutdown(elio::test::scaled_sec(5)));
}

TEST_CASE("WebSocket server enforces max_request_size across consumed upgrade bytes",
          "[websocket][server][limits][regression]") {
    elio::http::websocket::ws_router routes;
    server_config config;
    config.enable_logging = false;
    config.keep_alive_timeout = elio::test::scaled_sec(1);
    config.read_buffer_size = 32;
    config.max_request_size = 128;

    std::atomic<bool> handler_started{false};
    routes.websocket("/ws", [&](elio::http::websocket::ws_connection&) -> task<void> {
        handler_started.store(true, std::memory_order_release);
        co_return;
    });

    elio::http::websocket::ws_server srv(std::move(routes), config);
    const uint16_t port = reserve_loopback_port();

    scheduler sched(2);
    sched.start();

    std::atomic<bool> listen_done{false};
    sched.go([&]() -> task<void> {
        co_await srv.listen(elio::net::ipv4_address("127.0.0.1", port));
        listen_done.store(true, std::memory_order_release);
        co_return;
    });

    REQUIRE(wait_until([&] { return srv.is_running(); },
                       elio::test::scaled_sec(2)));

    auto client = connect_loopback(port);
    send_all(client.get(),
             "GET /ws HTTP/1.1\r\n"
             "Host: localhost\r\n"
             "Upgrade: websocket\r\n"
             "Connection: Upgrade\r\n"
             "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
             "Sec-WebSocket-Version: 13\r\n"
             "\r\n");

    const auto response = read_until_close(client.get());
    CHECK(response.find("HTTP/1.1 413 Payload Too Large") !=
          std::string::npos);
    CHECK_FALSE(handler_started.load(std::memory_order_acquire));

    srv.stop();
    try_wake_listener(port);

    REQUIRE(wait_until([&] { return listen_done.load(std::memory_order_acquire); },
                       elio::test::scaled_sec(2)));
    REQUIRE(sched.shutdown(elio::test::scaled_sec(5)));
}

TEST_CASE("WebSocket server excludes pipelined frame bytes from request size",
          "[websocket][server][limits][regression]") {
    const std::string upgrade_request =
        "GET /ws HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
        "Sec-WebSocket-Version: 13\r\n"
        "\r\n";

    elio::http::websocket::ws_router routes;
    server_config config;
    config.enable_logging = false;
    config.keep_alive_timeout = elio::test::scaled_sec(1);
    config.read_buffer_size = 256;
    config.max_request_size = upgrade_request.size();

    std::atomic<bool> handler_done{false};
    std::string received_message;
    routes.websocket("/ws", [&](elio::http::websocket::ws_connection& conn) -> task<void> {
        auto msg = co_await conn.receive();
        if (msg) {
            received_message = msg->data;
        }
        handler_done.store(true, std::memory_order_release);
        co_return;
    });

    elio::http::websocket::ws_server srv(std::move(routes), config);
    const uint16_t port = reserve_loopback_port();

    scheduler sched(2);
    sched.start();

    std::atomic<bool> listen_done{false};
    sched.go([&]() -> task<void> {
        co_await srv.listen(elio::net::ipv4_address("127.0.0.1", port));
        listen_done.store(true, std::memory_order_release);
        co_return;
    });

    REQUIRE(wait_until([&] { return srv.is_running(); },
                       elio::test::scaled_sec(2)));

    auto first_frame = elio::http::websocket::encode_text_frame("hello", true);
    std::string client_bytes = upgrade_request;
    client_bytes.append(reinterpret_cast<const char*>(first_frame.data()),
                        first_frame.size());

    auto client = connect_loopback(port);
    send_all(client.get(), client_bytes);

    const auto response = read_until_close(client.get());
    CHECK(response.find("HTTP/1.1 101 Switching Protocols") !=
          std::string::npos);
    REQUIRE(wait_until([&] {
        return handler_done.load(std::memory_order_acquire);
    }, elio::test::scaled_sec(2)));
    CHECK(received_message == "hello");

    srv.stop();
    try_wake_listener(port);

    REQUIRE(wait_until([&] { return listen_done.load(std::memory_order_acquire); },
                       elio::test::scaled_sec(2)));
    REQUIRE(sched.shutdown(elio::test::scaled_sec(5)));
}

TEST_CASE("WebSocket server rejects invalid pipelined first frame before handler",
          "[websocket][server][limits][regression]") {
    const std::string upgrade_request =
        "GET /ws HTTP/1.1\r\n"
        "Host: localhost\r\n"
        "Upgrade: websocket\r\n"
        "Connection: Upgrade\r\n"
        "Sec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n"
        "Sec-WebSocket-Version: 13\r\n"
        "\r\n";

    elio::http::websocket::ws_router routes;
    server_config config;
    config.enable_logging = false;
    config.keep_alive_timeout = elio::test::scaled_sec(1);
    config.read_buffer_size = 256;

    elio::http::websocket::server_config ws_config;
    ws_config.max_message_size = 4;

    std::atomic<bool> handler_started{false};
    routes.websocket("/ws", [&](elio::http::websocket::ws_connection&) -> task<void> {
        handler_started.store(true, std::memory_order_release);
        co_return;
    }, ws_config);

    elio::http::websocket::ws_server srv(std::move(routes), config);
    const uint16_t port = reserve_loopback_port();

    scheduler sched(2);
    sched.start();

    std::atomic<bool> listen_done{false};
    sched.go([&]() -> task<void> {
        co_await srv.listen(elio::net::ipv4_address("127.0.0.1", port));
        listen_done.store(true, std::memory_order_release);
        co_return;
    });

    REQUIRE(wait_until([&] { return srv.is_running(); },
                       elio::test::scaled_sec(2)));

    auto oversized_frame =
        elio::http::websocket::encode_text_frame("hello", true);

    std::string client_bytes = upgrade_request;
    client_bytes.append(reinterpret_cast<const char*>(oversized_frame.data()),
                        oversized_frame.size());

    auto client = connect_loopback(port);
    send_all(client.get(), client_bytes);

    const auto response = read_until_close(client.get());
    CHECK(response.find("HTTP/1.1 101 Switching Protocols") !=
          std::string::npos);
    CHECK_FALSE(handler_started.load(std::memory_order_acquire));

    srv.stop();
    try_wake_listener(port);

    REQUIRE(wait_until([&] { return listen_done.load(std::memory_order_acquire); },
                       elio::test::scaled_sec(2)));
    REQUIRE(sched.shutdown(elio::test::scaled_sec(5)));
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
