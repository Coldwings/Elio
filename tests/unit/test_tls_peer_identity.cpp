#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>

#if defined(ELIO_HAS_TLS) && ELIO_HAS_TLS && defined(ELIO_RUNTIME_TEST_HOOKS)
#include <elio/tls/tls_stream.hpp>
#include <elio/runtime/scheduler.hpp>
#include <elio/sync/event.hpp>

#include <openssl/rsa.h>
#include <openssl/x509v3.h>
#include <array>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/socket.h>
#include <unistd.h>

namespace {
using namespace elio;
using identity_backend = io::io_context::backend_type;

struct identity_backend_guard {
    identity_backend previous;
    explicit identity_backend_guard(identity_backend selected)
        : previous(runtime::detail::worker_io_backend_for_test.load()) {
#if ELIO_HAS_IO_URING
        if (selected == identity_backend::io_uring && !io::io_uring_backend::is_available())
            SKIP("io_uring unavailable on this host");
#else
        if (selected == identity_backend::io_uring) SKIP("io_uring support not compiled");
#endif
        runtime::detail::worker_io_backend_for_test.store(selected);
    }
    ~identity_backend_guard() { runtime::detail::worker_io_backend_for_test.store(previous); }
};

void install_identity_certificate(tls::tls_context& server, tls::tls_context& client) {
    std::unique_ptr<EVP_PKEY_CTX, decltype(&EVP_PKEY_CTX_free)> generator(
        EVP_PKEY_CTX_new_id(EVP_PKEY_RSA, nullptr), EVP_PKEY_CTX_free);
    if (!generator || EVP_PKEY_keygen_init(generator.get()) <= 0 ||
        EVP_PKEY_CTX_set_rsa_keygen_bits(generator.get(), 2048) <= 0)
        throw std::runtime_error("TLS identity key setup failed");
    EVP_PKEY* raw = nullptr;
    if (EVP_PKEY_keygen(generator.get(), &raw) <= 0)
        throw std::runtime_error("TLS identity key generation failed");
    std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)> key(raw, EVP_PKEY_free);
    std::unique_ptr<X509, decltype(&X509_free)> certificate(X509_new(), X509_free);
    if (!certificate || X509_set_version(certificate.get(), 2) != 1 ||
        ASN1_INTEGER_set(X509_get_serialNumber(certificate.get()), 1) != 1 ||
        !X509_gmtime_adj(X509_getm_notBefore(certificate.get()), -60) ||
        !X509_gmtime_adj(X509_getm_notAfter(certificate.get()), 3600) ||
        X509_set_pubkey(certificate.get(), key.get()) != 1)
        throw std::runtime_error("TLS identity certificate setup failed");
    auto* subject = X509_get_subject_name(certificate.get());
    std::unique_ptr<X509_EXTENSION, decltype(&X509_EXTENSION_free)> san(
        X509V3_EXT_conf_nid(nullptr, nullptr, NID_subject_alt_name,
            const_cast<char*>("IP:127.0.0.1,IP:::1,DNS:localhost,DNS:127.0.0.2")),
        X509_EXTENSION_free);
    if (!subject || !san || X509_NAME_add_entry_by_txt(subject, "CN", MBSTRING_ASC,
        reinterpret_cast<const unsigned char*>("localhost"), -1, -1, 0) != 1 ||
        X509_set_issuer_name(certificate.get(), subject) != 1 ||
        X509_add_ext(certificate.get(), san.get(), -1) != 1 ||
        X509_sign(certificate.get(), key.get(), EVP_sha256()) <= 0 ||
        SSL_CTX_use_certificate(server.native_handle(), certificate.get()) != 1 ||
        SSL_CTX_use_PrivateKey(server.native_handle(), key.get()) != 1 ||
        X509_STORE_add_cert(SSL_CTX_get_cert_store(client.native_handle()), certificate.get()) != 1)
        throw std::runtime_error("TLS identity certificate publication failed");
    client.set_verify_mode(tls::verify_mode::peer);
}

int observe_identity_sni(SSL* session, int*, void* context) noexcept {
    try {
        if (const auto* name = SSL_get_servername(session, TLSEXT_NAMETYPE_host_name))
            *static_cast<std::string*>(context) = name;
        return SSL_TLSEXT_ERR_OK;
    } catch (...) { return SSL_TLSEXT_ERR_ALERT_FATAL; }
}

coro::task<void> serve_identity(tls::tls_stream& stream, coro::cancel_token token) {
    if (co_await stream.handshake(token)) {
        sync::event ended;
        co_await ended.wait(token);
    }
    co_await stream.abort_and_settle();
}

struct identity_result {
    bool authenticated;
    long verification;
    int error;
};

coro::task<identity_result> check_identity(tls::tls_stream& stream) {
    const bool authenticated = co_await stream.handshake();
    const auto verification = stream.verify_result();
    const auto error = authenticated ? 0 : errno;
    co_await stream.abort_and_settle();
    co_return identity_result{authenticated, verification, error};
}

struct identity_case {
    std::string_view previous;
    std::string_view reference;
    long verification;
    std::string_view sni;
};
} // namespace

TEST_CASE("TLS references distinguish IP SANs from DNS SANs and replace previous identities",
          "[tls][identity][issue-1270]") {
    const auto selected = GENERATE(identity_backend::epoll, identity_backend::io_uring);
    const auto version = GENERATE(tls::tls_version::tls_1_2, tls::tls_version::tls_1_3_only);
    identity_backend_guard backend_scope(selected);
    tls::tls_context server_context(tls::tls_mode::server, version);
    tls::tls_context client_context(tls::tls_mode::client, version);
    install_identity_certificate(server_context, client_context);
    const std::array cases{
        identity_case{"", "127.0.0.1", X509_V_OK, ""},
        identity_case{"", "::1", X509_V_OK, ""},
        identity_case{"", "127.0.0.2", X509_V_ERR_IP_ADDRESS_MISMATCH, ""},
        identity_case{"", "::2", X509_V_ERR_IP_ADDRESS_MISMATCH, ""},
        identity_case{"", "localhost", X509_V_OK, "localhost"},
        identity_case{"wrong.invalid", "127.0.0.1", X509_V_OK, ""},
        identity_case{"127.0.0.2", "localhost", X509_V_OK, "localhost"},
        identity_case{"127.0.0.1", "127.0.0.2", X509_V_ERR_IP_ADDRESS_MISMATCH, ""},
        identity_case{"localhost", "wrong.invalid", X509_V_ERR_HOSTNAME_MISMATCH, "wrong.invalid"},
        identity_case{"localhost", "", X509_V_OK, ""}
    };
    for (const auto& item : cases) {
        CAPTURE(selected, version, item.previous, item.reference);
        std::array<int, 2> descriptors{};
        REQUIRE(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC,
                             0, descriptors.data()) == 0);
        net::tcp_stream client_lower(descriptors[0]);
        net::tcp_stream server_lower(descriptors[1]);
        tls::tls_stream client(std::move(client_lower), client_context);
        tls::tls_stream server(std::move(server_lower), server_context);
        if (!item.previous.empty()) client.set_hostname(item.previous);
        client.set_hostname(item.reference);
        std::string sni;
        SSL_CTX_set_tlsext_servername_callback(server_context.native_handle(), observe_identity_sni);
        SSL_CTX_set_tlsext_servername_arg(server_context.native_handle(), &sni);
        coro::cancel_source stop;
        runtime::scheduler scheduler(1);
        scheduler.start();
        auto serving = scheduler.go_joinable(serve_identity(server, stop.get_token()));
        auto checking = scheduler.go_joinable(check_identity(client));
        checking.wait_destroyed();
        stop.cancel();
        serving.wait_destroyed();
        scheduler.shutdown();
        const auto result = checking.await_resume();
        serving.await_resume();
        CHECK(result.authenticated == (item.verification == X509_V_OK));
        CHECK(result.verification == item.verification);
        CHECK(sni == item.sni);
    }
}

TEST_CASE("TLS peer reference rejects embedded NUL without permitting a later handshake",
          "[tls][identity][issue-1270]") {
    std::array<int, 2> descriptors{};
    REQUIRE(::socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC,
                         0, descriptors.data()) == 0);
    net::tcp_stream peer(descriptors[1]);
    tls::tls_context context(tls::tls_mode::client);
    tls::tls_stream stream(net::tcp_stream{descriptors[0]}, context);
    REQUIRE_THROWS_AS(stream.set_hostname(std::string_view("127.0.0.1\0evil", 14)),
                      std::invalid_argument);
    stream.set_hostname("localhost");
    runtime::scheduler scheduler(1);
    scheduler.start();
    auto checking = scheduler.go_joinable(check_identity(stream));
    checking.wait_destroyed();
    scheduler.shutdown();
    const auto result = checking.await_resume();
    CHECK_FALSE(result.authenticated);
    CHECK(result.error == EINVAL);
}
#endif
