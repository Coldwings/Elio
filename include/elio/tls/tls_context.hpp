#pragma once

#include <openssl/ssl.h>
#include <openssl/err.h>
#include <openssl/x509.h>
#include <openssl/x509_vfy.h>
#include <elio/log/macros.hpp>

#include <string>
#include <string_view>
#include <memory>
#include <stdexcept>

namespace elio::tls {

/// TLS protocol version
enum class tls_version {
    tls_1_2,
    tls_1_3,
    tls_1_2_or_higher,
    tls_1_3_only
};

/// TLS context mode
enum class tls_mode {
    client,
    server
};

/// TLS verification mode
enum class verify_mode {
    none,           ///< No verification
    peer,           ///< Verify peer certificate
    fail_if_no_cert ///< Fail if no peer certificate (server mode)
};

/// RAII wrapper for OpenSSL initialization
class openssl_init {
public:
    openssl_init() {
        // OpenSSL 1.1.0+ auto-initializes, but explicit init doesn't hurt
        OPENSSL_init_ssl(OPENSSL_INIT_LOAD_SSL_STRINGS | OPENSSL_INIT_LOAD_CRYPTO_STRINGS, nullptr);
    }
    
    ~openssl_init() {
        // Cleanup is handled automatically in OpenSSL 1.1.0+
    }
    
    static openssl_init& instance() {
        static openssl_init init;
        return init;
    }
};

/// TLS context configuration and SSL_CTX wrapper
class tls_context {
public:
    /// Create a TLS context
    /// @param mode Client or server mode
    /// @param version TLS version requirements
    explicit tls_context(tls_mode mode, tls_version version = tls_version::tls_1_2_or_higher)
        : mode_(mode) {
        // Ensure OpenSSL is initialized
        openssl_init::instance();
        
        // Create SSL context
        const SSL_METHOD* method = nullptr;
        switch (mode) {
            case tls_mode::client:
                method = TLS_client_method();
                break;
            case tls_mode::server:
                method = TLS_server_method();
                break;
        }
        
        ctx_ = SSL_CTX_new(method);
        if (!ctx_) {
            throw std::runtime_error("Failed to create SSL context: " + get_ssl_error());
        }
        
        // Set minimum TLS version
        int min_version = TLS1_2_VERSION;
        int max_version = 0;  // 0 means no max
        
        switch (version) {
            case tls_version::tls_1_2:
                min_version = TLS1_2_VERSION;
                max_version = TLS1_2_VERSION;
                break;
            case tls_version::tls_1_3:
            case tls_version::tls_1_3_only:
                min_version = TLS1_3_VERSION;
                max_version = TLS1_3_VERSION;
                break;
            case tls_version::tls_1_2_or_higher:
                min_version = TLS1_2_VERSION;
                break;
        }
        
        SSL_CTX_set_min_proto_version(ctx_, min_version);
        if (max_version > 0) {
            SSL_CTX_set_max_proto_version(ctx_, max_version);
        }
        
        // Set default options
        SSL_CTX_set_options(ctx_, SSL_OP_NO_SSLv2 | SSL_OP_NO_SSLv3 | SSL_OP_NO_COMPRESSION);
        
        // Enable session caching for performance
        SSL_CTX_set_session_cache_mode(ctx_, SSL_SESS_CACHE_BOTH);
        
        ELIO_LOG_DEBUG("TLS context created (mode={}, version={})", 
                       mode == tls_mode::client ? "client" : "server",
                       static_cast<int>(version));
    }
    
    /// Destructor
    ~tls_context() {
        if (ctx_) {
            SSL_CTX_free(ctx_);
        }
    }
    
    // Non-copyable
    tls_context(const tls_context&) = delete;
    tls_context& operator=(const tls_context&) = delete;
    
    // Movable
    tls_context(tls_context&& other) noexcept
        : ctx_(other.ctx_), mode_(other.mode_) {
        other.ctx_ = nullptr;
    }
    
    tls_context& operator=(tls_context&& other) noexcept {
        if (this != &other) {
            if (ctx_) SSL_CTX_free(ctx_);
            ctx_ = other.ctx_;
            mode_ = other.mode_;
            other.ctx_ = nullptr;
        }
        return *this;
    }
    
    /// Load certificate chain from file
    /// @param cert_file Path to PEM certificate file
    bool load_certificate(std::string_view cert_file) {
        if (SSL_CTX_use_certificate_chain_file(ctx_, std::string(cert_file).c_str()) != 1) {
            ELIO_LOG_ERROR("Failed to load certificate: {}", get_ssl_error());
            return false;
        }
        ELIO_LOG_INFO("Loaded certificate from {}", cert_file);
        return true;
    }
    
    /// Load private key from file
    /// @param key_file Path to PEM private key file
    /// @param password Optional password for encrypted key
    bool load_private_key(std::string_view key_file, std::string_view password = "") {
        if (!password.empty()) {
            SSL_CTX_set_default_passwd_cb_userdata(ctx_, const_cast<char*>(password.data()));
        }
        
        if (SSL_CTX_use_PrivateKey_file(ctx_, std::string(key_file).c_str(), SSL_FILETYPE_PEM) != 1) {
            ELIO_LOG_ERROR("Failed to load private key: {}", get_ssl_error());
            return false;
        }
        
        // Verify key matches certificate
        if (SSL_CTX_check_private_key(ctx_) != 1) {
            ELIO_LOG_ERROR("Private key does not match certificate: {}", get_ssl_error());
            return false;
        }
        
        ELIO_LOG_INFO("Loaded private key from {}", key_file);
        return true;
    }
    
    /// Load CA certificates for verification
    /// @param ca_file Path to CA certificate file (PEM)
    /// @param ca_path Path to directory containing CA certificates
    bool load_verify_locations(std::string_view ca_file = "", std::string_view ca_path = "") {
        const char* file = ca_file.empty() ? nullptr : std::string(ca_file).c_str();
        const char* path = ca_path.empty() ? nullptr : std::string(ca_path).c_str();
        
        if (SSL_CTX_load_verify_locations(ctx_, file, path) != 1) {
            ELIO_LOG_ERROR("Failed to load CA certificates: {}", get_ssl_error());
            return false;
        }
        return true;
    }
    
    /// Use system default CA certificates
    /// Tries multiple common locations if the OpenSSL default paths fail
    bool use_default_verify_paths() {
        // Try explicitly loading from known locations with both file and directory
        // This is more reliable than SSL_CTX_set_default_verify_paths on some systems
        static const struct {
            const char* file;
            const char* dir;
        } ca_locations[] = {
            {"/etc/ssl/certs/ca-certificates.crt", "/etc/ssl/certs"},     // Debian/Ubuntu
            {"/etc/pki/tls/certs/ca-bundle.crt", "/etc/pki/tls/certs"},   // Fedora/RHEL
            {"/etc/ssl/ca-bundle.pem", "/etc/ssl/certs"},                  // OpenSUSE
            {"/etc/ssl/cert.pem", "/etc/ssl/certs"},                       // Alpine/macOS
            {nullptr, nullptr}
        };
        
        for (const auto& loc : ca_locations) {
            if (!loc.file && !loc.dir) break;
            if (SSL_CTX_load_verify_locations(ctx_, loc.file, loc.dir) == 1) {
                ELIO_LOG_DEBUG("Loaded CA certificates from file={} dir={}", 
                             loc.file ? loc.file : "(none)", 
                             loc.dir ? loc.dir : "(none)");
                return true;
            }
        }
        
        // Fallback to OpenSSL default
        if (SSL_CTX_set_default_verify_paths(ctx_) == 1) {
            ELIO_LOG_DEBUG("Using OpenSSL default CA paths");
            return true;
        }
        
        ELIO_LOG_WARNING("Failed to load system CA certificates");
        return false;
    }
    
    /// Set peer verification mode
    void set_verify_mode(verify_mode mode) {
        int ssl_mode = SSL_VERIFY_NONE;
        switch (mode) {
            case verify_mode::none:
                ssl_mode = SSL_VERIFY_NONE;
                break;
            case verify_mode::peer:
                ssl_mode = SSL_VERIFY_PEER;
                break;
            case verify_mode::fail_if_no_cert:
                ssl_mode = SSL_VERIFY_PEER | SSL_VERIFY_FAIL_IF_NO_PEER_CERT;
                break;
        }
        
        SSL_CTX_set_verify(ctx_, ssl_mode, nullptr);
        
        // Set reasonable verification depth
        SSL_CTX_set_verify_depth(ctx_, 10);
        
        // Enable partial chain verification (trust intermediate CAs if present in store)
        X509_STORE* store = SSL_CTX_get_cert_store(ctx_);
        if (store) {
            X509_STORE_set_flags(store, X509_V_FLAG_PARTIAL_CHAIN);
        }
    }
    
    /// Set ALPN protocols (for HTTP/2 negotiation)
    /// @param protocols Comma-separated protocol list (e.g., "h2,http/1.1")
    bool set_alpn_protocols(std::string_view protocols) {
        // Build wire format: each protocol prefixed by its length
        std::string wire;
        size_t start = 0;
        while (start < protocols.size()) {
            size_t end = protocols.find(',', start);
            if (end == std::string_view::npos) end = protocols.size();
            
            size_t len = end - start;
            if (len > 0 && len <= 255) {
                wire += static_cast<char>(len);
                wire += protocols.substr(start, len);
            }
            start = end + 1;
        }
        
        if (mode_ == tls_mode::client) {
            if (SSL_CTX_set_alpn_protos(ctx_, 
                    reinterpret_cast<const unsigned char*>(wire.data()), 
                    static_cast<unsigned>(wire.size())) != 0) {
                ELIO_LOG_ERROR("Failed to set ALPN protocols");
                return false;
            }
        } else {
            // Server-side ALPN callback
            alpn_protocols_ = std::move(wire);
            SSL_CTX_set_alpn_select_cb(ctx_, alpn_select_callback, this);
        }
        return true;
    }
    
    /// Set cipher list
    bool set_ciphers(std::string_view ciphers) {
        if (SSL_CTX_set_cipher_list(ctx_, std::string(ciphers).c_str()) != 1) {
            ELIO_LOG_ERROR("Failed to set ciphers: {}", get_ssl_error());
            return false;
        }
        return true;
    }
    
    /// Set TLS 1.3 cipher suites
    bool set_ciphersuites(std::string_view ciphersuites) {
        if (SSL_CTX_set_ciphersuites(ctx_, std::string(ciphersuites).c_str()) != 1) {
            ELIO_LOG_ERROR("Failed to set TLS 1.3 ciphersuites: {}", get_ssl_error());
            return false;
        }
        return true;
    }
    
    /// Get the underlying SSL_CTX pointer
    SSL_CTX* native_handle() noexcept { return ctx_; }
    const SSL_CTX* native_handle() const noexcept { return ctx_; }
    
    /// Get context mode
    tls_mode mode() const noexcept { return mode_; }
    
    /// Create a default client context with system CA certificates
    static tls_context make_client() {
        tls_context ctx(tls_mode::client);
        ctx.use_default_verify_paths();
        ctx.set_verify_mode(verify_mode::peer);
        return ctx;
    }
    
    /// Create a server context with certificate and key
    static tls_context make_server(std::string_view cert_file, std::string_view key_file) {
        tls_context ctx(tls_mode::server);
        if (!ctx.load_certificate(cert_file) || !ctx.load_private_key(key_file)) {
            throw std::runtime_error("Failed to load server certificate/key");
        }
        return ctx;
    }
    
private:
    static std::string get_ssl_error() {
        char buf[256];
        ERR_error_string_n(ERR_get_error(), buf, sizeof(buf));
        return std::string(buf);
    }
    
    static int alpn_select_callback(SSL* ssl, const unsigned char** out, unsigned char* outlen,
                                    const unsigned char* in, unsigned int inlen, void* arg) {
        (void)ssl;  // Unused but required by callback signature
        auto* ctx = static_cast<tls_context*>(arg);
        
        if (SSL_select_next_proto(const_cast<unsigned char**>(out), outlen,
                reinterpret_cast<const unsigned char*>(ctx->alpn_protocols_.data()),
                static_cast<unsigned>(ctx->alpn_protocols_.size()),
                in, inlen) != OPENSSL_NPN_NEGOTIATED) {
            return SSL_TLSEXT_ERR_NOACK;
        }
        return SSL_TLSEXT_ERR_OK;
    }
    
    SSL_CTX* ctx_ = nullptr;
    tls_mode mode_;
    std::string alpn_protocols_;
};

} // namespace elio::tls
