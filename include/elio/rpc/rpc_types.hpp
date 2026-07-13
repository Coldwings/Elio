#pragma once

/// @file rpc_types.hpp
/// @brief Type system for RPC schema definition
///
/// This module provides C++ template-based schema definition for RPC messages.
/// It supports:
/// - Primitive types (integers, floats, bool)
/// - Variable-length types (strings, byte arrays)
/// - Containers (arrays, maps)
/// - Nested structures
/// - Optional fields
///
/// Usage:
/// @code
/// struct MyMessage {
///     int32_t id;
///     std::string name;
///     std::vector<int32_t> values;
///     
///     // Required for serialization
///     ELIO_RPC_FIELDS(id, name, values)
/// };
/// @endcode

#include "rpc_buffer.hpp"

#include <array>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <tuple>
#include <type_traits>
#include <unordered_map>
#include <utility>
#include <vector>

namespace elio::rpc {

// ============================================================================
// Type traits
// ============================================================================

/// Check if T is a primitive type that can be directly memcpy'd
template<typename T>
struct is_primitive : std::bool_constant<
    std::is_arithmetic_v<T> || std::is_enum_v<T>> {};

template<typename T>
inline constexpr bool is_primitive_v = is_primitive<T>::value;

/// Check if T is a string type
template<typename T>
struct is_string_type : std::false_type {};

template<>
struct is_string_type<std::string> : std::true_type {};

template<>
struct is_string_type<std::string_view> : std::true_type {};

template<typename T>
inline constexpr bool is_string_type_v = is_string_type<T>::value;

/// Check if T is a std::vector
template<typename T>
struct is_vector : std::false_type {};

template<typename T, typename A>
struct is_vector<std::vector<T, A>> : std::true_type {};

template<typename T>
inline constexpr bool is_vector_v = is_vector<T>::value;

/// Check if T is a std::array
template<typename T>
struct is_std_array : std::false_type {};

template<typename T, size_t N>
struct is_std_array<std::array<T, N>> : std::true_type {};

template<typename T>
inline constexpr bool is_std_array_v = is_std_array<T>::value;

/// Check if T is a map type
template<typename T>
struct is_map_type : std::false_type {};

template<typename K, typename V, typename C, typename A>
struct is_map_type<std::map<K, V, C, A>> : std::true_type {};

template<typename K, typename V, typename H, typename E, typename A>
struct is_map_type<std::unordered_map<K, V, H, E, A>> : std::true_type {};

template<typename T>
inline constexpr bool is_map_type_v = is_map_type<T>::value;

/// Check if T is std::optional
template<typename T>
struct is_optional : std::false_type {};

template<typename T>
struct is_optional<std::optional<T>> : std::true_type {};

template<typename T>
inline constexpr bool is_optional_v = is_optional<T>::value;

/// Check if T is a span
template<typename T>
struct is_span : std::false_type {};

template<typename T, size_t E>
struct is_span<std::span<T, E>> : std::true_type {};

template<typename T>
inline constexpr bool is_span_v = is_span<T>::value;

/// Get element type of container
template<typename T>
struct element_type { using type = T; };

template<typename T, typename A>
struct element_type<std::vector<T, A>> { using type = T; };

template<typename T, size_t N>
struct element_type<std::array<T, N>> { using type = T; };

template<typename T, size_t E>
struct element_type<std::span<T, E>> { using type = T; };

template<typename T>
struct element_type<std::optional<T>> { using type = T; };

template<typename T>
using element_type_t = typename element_type<T>::type;

/// Get key and value types for maps
template<typename T>
struct map_types;

template<typename K, typename V, typename C, typename A>
struct map_types<std::map<K, V, C, A>> {
    using key_type = K;
    using value_type = V;
};

template<typename K, typename V, typename H, typename E, typename A>
struct map_types<std::unordered_map<K, V, H, E, A>> {
    using key_type = K;
    using value_type = V;
};

// ============================================================================
// Wire-size lower bounds (used to bound attacker-controlled count fields)
// ============================================================================

/// Minimum number of bytes any value of T occupies on the wire.
/// Used to validate length-prefixed counts before allocating containers.
/// Defaults to 1 (every type consumes at least 1 byte except for empty
/// aggregates, which we cap separately by the global hard cap).
template<typename T>
struct min_wire_size : std::integral_constant<size_t, 1> {};

template<typename T>
requires is_primitive_v<T>
struct min_wire_size<T> : std::integral_constant<size_t, sizeof(T)> {};

template<>
struct min_wire_size<std::string> : std::integral_constant<size_t, 4> {}; // length prefix

template<>
struct min_wire_size<std::string_view> : std::integral_constant<size_t, 4> {};

template<typename T, typename A>
struct min_wire_size<std::vector<T, A>> : std::integral_constant<size_t, 4> {};

template<typename T, size_t N>
struct min_wire_size<std::array<T, N>>
    : std::integral_constant<size_t, N * min_wire_size<T>::value> {};

template<typename K, typename V, typename C, typename A>
struct min_wire_size<std::map<K, V, C, A>> : std::integral_constant<size_t, 4> {};

template<typename K, typename V, typename H, typename E, typename A>
struct min_wire_size<std::unordered_map<K, V, H, E, A>>
    : std::integral_constant<size_t, 4> {};

template<typename T>
struct min_wire_size<std::optional<T>> : std::integral_constant<size_t, 1> {};

template<typename T>
inline constexpr size_t min_wire_size_v = min_wire_size<T>::value;

/// Hard upper bound on container element counts. Even when the wire offers
/// many bytes (e.g. nested empty aggregates), we never allocate more than
/// this many container slots from a single deserialize call. Mirrors the
/// per-frame cap so a single message cannot trigger arbitrary RAM use.
inline constexpr size_t max_container_count = max_message_size;

/// Validate a wire-supplied count against the bytes left in the buffer.
/// Throws if the count would require more elements than the buffer could
/// possibly contain, or exceeds the global hard cap.
template<size_t MinElemSize>
inline void check_container_count(uint32_t count, size_t remaining) {
    if (count > max_container_count) {
        throw serialization_error("container count exceeds max_container_count");
    }
    if constexpr (MinElemSize > 0) {
        if (static_cast<size_t>(count) > remaining / MinElemSize) {
            throw serialization_error("container count exceeds remaining buffer");
        }
    }
}

// ============================================================================
// RPC struct field introspection
// ============================================================================

/// Marker type to detect if a struct has RPC field definitions
struct rpc_fields_marker {};

/// Check if T has ELIO_RPC_FIELDS defined
template<typename T, typename = void>
struct has_rpc_fields : std::false_type {};

template<typename T>
struct has_rpc_fields<T, std::void_t<typename T::_elio_rpc_fields_tag>> : std::true_type {};

template<typename T>
inline constexpr bool has_rpc_fields_v = has_rpc_fields<T>::value;

/// Field descriptor for compile-time reflection
template<typename T, typename Class>
struct field_descriptor {
    using value_type = T;
    using class_type = Class;
    
    T Class::* ptr;
    const char* name;
    
    constexpr field_descriptor(T Class::* p, const char* n) : ptr(p), name(n) {}
    
    const T& get(const Class& obj) const { return obj.*ptr; }
    T& get(Class& obj) const { return obj.*ptr; }
    void set(Class& obj, T value) const { obj.*ptr = std::move(value); }
};

/// Helper to create field descriptor
template<typename T, typename Class>
constexpr auto make_field(T Class::* ptr, const char* name) {
    return field_descriptor<T, Class>{ptr, name};
}

// ============================================================================
// Serialization
// ============================================================================

// Forward declarations
template<typename T>
void serialize(buffer_writer& writer, const T& value);

template<typename T>
void deserialize(buffer_view& reader, T& value);

/// Serialize primitive types
template<typename T>
requires is_primitive_v<T>
void serialize_impl(buffer_writer& writer, const T& value) {
    writer.write(value);
}

/// Deserialize primitive types
template<typename T>
requires is_primitive_v<T>
void deserialize_impl(buffer_view& reader, T& value) {
    value = reader.read<T>();
}

/// Serialize string types
inline void serialize_impl(buffer_writer& writer, const std::string& value) {
    writer.write_string(value);
}

inline void serialize_impl(buffer_writer& writer, std::string_view value) {
    writer.write_string(value);
}

/// Deserialize string (into std::string)
inline void deserialize_impl(buffer_view& reader, std::string& value) {
    std::string_view sv = reader.read_string();
    value.assign(sv.data(), sv.size());
}

/// Serialize byte span (blob)
inline void serialize_impl(buffer_writer& writer, std::span<const uint8_t> value) {
    writer.write_blob(value);
}

/// Deserialize into vector<uint8_t>
inline void deserialize_impl(buffer_view& reader, std::vector<uint8_t>& value) {
    auto span = reader.read_blob();
    value.assign(span.begin(), span.end());
}

/// Serialize buffer_ref by copying referenced bytes into the writer.
inline void serialize_impl(buffer_writer& writer, const buffer_ref& ref) {
    writer.write_blob(ref.span());
}

/// Deserialize buffer_ref (returns view into received buffer - zero copy)
inline void deserialize_impl(buffer_view& reader, buffer_ref& ref) {
    auto span = reader.read_blob();
    ref = buffer_ref(span);
}

/// Serialize vector
template<typename T, typename A>
void serialize_impl(buffer_writer& writer, const std::vector<T, A>& vec) {
    writer.write_array_size(static_cast<uint32_t>(vec.size()));
    for (const auto& elem : vec) {
        serialize(writer, elem);
    }
}

/// Deserialize vector
template<typename T, typename A>
void deserialize_impl(buffer_view& reader, std::vector<T, A>& vec) {
    uint32_t count = reader.read_array_size();
    // Bound count against attacker-controlled DoS: a malicious peer could
    // send count=0xFFFFFFFF to force a 4G * sizeof(T) allocation.
    check_container_count<min_wire_size_v<T>>(count, reader.remaining());
    vec.clear();
    vec.reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
        T elem;
        deserialize(reader, elem);
        vec.push_back(std::move(elem));
    }
}

/// Serialize std::array
template<typename T, size_t N>
void serialize_impl(buffer_writer& writer, const std::array<T, N>& arr) {
    // Fixed-size array doesn't need size prefix
    for (const auto& elem : arr) {
        serialize(writer, elem);
    }
}

/// Deserialize std::array
template<typename T, size_t N>
void deserialize_impl(buffer_view& reader, std::array<T, N>& arr) {
    for (auto& elem : arr) {
        deserialize(reader, elem);
    }
}

/// Serialize map types
template<typename Map>
requires is_map_type_v<Map>
void serialize_impl(buffer_writer& writer, const Map& map) {
    writer.write_array_size(static_cast<uint32_t>(map.size()));
    for (const auto& [key, value] : map) {
        serialize(writer, key);
        serialize(writer, value);
    }
}

/// Deserialize map types
template<typename Map>
requires is_map_type_v<Map>
void deserialize_impl(buffer_view& reader, Map& map) {
    using K = typename map_types<Map>::key_type;
    using V = typename map_types<Map>::value_type;

    uint32_t count = reader.read_array_size();
    // A malicious count would force unbounded hash-bucket / node allocation.
    constexpr size_t pair_min = min_wire_size_v<K> + min_wire_size_v<V>;
    check_container_count<pair_min>(count, reader.remaining());
    map.clear();
    for (uint32_t i = 0; i < count; ++i) {
        K key;
        V value;
        deserialize(reader, key);
        deserialize(reader, value);
        map.emplace(std::move(key), std::move(value));
    }
}

/// Serialize optional
template<typename T>
void serialize_impl(buffer_writer& writer, const std::optional<T>& opt) {
    writer.write(static_cast<uint8_t>(opt.has_value() ? 1 : 0));
    if (opt.has_value()) {
        serialize(writer, *opt);
    }
}

/// Deserialize optional
template<typename T>
void deserialize_impl(buffer_view& reader, std::optional<T>& opt) {
    uint8_t has_value = reader.read<uint8_t>();
    if (has_value) {
        T value;
        deserialize(reader, value);
        opt = std::move(value);
    } else {
        opt.reset();
    }
}

/// Serialize RPC struct with fields
template<typename T>
requires has_rpc_fields_v<T>
void serialize_impl(buffer_writer& writer, const T& obj) {
    auto fields = T::_elio_rpc_get_fields();
    std::apply([&](const auto&... field) {
        (serialize(writer, field.get(obj)), ...);
    }, fields);
}

/// Deserialize RPC struct with fields
template<typename T>
requires has_rpc_fields_v<T>
void deserialize_impl(buffer_view& reader, T& obj) {
    auto fields = T::_elio_rpc_get_fields();
    std::apply([&](const auto&... field) {
        (deserialize(reader, field.get(obj)), ...);
    }, fields);
}

/// Main serialize function - dispatches to appropriate impl
template<typename T>
void serialize(buffer_writer& writer, const T& value) {
    serialize_impl(writer, value);
}

/// Main deserialize function - dispatches to appropriate impl
template<typename T>
void deserialize(buffer_view& reader, T& value) {
    deserialize_impl(reader, value);
}

/// Convenience: serialize to new buffer
template<typename T>
buffer_writer serialize(const T& value) {
    buffer_writer writer;
    serialize(writer, value);
    return writer;
}

/// Convenience: deserialize from view
template<typename T>
T deserialize(buffer_view& reader) {
    T value;
    deserialize(reader, value);
    return value;
}

/// Convenience: deserialize from data pointer
template<typename T>
T deserialize(const void* data, size_t size) {
    buffer_view reader(data, size);
    return deserialize<T>(reader);
}

// ============================================================================
// Macro for field definition
// ============================================================================

/// Internal helper macros
#define _ELIO_RPC_FIELD_IMPL(Class, field) \
    ::elio::rpc::make_field(&Class::field, #field)

#define _ELIO_RPC_EXPAND(x) x

// FOR_EACH macros that pass Class through
#define _ELIO_RPC_FOR_EACH_1(Class, macro, x) macro(Class, x)
#define _ELIO_RPC_FOR_EACH_2(Class, macro, x, ...) macro(Class, x), _ELIO_RPC_EXPAND(_ELIO_RPC_FOR_EACH_1(Class, macro, __VA_ARGS__))
#define _ELIO_RPC_FOR_EACH_3(Class, macro, x, ...) macro(Class, x), _ELIO_RPC_EXPAND(_ELIO_RPC_FOR_EACH_2(Class, macro, __VA_ARGS__))
#define _ELIO_RPC_FOR_EACH_4(Class, macro, x, ...) macro(Class, x), _ELIO_RPC_EXPAND(_ELIO_RPC_FOR_EACH_3(Class, macro, __VA_ARGS__))
#define _ELIO_RPC_FOR_EACH_5(Class, macro, x, ...) macro(Class, x), _ELIO_RPC_EXPAND(_ELIO_RPC_FOR_EACH_4(Class, macro, __VA_ARGS__))
#define _ELIO_RPC_FOR_EACH_6(Class, macro, x, ...) macro(Class, x), _ELIO_RPC_EXPAND(_ELIO_RPC_FOR_EACH_5(Class, macro, __VA_ARGS__))
#define _ELIO_RPC_FOR_EACH_7(Class, macro, x, ...) macro(Class, x), _ELIO_RPC_EXPAND(_ELIO_RPC_FOR_EACH_6(Class, macro, __VA_ARGS__))
#define _ELIO_RPC_FOR_EACH_8(Class, macro, x, ...) macro(Class, x), _ELIO_RPC_EXPAND(_ELIO_RPC_FOR_EACH_7(Class, macro, __VA_ARGS__))
#define _ELIO_RPC_FOR_EACH_9(Class, macro, x, ...) macro(Class, x), _ELIO_RPC_EXPAND(_ELIO_RPC_FOR_EACH_8(Class, macro, __VA_ARGS__))
#define _ELIO_RPC_FOR_EACH_10(Class, macro, x, ...) macro(Class, x), _ELIO_RPC_EXPAND(_ELIO_RPC_FOR_EACH_9(Class, macro, __VA_ARGS__))
#define _ELIO_RPC_FOR_EACH_11(Class, macro, x, ...) macro(Class, x), _ELIO_RPC_EXPAND(_ELIO_RPC_FOR_EACH_10(Class, macro, __VA_ARGS__))
#define _ELIO_RPC_FOR_EACH_12(Class, macro, x, ...) macro(Class, x), _ELIO_RPC_EXPAND(_ELIO_RPC_FOR_EACH_11(Class, macro, __VA_ARGS__))
#define _ELIO_RPC_FOR_EACH_13(Class, macro, x, ...) macro(Class, x), _ELIO_RPC_EXPAND(_ELIO_RPC_FOR_EACH_12(Class, macro, __VA_ARGS__))
#define _ELIO_RPC_FOR_EACH_14(Class, macro, x, ...) macro(Class, x), _ELIO_RPC_EXPAND(_ELIO_RPC_FOR_EACH_13(Class, macro, __VA_ARGS__))
#define _ELIO_RPC_FOR_EACH_15(Class, macro, x, ...) macro(Class, x), _ELIO_RPC_EXPAND(_ELIO_RPC_FOR_EACH_14(Class, macro, __VA_ARGS__))
#define _ELIO_RPC_FOR_EACH_16(Class, macro, x, ...) macro(Class, x), _ELIO_RPC_EXPAND(_ELIO_RPC_FOR_EACH_15(Class, macro, __VA_ARGS__))

#define _ELIO_RPC_GET_MACRO(_1,_2,_3,_4,_5,_6,_7,_8,_9,_10,_11,_12,_13,_14,_15,_16,NAME,...) NAME
#define _ELIO_RPC_FOR_EACH(Class, macro, ...) \
    _ELIO_RPC_EXPAND(_ELIO_RPC_GET_MACRO(__VA_ARGS__, \
        _ELIO_RPC_FOR_EACH_16, _ELIO_RPC_FOR_EACH_15, _ELIO_RPC_FOR_EACH_14, _ELIO_RPC_FOR_EACH_13, \
        _ELIO_RPC_FOR_EACH_12, _ELIO_RPC_FOR_EACH_11, _ELIO_RPC_FOR_EACH_10, _ELIO_RPC_FOR_EACH_9, \
        _ELIO_RPC_FOR_EACH_8, _ELIO_RPC_FOR_EACH_7, _ELIO_RPC_FOR_EACH_6, _ELIO_RPC_FOR_EACH_5, \
        _ELIO_RPC_FOR_EACH_4, _ELIO_RPC_FOR_EACH_3, _ELIO_RPC_FOR_EACH_2, _ELIO_RPC_FOR_EACH_1) \
    (Class, macro, __VA_ARGS__))

/// Define serializable fields for an RPC struct
/// @param ClassName The name of the enclosing struct/class
/// @param ... The field names to serialize
/// @example
/// struct Person {
///     std::string name;
///     int32_t age;
///     ELIO_RPC_FIELDS(Person, name, age)
/// };
#define ELIO_RPC_FIELDS(ClassName, ...) \
    using _elio_rpc_fields_tag = ::elio::rpc::rpc_fields_marker; \
    using _elio_rpc_self_type = ClassName; \
    static constexpr auto _elio_rpc_get_fields() { \
        return std::make_tuple(_ELIO_RPC_FOR_EACH(ClassName, _ELIO_RPC_FIELD_IMPL, __VA_ARGS__)); \
    }

/// Define an empty RPC struct with no fields
/// @param ClassName The name of the enclosing struct/class
/// @example
/// struct EmptyRequest {
///     ELIO_RPC_EMPTY_FIELDS(EmptyRequest)
/// };
#define ELIO_RPC_EMPTY_FIELDS(ClassName) \
    using _elio_rpc_fields_tag = ::elio::rpc::rpc_fields_marker; \
    using _elio_rpc_self_type = ClassName; \
    static constexpr auto _elio_rpc_get_fields() { \
        return std::make_tuple(); \
    }

// ============================================================================
// RPC method definition helpers
// ============================================================================

/// Method ID type
using method_id_t = uint32_t;

/// RPC method descriptor
template<method_id_t MethodId, typename Request, typename Response>
struct method_descriptor {
    static constexpr method_id_t id = MethodId;
    using request_type = Request;
    using response_type = Response;
};

/// Define an RPC method
#define ELIO_RPC_METHOD(method_id, request_type, response_type) \
    ::elio::rpc::method_descriptor<method_id, request_type, response_type>

/// Empty request/response types for methods that don't need them
struct empty_request {
    ELIO_RPC_EMPTY_FIELDS(empty_request)
};

struct empty_response {
    ELIO_RPC_EMPTY_FIELDS(empty_response)
};

// ============================================================================
// RPC result type
// ============================================================================

/// Error codes for RPC operations
enum class rpc_error : uint32_t {
    success = 0,
    timeout = 1,
    connection_closed = 2,
    invalid_message = 3,
    method_not_found = 4,
    serialization_error = 5,
    internal_error = 6,
    cancelled = 7,
};

/// Convert error code to string
inline const char* rpc_error_str(rpc_error err) {
    switch (err) {
        case rpc_error::success: return "success";
        case rpc_error::timeout: return "timeout";
        case rpc_error::connection_closed: return "connection closed";
        case rpc_error::invalid_message: return "invalid message";
        case rpc_error::method_not_found: return "method not found";
        case rpc_error::serialization_error: return "serialization error";
        case rpc_error::internal_error: return "internal error";
        case rpc_error::cancelled: return "cancelled";
        default: return "unknown error";
    }
}

/// RPC call result
template<typename T>
class rpc_result {
public:
    rpc_result() = default;
    
    /// Construct success result
    explicit rpc_result(T value)
        : value_(std::move(value))
        , error_(rpc_error::success) {}
    
    /// Construct error result
    explicit rpc_result(rpc_error err)
        : error_(err) {}
    
    /// Check if successful
    bool ok() const noexcept { return error_ == rpc_error::success; }
    explicit operator bool() const noexcept { return ok(); }
    
    /// Get error code
    rpc_error error() const noexcept { return error_; }
    
    /// Get error message
    const char* error_message() const noexcept { return rpc_error_str(error_); }
    
    /// Get value (throws if error)
    T& value() & {
        if (!ok()) throw std::runtime_error(error_message());
        return value_;
    }
    const T& value() const& {
        if (!ok()) throw std::runtime_error(error_message());
        return value_;
    }
    T&& value() && {
        if (!ok()) throw std::runtime_error(error_message());
        return std::move(value_);
    }
    
    /// Get value or default
    template<typename U>
    T value_or(U&& default_value) const& {
        return ok() ? value_ : static_cast<T>(std::forward<U>(default_value));
    }
    template<typename U>
    T value_or(U&& default_value) && {
        return ok() ? std::move(value_) : static_cast<T>(std::forward<U>(default_value));
    }
    
    /// Access value (undefined if error)
    T* operator->() { return &value_; }
    const T* operator->() const { return &value_; }
    T& operator*() & { return value_; }
    const T& operator*() const& { return value_; }
    T&& operator*() && { return std::move(value_); }
    
private:
    T value_{};
    rpc_error error_ = rpc_error::success;
};

/// Specialization for void
template<>
class rpc_result<void> {
public:
    rpc_result() = default;
    explicit rpc_result(rpc_error err) : error_(err) {}
    
    static rpc_result success() { return rpc_result(); }
    
    bool ok() const noexcept { return error_ == rpc_error::success; }
    explicit operator bool() const noexcept { return ok(); }
    
    rpc_error error() const noexcept { return error_; }
    const char* error_message() const noexcept { return rpc_error_str(error_); }
    
private:
    rpc_error error_ = rpc_error::success;
};

} // namespace elio::rpc
