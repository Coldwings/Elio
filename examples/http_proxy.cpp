#include <elio/elio.hpp>
#include <elio/http/http_client.hpp>

#include <chrono>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>

elio::coro::task<int> async_main(int argc, char** argv) {
    using namespace elio;
    if (argc < 3 || argc > 5) {
        std::cerr << "Usage: http_proxy http[s]://proxy-host:port http[s]://origin/path"
                     " [origin-ca.pem|-] [proxy-ca.pem|-]\n";
        co_return 2;
    }
    http::transport_config config;
    config.proxy.emplace();
    config.proxy->endpoint = argv[1];
    // If needed, load explicit proxy.basic_auth from a caller-owned secret store,
    // not a URI or process argument. configure_tls customizes origin trust.
    if (argc >= 4 && std::string_view(argv[3]) != "-") {
        config.configure_tls = [ca = std::string(argv[3])](http::transport_tls_config& policy) {
            if (!policy.load_verify_locations(ca))
                throw std::runtime_error("Origin trust initialization failed");
        };
    }
    if (argc == 5 && std::string_view(argv[4]) != "-") {
        config.proxy->configure_tls = [ca = std::string(argv[4])](http::transport_tls_config& policy) {
            if (!policy.load_verify_locations(ca))
                throw std::runtime_error("Proxy trust initialization failed");
        };
    }
    config.limits = http::pool_limits{};
    config.acquisition_timeout = std::chrono::seconds(10);
    std::shared_ptr<http::transport> owner;
    try { owner = std::make_shared<http::transport>(config); }
    catch (const std::exception&) {
        std::cerr << "Invalid explicit proxy or TLS configuration\n";
        co_return 2;
    }
    http::client client(owner);
    auto result = co_await client.get_result(argv[2]);
    if (const auto* error = std::get_if<http::client_error>(&result)) {
        std::cerr << "Request failed: " << error->code.value()
                  << " at stage " << static_cast<int>(error->stage) << '\n';
        co_return 1;
    }
    std::cout << "HTTP status: " << std::get<http::response>(result).status_code() << '\n';
    (void)co_await owner->shutdown();
    co_return 0;
}

ELIO_ASYNC_MAIN(async_main)
