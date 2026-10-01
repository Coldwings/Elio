#pragma once

#include <elio/http/detail/owned_prefix_stream.hpp>
#include <elio/net/stream.hpp>
#include <elio/tls/tls_stream.hpp>

#include <chrono>
#include <optional>
#include <type_traits>
#include <utility>
#include <variant>

namespace elio::http::detail {

using connect_channel = owned_prefix_stream<net::tcp_stream>;
using connect_tls_stream = tls::basic_tls_stream<connect_channel>;

// The public closed TCP/TLS facade remains unchanged. Only Transport owns
// this target-bound variant; tunneled I/O never bypasses its publishing lower.
class route_connection {
public:
    route_connection() = default;
    explicit route_connection(net::stream stream) noexcept : stream_(std::move(stream)) {}
    explicit route_connection(connect_tls_stream stream) noexcept : stream_(std::move(stream)) {}
    route_connection(route_connection&&) noexcept = default;
    route_connection& operator=(route_connection&&) noexcept = default;
    route_connection(const route_connection&) = delete;
    route_connection& operator=(const route_connection&) = delete;

    coro::task<io::io_result> read(void* data, size_t size, coro::cancel_token token = {}) {
        return std::visit([&](auto& stream) -> coro::task<io::io_result> {
            if constexpr (std::same_as<std::decay_t<decltype(stream)>, std::monostate>)
                return disconnected_io();
            else return stream.read(data, size, std::move(token));
        }, stream_);
    }

    coro::task<io::io_result> write(const void* data, size_t size, coro::cancel_token token = {}) {
        return std::visit([&](auto& stream) -> coro::task<io::io_result> {
            if constexpr (std::same_as<std::decay_t<decltype(stream)>, std::monostate>)
                return disconnected_io();
            else return stream.write(data, size, std::move(token));
        }, stream_);
    }

    coro::task<io::io_result> write_all(std::string_view data, coro::cancel_token token = {}) {
        return std::visit([&](auto& stream) -> coro::task<io::io_result> {
            if constexpr (std::same_as<std::decay_t<decltype(stream)>, std::monostate>)
                return disconnected_io();
            else return stream.write_exactly(data, std::move(token));
        }, stream_);
    }

    void shutdown_socket() noexcept {
        std::visit([](auto& stream) {
            using stream_type = std::decay_t<decltype(stream)>;
            if constexpr (std::same_as<stream_type, net::stream>) {
                if (stream.is_tcp()) stream.as_tcp().shutdown_socket();
                else if (stream.is_tls()) stream.as_tls().shutdown_socket();
            } else if constexpr (!std::same_as<stream_type, std::monostate>) {
                stream.shutdown_socket();
            }
        }, stream_);
    }

    void mark_externally_shut_down() noexcept {
        std::visit([](auto& stream) {
            if constexpr (!std::same_as<std::decay_t<decltype(stream)>, std::monostate>)
                stream.mark_externally_shut_down();
        }, stream_);
    }

    void disconnect() noexcept { stream_.emplace<std::monostate>(); }

    int fd() const noexcept {
        // Layered connections intentionally expose no root fd to watchdogs.
        const auto* legacy = std::get_if<net::stream>(&stream_);
        return legacy ? legacy->fd() : -1;
    }
    bool is_connected() const noexcept { return !std::holds_alternative<std::monostate>(stream_); }
    auto last_use() const noexcept { return last_use_; }
    void touch() noexcept { last_use_ = std::chrono::steady_clock::now(); }

    std::optional<net::stream> take_legacy() noexcept {
        auto* legacy = std::get_if<net::stream>(&stream_);
        if (!legacy) return {};
        std::optional<net::stream> result(std::move(*legacy));
        disconnect();
        return result;
    }

private:
    static coro::task<io::io_result> disconnected_io() {
        co_return io::io_result{-ENOTCONN, 0};
    }
    std::variant<std::monostate, net::stream, connect_tls_stream> stream_;
    std::chrono::steady_clock::time_point last_use_ = std::chrono::steady_clock::now();
};

} // namespace elio::http::detail
