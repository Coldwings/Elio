#pragma once

#include <elio/http/detail/owned_prefix_stream.hpp>
#include <elio/http/detail/bounded_pool.hpp>
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
struct route_retirement;

// The public closed TCP/TLS facade remains unchanged. Only Transport owns
// this target-bound variant; tunneled I/O never bypasses its publishing lower.
class route_connection {
public:
    route_connection() = default;
    explicit route_connection(net::stream stream,
            std::shared_ptr<route_retirement> retirement = {})
        : retirement_(std::move(retirement)), stream_(std::move(stream)) {
        auto& legacy = std::get<net::stream>(stream_);
        if (retirement_ && legacy.is_tls())
            tls::detail::tls_retirement_access::bind(legacy.as_tls(), retirement_);
    }
    explicit route_connection(connect_tls_stream stream,
            std::shared_ptr<route_retirement> retirement = {}) noexcept
        : retirement_(std::move(retirement)), stream_(std::move(stream)) {}
    route_connection(route_connection&&) noexcept = default;
    route_connection& operator=(route_connection&& other) noexcept {
        if (this != &other) {
            disconnect();
            retirement_ = std::move(other.retirement_);
            stream_ = std::move(other.stream_);
            last_use_ = other.last_use_;
        }
        return *this;
    }
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

    void disconnect() noexcept {
        stream_.emplace<std::monostate>();
        retirement_.reset();
    }

    coro::task<void> abort_and_settle() {
        return std::visit([](auto& stream) -> coro::task<void> {
            using stream_type = std::decay_t<decltype(stream)>;
            if constexpr (std::same_as<stream_type, connect_tls_stream>)
                return stream.abort_and_settle();
            else if constexpr (std::same_as<stream_type, net::stream>) {
                if (stream.is_tls()) return stream.as_tls().abort_and_settle();
                if (stream.is_tcp()) stream.as_tcp().shutdown_socket();
                return settled();
            } else return settled();
        }, stream_);
    }

    const std::shared_ptr<route_retirement>& retirement_anchor() const noexcept { return retirement_; }

    bool io_quiescent() const noexcept {
        const auto* tunnel = std::get_if<connect_tls_stream>(&stream_);
        if (tunnel) return tls::detail::tls_idle_access::is_quiescent(*tunnel);
        const auto* legacy = std::get_if<net::stream>(&stream_);
        return !legacy || !legacy->is_tls() ||
            tls::detail::tls_idle_access::is_quiescent(legacy->as_tls());
    }

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
    static coro::task<void> settled() { co_return; }
    static coro::task<io::io_result> disconnected_io() {
        co_return io::io_result{-ENOTCONN, 0};
    }
    std::shared_ptr<route_retirement> retirement_;
    std::variant<std::monostate, net::stream, connect_tls_stream> stream_;
    std::chrono::steady_clock::time_point last_use_ = std::chrono::steady_clock::now();
};

struct route_retirement {
    std::shared_ptr<void> operation;
    bounded_pool<route_connection>::permit capacity;
};

inline void defer_stream_retirement(route_connection& stream,
        bounded_pool<route_connection>::permit& capacity) noexcept {
    if (auto anchor = stream.retirement_anchor()) anchor->capacity = std::move(capacity);
}

inline void abort_stream_io(route_connection& stream) noexcept { stream.shutdown_socket(); }

} // namespace elio::http::detail
