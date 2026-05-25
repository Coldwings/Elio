#pragma once

/// @file rpc_endian.hpp
/// @brief Little-endian byte-order helpers for the RPC wire format.
///
/// The RPC wire format is documented as little-endian. Historically the
/// implementation just memcpy'd native-order bytes, which silently broke
/// interop with big-endian peers (e.g. s390x). These helpers convert
/// to/from LE explicitly so the wire format matches the documentation
/// regardless of host endianness.
///
/// On x86 / ARM64-LE (the overwhelmingly common case) every helper is a
/// trivial pass-through and is fully eliminated by the optimizer.

#include <bit>
#include <cstdint>
#include <cstring>
#include <type_traits>

namespace elio::rpc::endian {

namespace detail {

template<typename T>
constexpr T integral_byteswap(T x) noexcept {
    static_assert(std::is_integral_v<T>);
    if constexpr (sizeof(T) == 1) {
        return x;
    } else if constexpr (sizeof(T) == 2) {
        return static_cast<T>(__builtin_bswap16(static_cast<uint16_t>(x)));
    } else if constexpr (sizeof(T) == 4) {
        return static_cast<T>(__builtin_bswap32(static_cast<uint32_t>(x)));
    } else if constexpr (sizeof(T) == 8) {
        return static_cast<T>(__builtin_bswap64(static_cast<uint64_t>(x)));
    } else {
        static_assert(sizeof(T) == 0, "Unsupported integral size for byteswap");
    }
}

template<typename T>
constexpr T integral_host_to_le(T x) noexcept {
    if constexpr (std::endian::native == std::endian::little) {
        return x;
    } else {
        return integral_byteswap(x);
    }
}

template<typename T>
constexpr T integral_le_to_host(T x) noexcept {
    return integral_host_to_le(x);  // symmetric
}

} // namespace detail

/// Write `value` as little-endian bytes into `dst`.
/// Supports integral, floating-point, enum, and bool types.
template<typename T>
inline void write_le(void* dst, T value) noexcept {
    if constexpr (std::is_same_v<T, bool>) {
        uint8_t b = value ? 1 : 0;
        std::memcpy(dst, &b, 1);
    } else if constexpr (std::is_enum_v<T>) {
        using U = std::underlying_type_t<T>;
        U bits = detail::integral_host_to_le(static_cast<U>(value));
        std::memcpy(dst, &bits, sizeof(U));
    } else if constexpr (std::is_floating_point_v<T>) {
        if constexpr (sizeof(T) == 4) {
            uint32_t bits;
            std::memcpy(&bits, &value, 4);
            bits = detail::integral_host_to_le(bits);
            std::memcpy(dst, &bits, 4);
        } else if constexpr (sizeof(T) == 8) {
            uint64_t bits;
            std::memcpy(&bits, &value, 8);
            bits = detail::integral_host_to_le(bits);
            std::memcpy(dst, &bits, 8);
        } else {
            static_assert(sizeof(T) == 0, "Unsupported floating-point size");
        }
    } else if constexpr (std::is_integral_v<T>) {
        T le = detail::integral_host_to_le(value);
        std::memcpy(dst, &le, sizeof(T));
    } else {
        // Aggregate / trivially-copyable struct: bytes are already in
        // their canonical layout, no swap.
        std::memcpy(dst, &value, sizeof(T));
    }
}

/// Read a little-endian-encoded T from `src`.
template<typename T>
inline T read_le(const void* src) noexcept {
    if constexpr (std::is_same_v<T, bool>) {
        uint8_t b;
        std::memcpy(&b, src, 1);
        return b != 0;
    } else if constexpr (std::is_enum_v<T>) {
        using U = std::underlying_type_t<T>;
        U bits;
        std::memcpy(&bits, src, sizeof(U));
        return static_cast<T>(detail::integral_le_to_host(bits));
    } else if constexpr (std::is_floating_point_v<T>) {
        if constexpr (sizeof(T) == 4) {
            uint32_t bits;
            std::memcpy(&bits, src, 4);
            bits = detail::integral_le_to_host(bits);
            T out;
            std::memcpy(&out, &bits, 4);
            return out;
        } else if constexpr (sizeof(T) == 8) {
            uint64_t bits;
            std::memcpy(&bits, src, 8);
            bits = detail::integral_le_to_host(bits);
            T out;
            std::memcpy(&out, &bits, 8);
            return out;
        } else {
            static_assert(sizeof(T) == 0, "Unsupported floating-point size");
        }
    } else if constexpr (std::is_integral_v<T>) {
        T bits;
        std::memcpy(&bits, src, sizeof(T));
        return detail::integral_le_to_host(bits);
    } else {
        T out;
        std::memcpy(&out, src, sizeof(T));
        return out;
    }
}

} // namespace elio::rpc::endian
