// Usage: http_streaming_server --port 8080 --file ./sample.bin
// The configured file is not selected by the request URL. Keep its contents
// stable during a response: fstat establishes a length, not a file snapshot.
#include <elio/elio.hpp>
#include <elio/http/http.hpp>
#include <elio/io/file_helpers.hpp>
#include <elio/runtime/spawn_blocking.hpp>

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cstdio>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>

using namespace elio;
using namespace elio::http;

namespace {
struct file_source {
    file_source() = default;
    file_source(const file_source&) = delete;
    file_source& operator=(const file_source&) = delete;
    file_source(file_source&&) noexcept = default;
    file_source& operator=(file_source&&) noexcept = default;
    std::shared_ptr<io::fd_guard> handle;
    uint64_t length = 0;
    int error = 0;
};

file_source open_source(const std::string& path, coro::cancel_token token) {
    file_source source;
    if (token.is_cancelled()) { source.error = ECANCELED; return source; }
    // O_NONBLOCK prevents opening a misconfigured FIFO from waiting forever;
    // fstat below only permits regular files. Regular-file reads still belong
    // on the blocking pool, including when the I/O backend is epoll.
    int fd;
    do { fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NONBLOCK); }
    while (fd < 0 && errno == EINTR && !token.is_cancelled());
    if (fd < 0) { source.error = errno; return source; }
    io::fd_guard guard(fd);
    struct stat metadata{};
    if (::fstat(fd, &metadata) != 0) { source.error = errno; return source; }
    if (!S_ISREG(metadata.st_mode) || metadata.st_size < 0) {
        source.error = EINVAL;
        return source;
    }
    source.handle = std::make_shared<io::fd_guard>(fd);
    (void)guard.release();
    source.length = static_cast<uint64_t>(metadata.st_size);
    return source;
}

coro::task<void> close_source(file_source& source) {
    if (source.handle) {
        // Normal completion, HEAD and cooperative cancellation close on the
        // pool too. fd_guard remains the exception-safety fallback.
        co_await spawn_blocking([owned = std::move(source.handle)]() mutable { owned.reset(); });
    }
}

coro::task<send_result> stream_file(file_source& source, body_writer& writer,
                                   coro::cancel_token token) {
    send_result result;
    try {
        std::array<char, 64 * 1024> buffer{};
        uint64_t offset = 0;
        while (offset < source.length) {
            if (token.is_cancelled()) { result = {send_errc::cancelled, ECANCELED}; break; }
            const auto count = static_cast<size_t>(std::min<uint64_t>(buffer.size(), source.length - offset));
            const int fd = source.handle->get();
            // The producer frame owns buffer across this await. Cancellation
            // is checked after the syscall returns; it cannot destroy a frame
            // while a blocking worker is still using borrowed storage.
            const auto read = co_await spawn_blocking([fd, &buffer, count, offset, token] {
                ssize_t bytes;
                do { bytes = ::pread(fd, buffer.data(), count, static_cast<off_t>(offset)); }
                while (bytes < 0 && errno == EINTR && !token.is_cancelled());
                return std::pair{bytes, bytes < 0 ? errno : 0};
            });
            if (token.is_cancelled()) { result = {send_errc::cancelled, ECANCELED}; break; }
            if (read.first <= 0) {
                result = {send_errc::producer_error, read.first == 0 ? ENODATA : read.second};
                break;
            }
            result = co_await writer.write(
                std::string_view(buffer.data(), static_cast<size_t>(read.first)), token);
            if (!result.success()) break;
            offset += static_cast<uint64_t>(read.first);
            // Only now may the next pread overwrite the borrowed buffer.
        }
        if (token.is_cancelled() && result.success()) result = {send_errc::cancelled, ECANCELED};
    } catch (...) {
        result = {send_errc::producer_error};
    }
    co_await close_source(source);
    // Only the server validates the final length and finalizes the response.
    co_return result;
}

router streaming_routes(std::string configured_path, std::atomic<uint64_t>& invocations) {
    router routes;
    routes.get("/empty", [](context&) { return response(status::ok); });
    auto file_handler = [path = std::move(configured_path), &invocations](context& ctx) -> coro::task<reply> {
        const auto token = ctx.cancel_token();
        // Opening, permissions and type/size validation happen before selecting
        // final headers, so a failure here can still select an HTTP error.
        auto source = co_await spawn_blocking([&path, token] { return open_source(path, token); });
        if (source.error) {
            co_return source.error == ENOENT ? response::not_found("Configured file is unavailable")
                                            : response::internal_error("Configured file cannot be opened");
        }
        const auto length = source.length;
        if (ctx.req().get_method() == method::HEAD || token.is_cancelled()) {
            co_await close_source(source);
        }
        co_return streaming_response(status::ok,
            [source = std::move(source), &invocations](body_writer& writer, coro::cancel_token stop)
                mutable -> coro::task<send_result> {
                invocations.fetch_add(1, std::memory_order_relaxed);
                // HEAD keeps the same declared representation length but never
                // enters this producer, opens a read buffer or reads file data.
                if (!source.handle) co_return send_result{send_errc::producer_error, EBADF};
                co_return co_await stream_file(source, writer, std::move(stop));
            }, length);
    };
    routes.get("/file", file_handler);
    routes.add_route(method::HEAD, "/file", file_handler);
    routes.get("/file-invocations", [&invocations](context&) {
        return response::ok(std::to_string(invocations.load(std::memory_order_relaxed)));
    });
    routes.get("/cancel", [](context&) {
        return streaming_response(status::ok,
            [](body_writer& writer, coro::cancel_token token) -> coro::task<send_result> {
                while (!token.is_cancelled()) {
                    const auto sent = co_await writer.write("tick\n", token);
                    if (!sent.success()) co_return sent;
                    if (co_await time::sleep_for(std::chrono::seconds(1), token) == coro::cancel_result::cancelled) break;
                }
                co_return send_result{send_errc::cancelled, ECANCELED};
            });
    });
    routes.get("/failure", [](context&) {
        return streaming_response(status::ok,
            [](body_writer& writer, coro::cancel_token token) -> coro::task<send_result> {
                const auto sent = co_await writer.write("prefix", token);
                if (!sent.success()) co_return sent;
                // Headers already started: fail/close, never append another
                // response or a successful chunked terminator.
                co_return send_result{send_errc::producer_error};
            });
    });
    return routes;
}

coro::task<int> serve_example(uint16_t port, std::string path) {
    std::atomic<uint64_t> invocations{0};
    server_config config;
    config.write_timeout = std::chrono::seconds(5);
    http::server service(streaming_routes(std::move(path), invocations), config);
    const net::socket_address address = net::ipv4_address("127.0.0.1", port);
    ELIO_LOG_INFO("HTTP streaming example: http://127.0.0.1:{}; press Ctrl+C to stop", port);
    // Both the route state and producers outlive listener/session cleanup.
    // Regular-file blocking operations are cooperative, not forcibly aborted.
    co_await elio::serve(service, [&] { return service.listen(address); });
    co_return 0;
}
} // namespace

int main(int argc, char** argv) {
    try {
        uint16_t port = 0;
        std::string path;
        for (int i = 1; i < argc; i += 2) {
            if (i + 1 == argc) throw std::invalid_argument("missing option value");
            const std::string_view option(argv[i]);
            const std::string_view value(argv[i + 1]);
            if (option == "--port" && port == 0) {
                const auto parsed = std::from_chars(value.data(), value.data() + value.size(), port);
                if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() || port == 0) {
                    throw std::invalid_argument("port must be in 1..65535");
                }
            } else if (option == "--file" && path.empty() && !value.empty()) path = value;
            else throw std::invalid_argument("unknown or duplicate option");
        }
        if (!port || path.empty()) throw std::invalid_argument("--port and --file are required");
        signal::signal_set shutdown_signals(default_shutdown_signals);
        shutdown_signals.block_all_threads();
        return elio::run(serve_example, port, std::move(path));
    } catch (const std::exception& error) {
        std::fprintf(stderr, "%s\nUsage: http_streaming_server --port PORT --file FILE\n", error.what());
        return 1;
    }
}
