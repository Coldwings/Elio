#pragma once

#include <elio/http/http_common.hpp>
#include <cstdint>
#include <optional>

namespace elio::http {

/// Response metadata, independent of body storage and production.
class response_head {
public:
    response_head() = default;
    explicit response_head(status value) : status_(value) {}

    status get_status() const noexcept { return status_; }
    void set_status(status value) noexcept { status_ = value; }
    uint16_t status_code() const noexcept { return static_cast<uint16_t>(status_); }
    std::string_view version() const noexcept { return version_; }
    void set_version(std::string_view value) {
        detail::validate_http_version(value);
        version_ = value;
    }
    const headers& get_headers() const noexcept { return headers_; }
    headers& get_headers() noexcept { return headers_; }
    void set_header(std::string_view name, std::string_view value) {
        headers_.set(name, value);
    }
    std::string_view header(std::string_view name) const { return headers_.get(name); }
    std::string_view content_type() const { return headers_.content_type(); }
    void set_content_type(std::string_view value) { headers_.set_content_type(value); }

    /// Hypothetical representation length for HEAD and 304, not a body size.
    std::optional<uint64_t> representation_length() const noexcept {
        return representation_length_;
    }
    /// Caller asserts the size of the corresponding selected representation.
    /// Preflight checks framing consistency, not whether a hypothetical GET
    /// would produce this size; it does not invoke the producer to verify it.
    void set_representation_length(std::optional<uint64_t> value) noexcept {
        representation_length_ = value;
    }
    bool is_success() const noexcept { return status_code() >= 200 && status_code() < 300; }
    bool is_redirect() const noexcept { return status_code() >= 300 && status_code() < 400; }
    bool is_client_error() const noexcept { return status_code() >= 400 && status_code() < 500; }
    bool is_server_error() const noexcept { return status_code() >= 500 && status_code() < 600; }

private:
    status status_ = status::ok;
    std::string version_ = "HTTP/1.1";
    headers headers_;
    std::optional<uint64_t> representation_length_;
};

} // namespace elio::http
