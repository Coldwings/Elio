#pragma once

#include <elio/tls/tls_context.hpp>
#include <elio/net/tcp.hpp>
#include <elio/io/io_context.hpp>
#include <elio/coro/task.hpp>
#include <elio/log/macros.hpp>

#include <openssl/ssl.h>
#include <openssl/err.h>

#include <string>
#include <string_view>
#include <memory>
#include <optional>

namespace elio::tls {

/// TLS handshake result
enum class handshake_result {
    success,
    want_read,
    want_write,
    error
};

/// TLS stream wrapping a TCP connection with SSL/TLS encryption
class tls_stream {
public:
    /// Create a TLS stream from an existing TCP stream
    /// @param tcp The underlying TCP stream (takes ownership)
    /// @param ctx TLS context to use
    tls_stream(net::tcp_stream tcp, tls_context& ctx)
        : tcp_(std::move(tcp)) {
        ssl_ = SSL_new(ctx.native_handle());
        if (!ssl_) {
            throw std::runtime_error("Failed to create SSL object");
        }
        
        // Set the file descriptor
        SSL_set_fd(ssl_, tcp_.fd());
        
        // Set mode based on context
        if (ctx.mode() == tls_mode::client) {
            SSL_set_connect_state(ssl_);
        } else {
            SSL_set_accept_state(ssl_);
        }
        
        mode_ = ctx.mode();
    }
    
    /// Destructor
    ~tls_stream() {
        if (ssl_) {
            // Don't call SSL_shutdown in destructor - it may block
            SSL_free(ssl_);
        }
    }
    
    // Non-copyable
    tls_stream(const tls_stream&) = delete;
    tls_stream& operator=(const tls_stream&) = delete;
    
    // Movable
    tls_stream(tls_stream&& other) noexcept
        : tcp_(std::move(other.tcp_))
        , ssl_(other.ssl_)
        , mode_(other.mode_)
        , handshake_complete_(other.handshake_complete_) {
        other.ssl_ = nullptr;
    }
    
    tls_stream& operator=(tls_stream&& other) noexcept {
        if (this != &other) {
            if (ssl_) SSL_free(ssl_);
            tcp_ = std::move(other.tcp_);
            ssl_ = other.ssl_;
            mode_ = other.mode_;
            handshake_complete_ = other.handshake_complete_;
            other.ssl_ = nullptr;
        }
        return *this;
    }
    
    /// Set SNI hostname (for client connections)
    void set_hostname(std::string_view hostname) {
        hostname_ = std::string(hostname);
        // Set SNI extension
        SSL_set_tlsext_host_name(ssl_, hostname_.c_str());
        
        // Configure hostname verification for OpenSSL 1.1.0+
        X509_VERIFY_PARAM* param = SSL_get0_param(ssl_);
        X509_VERIFY_PARAM_set1_host(param, hostname_.c_str(), hostname_.size());
    }
    
    /// Perform TLS handshake asynchronously
    /// @return true on success, false on error (check errno)
    coro::task<bool> handshake() {
        while (true) {
            ERR_clear_error();
            int ret = (mode_ == tls_mode::client) ? SSL_connect(ssl_) : SSL_accept(ssl_);
            
            if (ret == 1) {
                handshake_complete_ = true;
                ELIO_LOG_DEBUG("TLS handshake complete (protocol: {}, cipher: {})",
                              SSL_get_version(ssl_), SSL_get_cipher_name(ssl_));
                co_return true;
            }
            
            int err = SSL_get_error(ssl_, ret);
            
            if (err == SSL_ERROR_WANT_READ) {
                auto result = co_await tcp_.poll_read();
                if (result.result < 0) {
                    errno = -result.result;
                    co_return false;
                }
            } else if (err == SSL_ERROR_WANT_WRITE) {
                auto result = co_await tcp_.poll_write();
                if (result.result < 0) {
                    errno = -result.result;
                    co_return false;
                }
            } else {
                // Handshake failed
                long verify_err = SSL_get_verify_result(ssl_);
                if (verify_err != X509_V_OK) {
                    ELIO_LOG_ERROR("TLS certificate verification failed: {} ({})", 
                                  X509_verify_cert_error_string(verify_err), verify_err);
                }
                ELIO_LOG_ERROR("TLS handshake failed: {}", get_ssl_error_string(err));
                errno = err;
                co_return false;
            }
        }
    }
    
    /// Read data asynchronously
    /// @param buffer Buffer to read into
    /// @param length Maximum bytes to read
    /// @return Number of bytes read, or negative error code
    coro::task<io::io_result> read(void* buffer, size_t length) {
        if (!handshake_complete_) {
            auto hs = co_await handshake();
            if (!hs) {
                co_return io::io_result{-errno, 0};
            }
        }
        
        while (true) {
            ERR_clear_error();
            int ret = SSL_read(ssl_, buffer, static_cast<int>(length));
            
            if (ret > 0) {
                co_return io::io_result{ret, 0};
            }
            
            if (ret == 0) {
                // Connection closed
                int err = SSL_get_error(ssl_, ret);
                if (err == SSL_ERROR_ZERO_RETURN) {
                    // Clean shutdown
                    co_return io::io_result{0, 0};
                }
                co_return io::io_result{-EIO, 0};
            }
            
            int err = SSL_get_error(ssl_, ret);
            
            if (err == SSL_ERROR_WANT_READ) {
                auto result = co_await tcp_.poll_read();
                if (result.result < 0) {
                    co_return result;
                }
            } else if (err == SSL_ERROR_WANT_WRITE) {
                auto result = co_await tcp_.poll_write();
                if (result.result < 0) {
                    co_return result;
                }
            } else {
                ELIO_LOG_ERROR("TLS read error: {}", get_ssl_error_string(err));
                co_return io::io_result{-EIO, 0};
            }
        }
    }
    
    /// Write data asynchronously
    /// @param buffer Data to write
    /// @param length Number of bytes to write
    /// @return Number of bytes written, or negative error code
    coro::task<io::io_result> write(const void* buffer, size_t length) {
        if (!handshake_complete_) {
            auto hs = co_await handshake();
            if (!hs) {
                co_return io::io_result{-errno, 0};
            }
        }
        
        while (true) {
            ERR_clear_error();
            int ret = SSL_write(ssl_, buffer, static_cast<int>(length));
            
            if (ret > 0) {
                co_return io::io_result{ret, 0};
            }
            
            int err = SSL_get_error(ssl_, ret);
            
            if (err == SSL_ERROR_WANT_READ) {
                auto result = co_await tcp_.poll_read();
                if (result.result < 0) {
                    co_return result;
                }
            } else if (err == SSL_ERROR_WANT_WRITE) {
                auto result = co_await tcp_.poll_write();
                if (result.result < 0) {
                    co_return result;
                }
            } else {
                ELIO_LOG_ERROR("TLS write error: {}", get_ssl_error_string(err));
                co_return io::io_result{-EIO, 0};
            }
        }
    }
    
    /// Write string data
    coro::task<io::io_result> write(std::string_view data) {
        return write(data.data(), data.size());
    }
    
    /// Perform TLS shutdown
    coro::task<void> shutdown() {
        if (!ssl_ || !handshake_complete_) {
            co_return;
        }
        
        // Try shutdown (may need to retry)
        for (int i = 0; i < 2; ++i) {
            ERR_clear_error();
            int ret = SSL_shutdown(ssl_);
            
            if (ret == 1) {
                // Shutdown complete
                break;
            }
            
            if (ret == 0) {
                // Need to call again
                continue;
            }
            
            int err = SSL_get_error(ssl_, ret);
            if (err == SSL_ERROR_WANT_READ) {
                co_await tcp_.poll_read();
            } else if (err == SSL_ERROR_WANT_WRITE) {
                co_await tcp_.poll_write();
            } else {
                // Error or done
                break;
            }
        }
        
        handshake_complete_ = false;
    }
    
    /// Get negotiated ALPN protocol
    std::string_view alpn_protocol() const {
        const unsigned char* proto = nullptr;
        unsigned int len = 0;
        SSL_get0_alpn_selected(ssl_, &proto, &len);
        if (proto && len > 0) {
            return std::string_view(reinterpret_cast<const char*>(proto), len);
        }
        return {};
    }
    
    /// Get TLS version string
    const char* version() const {
        return SSL_get_version(ssl_);
    }
    
    /// Get cipher name
    const char* cipher() const {
        return SSL_get_cipher_name(ssl_);
    }
    
    /// Get underlying file descriptor
    int fd() const noexcept { return tcp_.fd(); }
    
    /// Get underlying TCP stream (const)
    const net::tcp_stream& tcp() const noexcept { return tcp_; }
    
    /// Check if handshake is complete
    bool is_handshake_complete() const noexcept { return handshake_complete_; }
    
    /// Get peer certificate (if any)
    X509* peer_certificate() const {
        return SSL_get_peer_certificate(ssl_);
    }
    
    /// Verify peer certificate result
    long verify_result() const {
        return SSL_get_verify_result(ssl_);
    }
    
private:
    static std::string get_ssl_error_string(int err) {
        switch (err) {
            case SSL_ERROR_NONE: return "none";
            case SSL_ERROR_SSL: {
                char buf[256];
                ERR_error_string_n(ERR_get_error(), buf, sizeof(buf));
                return buf;
            }
            case SSL_ERROR_WANT_READ: return "want_read";
            case SSL_ERROR_WANT_WRITE: return "want_write";
            case SSL_ERROR_WANT_X509_LOOKUP: return "want_x509_lookup";
            case SSL_ERROR_SYSCALL: return "syscall error: " + std::string(strerror(errno));
            case SSL_ERROR_ZERO_RETURN: return "zero_return";
            case SSL_ERROR_WANT_CONNECT: return "want_connect";
            case SSL_ERROR_WANT_ACCEPT: return "want_accept";
            default: return "unknown(" + std::to_string(err) + ")";
        }
    }
    
    net::tcp_stream tcp_;
    SSL* ssl_ = nullptr;
    tls_mode mode_ = tls_mode::client;
    bool handshake_complete_ = false;
    std::string hostname_;  // Store hostname for SNI and verification
};

/// Connect to a TLS server
/// @param ctx TLS context (client mode)
/// @param host Hostname to connect to
/// @param port Port to connect to
/// @return TLS stream on success, std::nullopt on error (check errno)
inline coro::task<std::optional<tls_stream>> 
tls_connect(tls_context& ctx, std::string_view host, uint16_t port) {
    // First establish TCP connection
    auto tcp_result = co_await net::tcp_connect(host, port);
    if (!tcp_result) {
        co_return std::nullopt;
    }
    
    // Create TLS stream
    tls_stream stream(std::move(*tcp_result), ctx);
    stream.set_hostname(host);
    
    // Perform handshake
    auto hs_result = co_await stream.handshake();
    if (!hs_result) {
        co_return std::nullopt;
    }
    
    co_return std::move(stream);
}

/// TLS listener for accepting secure connections
class tls_listener {
public:
    /// Create a TLS listener from a TCP listener and TLS context
    tls_listener(net::tcp_listener tcp, tls_context& ctx)
        : tcp_(std::move(tcp)), ctx_(&ctx) {}
    
    /// Accept a new TLS connection
    /// @return TLS stream on success, std::nullopt on error (check errno)
    coro::task<std::optional<tls_stream>> accept() {
        auto tcp_result = co_await tcp_.accept();
        if (!tcp_result) {
            co_return std::nullopt;
        }
        
        tls_stream stream(std::move(*tcp_result), *ctx_);
        
        // Perform handshake
        auto hs_result = co_await stream.handshake();
        if (!hs_result) {
            co_return std::nullopt;
        }
        
        co_return std::move(stream);
    }
    
    /// Get underlying file descriptor
    int fd() const noexcept { return tcp_.fd(); }
    
    /// Bind and create a TLS listener (IPv4)
    /// @return TLS listener on success, std::nullopt on error (check errno)
    static std::optional<tls_listener> 
    bind(const net::ipv4_address& addr, io::io_context& io_ctx, tls_context& ctx) {
        auto tcp_result = net::tcp_listener::bind(addr, io_ctx);
        if (!tcp_result) {
            return std::nullopt;
        }
        return tls_listener(std::move(*tcp_result), ctx);
    }
    
    /// Bind and create a TLS listener (IPv6)
    static std::optional<tls_listener>
    bind(const net::ipv6_address& addr, io::io_context& io_ctx, tls_context& ctx) {
        auto tcp_result = net::tcp_listener::bind(addr, io_ctx);
        if (!tcp_result) {
            return std::nullopt;
        }
        return tls_listener(std::move(*tcp_result), ctx);
    }
    
    /// Bind and create a TLS listener (generic address)
    static std::optional<tls_listener>
    bind(const net::socket_address& addr, io::io_context& io_ctx, tls_context& ctx) {
        auto tcp_result = net::tcp_listener::bind(addr, io_ctx);
        if (!tcp_result) {
            return std::nullopt;
        }
        return tls_listener(std::move(*tcp_result), ctx);
    }
    
private:
    net::tcp_listener tcp_;
    tls_context* ctx_;
};

} // namespace elio::tls
