#pragma once

/// @file websocket_handshake.hpp
/// @brief WebSocket handshake utilities (RFC 6455)
///
/// This file provides utilities for WebSocket connection handshake,
/// including Sec-WebSocket-Key generation and validation.

#include <elio/http/http_common.hpp>

#include <string>
#include <string_view>
#include <array>
#include <algorithm>
#include <cstdio>
#include <cstdint>
#include <stdexcept>
#include <sys/random.h>
#include <vector>

#include <openssl/sha.h>
#include <openssl/evp.h>

namespace elio::http::websocket {

/// WebSocket GUID used in handshake (RFC 6455 Section 1.3)
inline constexpr std::string_view WS_GUID = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";

/// Base64 encoding helper
inline std::string base64_encode(const uint8_t* data, size_t len) {
    static const char table[] = 
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    
    std::string result;
    result.reserve(((len + 2) / 3) * 4);
    
    for (size_t i = 0; i < len; i += 3) {
        uint32_t octet_a = i < len ? data[i] : 0;
        uint32_t octet_b = i + 1 < len ? data[i + 1] : 0;
        uint32_t octet_c = i + 2 < len ? data[i + 2] : 0;
        
        uint32_t triple = (octet_a << 16) | (octet_b << 8) | octet_c;
        
        result += table[(triple >> 18) & 0x3F];
        result += table[(triple >> 12) & 0x3F];
        result += (i + 1 < len) ? table[(triple >> 6) & 0x3F] : '=';
        result += (i + 2 < len) ? table[triple & 0x3F] : '=';
    }
    
    return result;
}

/// Base64 decoding helper
inline std::string base64_decode(std::string_view encoded) {
    static const int decode_table[] = {
        -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
        -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
        -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, 62, -1, -1, -1, 63,
        52, 53, 54, 55, 56, 57, 58, 59, 60, 61, -1, -1, -1, -1, -1, -1,
        -1,  0,  1,  2,  3,  4,  5,  6,  7,  8,  9, 10, 11, 12, 13, 14,
        15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, -1, -1, -1, -1, -1,
        -1, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37, 38, 39, 40,
        41, 42, 43, 44, 45, 46, 47, 48, 49, 50, 51, -1, -1, -1, -1, -1
    };
    
    std::string result;
    result.reserve(encoded.size() * 3 / 4);
    
    uint32_t buffer = 0;
    int bits_collected = 0;
    
    for (char c : encoded) {
        if (c == '=') break;
        if (static_cast<unsigned char>(c) >= 128) continue;
        int value = decode_table[static_cast<unsigned char>(c)];
        if (value < 0) continue;
        
        buffer = (buffer << 6) | static_cast<uint32_t>(value);
        bits_collected += 6;
        
        if (bits_collected >= 8) {
            bits_collected -= 8;
            result += static_cast<char>((buffer >> bits_collected) & 0xFF);
        }
    }
    
    return result;
}

namespace detail {

inline int base64_value(unsigned char c) noexcept {
    if (c >= 'A' && c <= 'Z') return static_cast<int>(c - 'A');
    if (c >= 'a' && c <= 'z') return static_cast<int>(c - 'a') + 26;
    if (c >= '0' && c <= '9') return static_cast<int>(c - '0') + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

/// Strict Base64 decoder used for protocol validation.  Unlike base64_decode(),
/// this rejects skipped characters, misplaced padding, and non-zero pad bits.
inline bool base64_decode_strict(std::string_view encoded, std::string& decoded) {
    decoded.clear();
    if (encoded.empty() || encoded.size() % 4 != 0) return false;

    size_t padding = 0;
    if (!encoded.empty() && encoded.back() == '=') {
        padding = 1;
        if (encoded.size() >= 2 && encoded[encoded.size() - 2] == '=') {
            padding = 2;
        }
    }

    decoded.reserve((encoded.size() / 4) * 3 - padding);

    for (size_t i = 0; i < encoded.size(); i += 4) {
        const bool final_block = i + 4 == encoded.size();
        unsigned char c0 = static_cast<unsigned char>(encoded[i]);
        unsigned char c1 = static_cast<unsigned char>(encoded[i + 1]);
        unsigned char c2 = static_cast<unsigned char>(encoded[i + 2]);
        unsigned char c3 = static_cast<unsigned char>(encoded[i + 3]);

        int v0 = base64_value(c0);
        int v1 = base64_value(c1);
        if (v0 < 0 || v1 < 0) return false;

        if (c2 == '=') {
            if (!final_block || c3 != '=') return false;
            if ((v1 & 0x0F) != 0) return false;
            decoded += static_cast<char>((v0 << 2) | (v1 >> 4));
            continue;
        }

        int v2 = base64_value(c2);
        if (v2 < 0) return false;

        if (c3 == '=') {
            if (!final_block) return false;
            if ((v2 & 0x03) != 0) return false;
            decoded += static_cast<char>((v0 << 2) | (v1 >> 4));
            decoded += static_cast<char>(((v1 & 0x0F) << 4) | (v2 >> 2));
            continue;
        }

        int v3 = base64_value(c3);
        if (v3 < 0) return false;

        decoded += static_cast<char>((v0 << 2) | (v1 >> 4));
        decoded += static_cast<char>(((v1 & 0x0F) << 4) | (v2 >> 2));
        decoded += static_cast<char>(((v2 & 0x03) << 6) | v3);
    }

    return true;
}

} // namespace detail

inline bool is_valid_websocket_key(std::string_view key) {
    if (key.size() != 24) return false;

    std::string decoded;
    return detail::base64_decode_strict(key, decoded) && decoded.size() == 16;
}

/// Generate a random 16-byte Sec-WebSocket-Key (base64 encoded).
/// Uses getrandom(2) for cryptographic randomness per RFC 6455 §5.3.
inline std::string generate_websocket_key() {
    std::array<uint8_t, 16> key{};
    ssize_t n = ::getrandom(key.data(), key.size(), 0);
    if (n < static_cast<ssize_t>(key.size())) {
        // Fallback: should never happen on Linux 3.17+
        FILE* f = fopen("/dev/urandom", "rb");
        if (!f) {
            throw std::runtime_error("Failed to open /dev/urandom for WebSocket key generation");
        }
        auto rd = fread(key.data(), 1, key.size(), f);
        fclose(f);
        if (rd != key.size()) {
            throw std::runtime_error("Failed to read sufficient entropy for WebSocket key");
        }
    }

    return base64_encode(key.data(), key.size());
}

/// Compute Sec-WebSocket-Accept from Sec-WebSocket-Key
/// @param key The client's Sec-WebSocket-Key header value
/// @return The Sec-WebSocket-Accept value for the server response
inline std::string compute_websocket_accept(std::string_view key) {
    // Concatenate key with GUID
    std::string concat;
    concat.reserve(key.size() + WS_GUID.size());
    concat.append(key);
    concat.append(WS_GUID);
    
    // SHA-1 hash
    std::array<uint8_t, SHA_DIGEST_LENGTH> hash;
    SHA1(reinterpret_cast<const uint8_t*>(concat.data()), 
         concat.size(), hash.data());
    
    // Base64 encode
    return base64_encode(hash.data(), hash.size());
}

/// Verify Sec-WebSocket-Accept matches expected value
/// @param accept The server's Sec-WebSocket-Accept header value
/// @param key The client's original Sec-WebSocket-Key
/// @return true if accept value is valid
inline bool verify_websocket_accept(std::string_view accept, std::string_view key) {
    return accept == compute_websocket_accept(key);
}

/// WebSocket subprotocol negotiation result
struct protocol_negotiation {
    std::string selected_protocol;  ///< Selected subprotocol (empty if none)
    bool success = false;           ///< Negotiation successful
};

/// Parse Sec-WebSocket-Protocol header (comma-separated list)
inline std::vector<std::string> parse_protocols(std::string_view protocols) {
    std::vector<std::string> result;
    
    size_t start = 0;
    while (start < protocols.size()) {
        // Skip whitespace
        while (start < protocols.size() && 
               (protocols[start] == ' ' || protocols[start] == '\t')) {
            ++start;
        }
        
        // Find end of token
        size_t end = protocols.find(',', start);
        if (end == std::string_view::npos) {
            end = protocols.size();
        }
        
        // Trim trailing whitespace
        size_t token_end = end;
        while (token_end > start && 
               (protocols[token_end - 1] == ' ' || protocols[token_end - 1] == '\t')) {
            --token_end;
        }
        
        if (token_end > start) {
            result.emplace_back(protocols.substr(start, token_end - start));
        }
        
        start = end + 1;
    }
    
    return result;
}

/// Negotiate subprotocol from client and server lists
/// @param client_protocols Client's Sec-WebSocket-Protocol list
/// @param server_protocols Server's supported protocols
/// @return First matching protocol, or empty if none
inline std::string negotiate_protocol(const std::vector<std::string>& client_protocols,
                                      const std::vector<std::string>& server_protocols) {
    for (const auto& client_proto : client_protocols) {
        for (const auto& server_proto : server_protocols) {
            if (client_proto == server_proto) {
                return client_proto;
            }
        }
    }
    return "";
}

namespace detail {

inline bool header_has_token(std::string_view value, std::string_view token) {
    size_t pos = 0;
    while (pos <= value.size()) {
        auto comma = value.find(',', pos);
        auto part = comma == std::string_view::npos
            ? value.substr(pos)
            : value.substr(pos, comma - pos);
        part = ::elio::http::detail::trim_ows(part);

        if (::elio::http::detail::ascii_iequals(part, token)) {
            return true;
        }

        if (comma == std::string_view::npos) break;
        pos = comma + 1;
    }

    return false;
}

inline void validate_client_handshake_value(bool valid,
                                            std::string_view field) {
    if (!valid) {
        throw std::invalid_argument(
            "elio::http::websocket: invalid client handshake " +
            std::string(field));
    }
}

} // namespace detail

/// WebSocket handshake request info
struct handshake_request {
    std::string key;                      ///< Sec-WebSocket-Key
    std::string version;                  ///< Sec-WebSocket-Version (should be "13")
    std::vector<std::string> protocols;   ///< Sec-WebSocket-Protocol list
    std::vector<std::string> extensions;  ///< Sec-WebSocket-Extensions list
    std::string origin;                   ///< Origin header (for browser clients)
    std::string host;                     ///< Host header
    std::string path;                     ///< Request path
};

/// Check if HTTP request is a valid WebSocket upgrade request
/// @param method HTTP method (must be GET)
/// @param upgrade Upgrade header value
/// @param connection Connection header value
/// @param version Sec-WebSocket-Version header value
/// @return true if request is a valid WebSocket upgrade
inline bool is_websocket_upgrade(std::string_view method,
                                 std::string_view upgrade,
                                 std::string_view connection,
                                 std::string_view version) {
    // Must be GET
    if (method != "GET") return false;
    
    // Upgrade header is a comma-separated list of protocol tokens.
    if (!detail::header_has_token(upgrade, "websocket")) return false;
    
    // Connection header is a comma-separated list of hop-by-hop option tokens.
    if (!detail::header_has_token(connection, "upgrade")) return false;
    
    // Version must be "13"
    if (version != "13") return false;
    
    return true;
}

/// Build WebSocket client handshake request headers
/// @param host Host header value
/// @param path Request path
/// @param key Sec-WebSocket-Key (generate if empty)
/// @param protocols Optional subprotocols
/// @param origin Optional origin
/// @return HTTP request string
inline std::string build_client_handshake(std::string_view host,
                                          std::string_view path,
                                          std::string_view key,
                                          const std::vector<std::string>& protocols = {},
                                          std::string_view origin = "") {
    detail::validate_client_handshake_value(
        ::elio::http::detail::is_valid_url_input(host), "host");
    detail::validate_client_handshake_value(
        path.empty() || ::elio::http::detail::is_valid_request_target(path),
        "path");
    detail::validate_client_handshake_value(
        ::elio::http::detail::is_valid_header_value(key), "key");
    detail::validate_client_handshake_value(
        origin.empty() || ::elio::http::detail::is_valid_header_value(origin),
        "origin");
    for (const auto& protocol : protocols) {
        detail::validate_client_handshake_value(
            ::elio::http::detail::is_valid_token(protocol),
            "subprotocol");
    }

    std::string request;
    request.reserve(512);
    
    // Request line
    request += "GET ";
    request += path.empty() ? "/" : path;
    request += " HTTP/1.1\r\n";
    
    // Required headers
    request += "Host: ";
    request += host;
    request += "\r\n";
    
    request += "Upgrade: websocket\r\n";
    request += "Connection: Upgrade\r\n";
    
    request += "Sec-WebSocket-Key: ";
    request += key;
    request += "\r\n";
    
    request += "Sec-WebSocket-Version: 13\r\n";
    
    // Optional headers
    if (!protocols.empty()) {
        request += "Sec-WebSocket-Protocol: ";
        for (size_t i = 0; i < protocols.size(); ++i) {
            if (i > 0) request += ", ";
            request += protocols[i];
        }
        request += "\r\n";
    }
    
    if (!origin.empty()) {
        request += "Origin: ";
        request += origin;
        request += "\r\n";
    }
    
    request += "\r\n";
    return request;
}

/// Build WebSocket server handshake response
/// @param accept Sec-WebSocket-Accept value
/// @param protocol Selected subprotocol (empty if none)
/// @return HTTP response string
inline std::string build_server_handshake(std::string_view accept,
                                          std::string_view protocol = "") {
    std::string response;
    response.reserve(256);
    
    response += "HTTP/1.1 101 Switching Protocols\r\n";
    response += "Upgrade: websocket\r\n";
    response += "Connection: Upgrade\r\n";
    response += "Sec-WebSocket-Accept: ";
    response += accept;
    response += "\r\n";
    
    if (!protocol.empty()) {
        response += "Sec-WebSocket-Protocol: ";
        response += protocol;
        response += "\r\n";
    }
    
    response += "\r\n";
    return response;
}

/// Build WebSocket upgrade rejection response
inline std::string build_rejection_response(uint16_t status_code = 400,
                                            std::string_view reason = "Bad Request") {
    std::string response;
    response += "HTTP/1.1 ";
    response += std::to_string(status_code);
    response += " ";
    response += reason;
    response += "\r\n";
    response += "Connection: close\r\n";
    response += "\r\n";
    return response;
}

} // namespace elio::http::websocket
