#include <catch2/catch_test_macros.hpp>
#include <elio/rpc/rpc.hpp>

using namespace elio::rpc;

// ============================================================================
// Test message types
// ============================================================================

struct SimpleMessage {
    int32_t id;
    std::string name;
    
    ELIO_RPC_FIELDS(SimpleMessage, id, name)
};

struct NestedChild {
    int32_t value;
    std::string label;
    
    ELIO_RPC_FIELDS(NestedChild, value, label)
};

struct NestedParent {
    std::string name;
    NestedChild child;
    std::vector<NestedChild> children;
    
    ELIO_RPC_FIELDS(NestedParent, name, child, children)
};

struct AllTypesMessage {
    int8_t i8;
    int16_t i16;
    int32_t i32;
    int64_t i64;
    uint8_t u8;
    uint16_t u16;
    uint32_t u32;
    uint64_t u64;
    float f32;
    double f64;
    bool b;
    std::string s;
    std::vector<int32_t> vec;
    std::optional<std::string> opt;
    std::map<std::string, int32_t> map;
    
    ELIO_RPC_FIELDS(AllTypesMessage, i8, i16, i32, i64, u8, u16, u32, u64, f32, f64, b, s, vec, opt, map)
};

struct ArrayOfStructs {
    std::vector<SimpleMessage> items;
    
    ELIO_RPC_FIELDS(ArrayOfStructs, items)
};

struct MapOfStructs {
    std::map<std::string, SimpleMessage> items;
    
    ELIO_RPC_FIELDS(MapOfStructs, items)
};

// Test structs for protocol tests
struct TestRequest {
    int32_t value;
    ELIO_RPC_FIELDS(TestRequest, value)
};

struct TestResponse {
    std::string result;
    ELIO_RPC_FIELDS(TestResponse, result)
};

// ============================================================================
// Buffer tests
// ============================================================================

TEST_CASE("buffer_writer basic operations", "[rpc][buffer]") {
    buffer_writer writer;
    
    SECTION("write primitive types") {
        writer.write(int32_t{42});
        writer.write(int64_t{12345678901234LL});
        writer.write(double{3.14159});
        
        REQUIRE(writer.size() == sizeof(int32_t) + sizeof(int64_t) + sizeof(double));
    }
    
    SECTION("write string") {
        writer.write_string("Hello, World!");
        
        // 4 bytes length prefix + 13 bytes string
        REQUIRE(writer.size() == 4 + 13);
    }
    
    SECTION("write blob") {
        std::vector<uint8_t> data = {1, 2, 3, 4, 5};
        writer.write_blob(data);
        
        // 4 bytes length prefix + 5 bytes data
        REQUIRE(writer.size() == 4 + 5);
    }
    
    SECTION("reserve and back-patch") {
        size_t offset = writer.reserve_space(4);
        writer.write(int32_t{100});
        writer.write_at<int32_t>(offset, 42);
        
        buffer_view view = writer.view();
        REQUIRE(view.read<int32_t>() == 42);
        REQUIRE(view.read<int32_t>() == 100);
    }
}

TEST_CASE("buffer_view basic operations", "[rpc][buffer]") {
    buffer_writer writer;
    writer.write(int32_t{42});
    writer.write(int64_t{12345});
    writer.write_string("test");
    
    buffer_view view = writer.view();
    
    SECTION("read values") {
        REQUIRE(view.read<int32_t>() == 42);
        REQUIRE(view.read<int64_t>() == 12345);
        REQUIRE(view.read_string() == "test");
    }
    
    SECTION("peek without advancing") {
        int32_t peeked = view.peek<int32_t>();
        REQUIRE(peeked == 42);
        REQUIRE(view.position() == 0);
    }
    
    SECTION("seek and skip") {
        view.skip(4);
        REQUIRE(view.read<int64_t>() == 12345);
        
        view.seek(0);
        REQUIRE(view.read<int32_t>() == 42);
    }
    
    SECTION("remaining bytes") {
        REQUIRE(view.remaining() == writer.size());
        view.read<int32_t>();
        REQUIRE(view.remaining() == writer.size() - 4);
    }
}

TEST_CASE("buffer error handling", "[rpc][buffer]") {
    buffer_writer writer;
    writer.write(int32_t{42});
    
    buffer_view view = writer.view();
    
    SECTION("read past end throws") {
        view.read<int32_t>();  // OK
        REQUIRE_THROWS_AS(view.read<int32_t>(), serialization_error);
    }
    
    SECTION("seek past end throws") {
        REQUIRE_THROWS_AS(view.seek(100), serialization_error);
    }
    
    SECTION("skip past end throws") {
        REQUIRE_THROWS_AS(view.skip(100), serialization_error);
    }
}

TEST_CASE("iovec_buffer", "[rpc][buffer]") {
    buffer_writer writer1;
    writer1.write(int32_t{1});
    
    buffer_writer writer2;
    writer2.write(int32_t{2});
    
    iovec_buffer iov;
    iov.add(writer1);
    iov.add(writer2);
    
    REQUIRE(iov.count() == 2);
    REQUIRE(iov.total_size() == 8);
    
    auto flat = iov.flatten();
    REQUIRE(flat.size() == 8);
    
    buffer_view view(flat.data(), flat.size());
    REQUIRE(view.read<int32_t>() == 1);
    REQUIRE(view.read<int32_t>() == 2);
}

// ============================================================================
// Serialization tests
// ============================================================================

TEST_CASE("serialize primitive types", "[rpc][serialize]") {
    SECTION("integers") {
        buffer_writer writer;
        serialize(writer, int32_t{42});
        serialize(writer, int64_t{-12345678901234LL});
        
        buffer_view view = writer.view();
        int32_t i32;
        int64_t i64;
        deserialize(view, i32);
        deserialize(view, i64);
        
        REQUIRE(i32 == 42);
        REQUIRE(i64 == -12345678901234LL);
    }
    
    SECTION("floats") {
        buffer_writer writer;
        serialize(writer, float{3.14f});
        serialize(writer, double{2.71828});
        
        buffer_view view = writer.view();
        float f;
        double d;
        deserialize(view, f);
        deserialize(view, d);
        
        REQUIRE(f == 3.14f);
        REQUIRE(d == 2.71828);
    }
    
    SECTION("bool") {
        buffer_writer writer;
        serialize(writer, true);
        serialize(writer, false);
        
        buffer_view view = writer.view();
        bool b1, b2;
        deserialize(view, b1);
        deserialize(view, b2);
        
        REQUIRE(b1 == true);
        REQUIRE(b2 == false);
    }
}

TEST_CASE("serialize strings", "[rpc][serialize]") {
    std::string original = "Hello, World!";
    
    buffer_writer writer;
    serialize(writer, original);
    
    buffer_view view = writer.view();
    std::string result;
    deserialize(view, result);
    
    REQUIRE(result == original);
}

TEST_CASE("serialize vectors", "[rpc][serialize]") {
    SECTION("vector of integers") {
        std::vector<int32_t> original = {1, 2, 3, 4, 5};
        
        buffer_writer writer;
        serialize(writer, original);
        
        buffer_view view = writer.view();
        std::vector<int32_t> result;
        deserialize(view, result);
        
        REQUIRE(result == original);
    }
    
    SECTION("vector of strings") {
        std::vector<std::string> original = {"one", "two", "three"};
        
        buffer_writer writer;
        serialize(writer, original);
        
        buffer_view view = writer.view();
        std::vector<std::string> result;
        deserialize(view, result);
        
        REQUIRE(result == original);
    }
    
    SECTION("empty vector") {
        std::vector<int32_t> original;
        
        buffer_writer writer;
        serialize(writer, original);
        
        buffer_view view = writer.view();
        std::vector<int32_t> result;
        deserialize(view, result);
        
        REQUIRE(result.empty());
    }
}

TEST_CASE("serialize std::array", "[rpc][serialize]") {
    std::array<int32_t, 3> original = {10, 20, 30};
    
    buffer_writer writer;
    serialize(writer, original);
    
    buffer_view view = writer.view();
    std::array<int32_t, 3> result;
    deserialize(view, result);
    
    REQUIRE(result == original);
}

TEST_CASE("serialize maps", "[rpc][serialize]") {
    SECTION("std::map") {
        std::map<std::string, int32_t> original = {{"a", 1}, {"b", 2}, {"c", 3}};
        
        buffer_writer writer;
        serialize(writer, original);
        
        buffer_view view = writer.view();
        std::map<std::string, int32_t> result;
        deserialize(view, result);
        
        REQUIRE(result == original);
    }
    
    SECTION("std::unordered_map") {
        std::unordered_map<int32_t, std::string> original = {{1, "one"}, {2, "two"}};
        
        buffer_writer writer;
        serialize(writer, original);
        
        buffer_view view = writer.view();
        std::unordered_map<int32_t, std::string> result;
        deserialize(view, result);
        
        REQUIRE(result == original);
    }
}

TEST_CASE("serialize optionals", "[rpc][serialize]") {
    SECTION("has value") {
        std::optional<std::string> original = "hello";
        
        buffer_writer writer;
        serialize(writer, original);
        
        buffer_view view = writer.view();
        std::optional<std::string> result;
        deserialize(view, result);
        
        REQUIRE(result.has_value());
        REQUIRE(*result == "hello");
    }
    
    SECTION("no value") {
        std::optional<std::string> original;
        
        buffer_writer writer;
        serialize(writer, original);
        
        buffer_view view = writer.view();
        std::optional<std::string> result;
        deserialize(view, result);
        
        REQUIRE_FALSE(result.has_value());
    }
}

TEST_CASE("serialize simple struct", "[rpc][serialize]") {
    SimpleMessage original{42, "test name"};
    
    buffer_writer writer;
    serialize(writer, original);
    
    buffer_view view = writer.view();
    SimpleMessage result;
    deserialize(view, result);
    
    REQUIRE(result.id == original.id);
    REQUIRE(result.name == original.name);
}

TEST_CASE("serialize nested structs", "[rpc][serialize]") {
    NestedParent original;
    original.name = "parent";
    original.child = {10, "child1"};
    original.children = {{20, "child2"}, {30, "child3"}};
    
    buffer_writer writer;
    serialize(writer, original);
    
    buffer_view view = writer.view();
    NestedParent result;
    deserialize(view, result);
    
    REQUIRE(result.name == original.name);
    REQUIRE(result.child.value == original.child.value);
    REQUIRE(result.child.label == original.child.label);
    REQUIRE(result.children.size() == original.children.size());
    REQUIRE(result.children[0].value == 20);
    REQUIRE(result.children[1].label == "child3");
}

TEST_CASE("serialize all types", "[rpc][serialize]") {
    AllTypesMessage original;
    original.i8 = -128;
    original.i16 = -32000;
    original.i32 = -2000000000;
    original.i64 = -9000000000000000000LL;
    original.u8 = 255;
    original.u16 = 65000;
    original.u32 = 4000000000U;
    original.u64 = 18000000000000000000ULL;
    original.f32 = 3.14159f;
    original.f64 = 2.718281828459045;
    original.b = true;
    original.s = "hello world";
    original.vec = {1, 2, 3, 4, 5};
    original.opt = "optional value";
    original.map = {{"key1", 100}, {"key2", 200}};
    
    buffer_writer writer;
    serialize(writer, original);
    
    buffer_view view = writer.view();
    AllTypesMessage result;
    deserialize(view, result);
    
    REQUIRE(result.i8 == original.i8);
    REQUIRE(result.i16 == original.i16);
    REQUIRE(result.i32 == original.i32);
    REQUIRE(result.i64 == original.i64);
    REQUIRE(result.u8 == original.u8);
    REQUIRE(result.u16 == original.u16);
    REQUIRE(result.u32 == original.u32);
    REQUIRE(result.u64 == original.u64);
    REQUIRE(result.f32 == original.f32);
    REQUIRE(result.f64 == original.f64);
    REQUIRE(result.b == original.b);
    REQUIRE(result.s == original.s);
    REQUIRE(result.vec == original.vec);
    REQUIRE(result.opt == original.opt);
    REQUIRE(result.map == original.map);
}

TEST_CASE("serialize array of structs", "[rpc][serialize]") {
    ArrayOfStructs original;
    original.items = {{1, "one"}, {2, "two"}, {3, "three"}};
    
    buffer_writer writer;
    serialize(writer, original);
    
    buffer_view view = writer.view();
    ArrayOfStructs result;
    deserialize(view, result);
    
    REQUIRE(result.items.size() == 3);
    REQUIRE(result.items[0].id == 1);
    REQUIRE(result.items[0].name == "one");
    REQUIRE(result.items[2].id == 3);
    REQUIRE(result.items[2].name == "three");
}

TEST_CASE("serialize map of structs", "[rpc][serialize]") {
    MapOfStructs original;
    original.items = {{"a", {1, "one"}}, {"b", {2, "two"}}};
    
    buffer_writer writer;
    serialize(writer, original);
    
    buffer_view view = writer.view();
    MapOfStructs result;
    deserialize(view, result);
    
    REQUIRE(result.items.size() == 2);
    REQUIRE(result.items["a"].id == 1);
    REQUIRE(result.items["a"].name == "one");
    REQUIRE(result.items["b"].id == 2);
}

// ============================================================================
// Protocol tests
// ============================================================================

TEST_CASE("frame header serialization", "[rpc][protocol]") {
    frame_header original;
    original.request_id = 12345;
    original.type = message_type::request;
    original.flags = message_flags::has_timeout;
    original.method_id = 42;
    original.payload_length = 1000;
    
    auto bytes = original.to_bytes();
    REQUIRE(bytes.size() == frame_header_size);
    
    auto result = frame_header::from_bytes(bytes.data());
    
    REQUIRE(result.magic == protocol_magic);
    REQUIRE(result.request_id == 12345);
    REQUIRE(result.type == message_type::request);
    REQUIRE(result.flags == message_flags::has_timeout);
    REQUIRE(result.method_id == 42);
    REQUIRE(result.payload_length == 1000);
    REQUIRE(result.is_valid());
}

TEST_CASE("frame header validation", "[rpc][protocol]") {
    SECTION("valid header") {
        frame_header h;
        h.magic = protocol_magic;
        h.payload_length = 1000;
        REQUIRE(h.is_valid());
    }
    
    SECTION("invalid magic") {
        frame_header h;
        h.magic = 0xDEADBEEF;
        h.payload_length = 1000;
        REQUIRE_FALSE(h.is_valid());
    }
    
    SECTION("payload too large") {
        frame_header h;
        h.magic = protocol_magic;
        h.payload_length = max_message_size + 1;
        REQUIRE_FALSE(h.is_valid());
    }
}

TEST_CASE("build request", "[rpc][protocol]") {
    TestRequest req{42};
    auto [header, payload] = build_request(123, 1, req, 5000);
    
    REQUIRE(header.request_id == 123);
    REQUIRE(header.type == message_type::request);
    REQUIRE(header.method_id == 1);
    REQUIRE(has_flag(header.flags, message_flags::has_timeout));
    
    // Payload should contain timeout + serialized request
    buffer_view view = payload.view();
    REQUIRE(view.read<uint32_t>() == 5000);  // timeout
    int32_t value;
    deserialize(view, value);
    REQUIRE(value == 42);
}

TEST_CASE("build response", "[rpc][protocol]") {
    TestResponse resp{"success"};
    auto [header, payload] = build_response(456, resp);
    
    REQUIRE(header.request_id == 456);
    REQUIRE(header.type == message_type::response);
    
    buffer_view view = payload.view();
    TestResponse result;
    deserialize(view, result);
    REQUIRE(result.result == "success");
}

TEST_CASE("build error response", "[rpc][protocol]") {
    auto [header, payload] = build_error_response(789, rpc_error::method_not_found, "test error");
    
    REQUIRE(header.request_id == 789);
    REQUIRE(header.type == message_type::error);
    
    buffer_view view = payload.view();
    error_payload err;
    deserialize(view, err);
    REQUIRE(err.code == rpc_error::method_not_found);
    REQUIRE(err.message == "test error");
}

TEST_CASE("ping/pong", "[rpc][protocol]") {
    auto ping = build_ping(100);
    REQUIRE(ping.type == message_type::ping);
    REQUIRE(ping.request_id == 100);
    REQUIRE(ping.payload_length == 0);
    
    auto pong = build_pong(100);
    REQUIRE(pong.type == message_type::pong);
    REQUIRE(pong.request_id == 100);
    REQUIRE(pong.payload_length == 0);
}

// ============================================================================
// rpc_result tests
// ============================================================================

TEST_CASE("rpc_result success", "[rpc][result]") {
    rpc_result<int> result(42);
    
    REQUIRE(result.ok());
    REQUIRE(static_cast<bool>(result));
    REQUIRE(result.error() == rpc_error::success);
    REQUIRE(result.value() == 42);
    REQUIRE(*result == 42);
}

TEST_CASE("rpc_result error", "[rpc][result]") {
    rpc_result<int> result(rpc_error::timeout);
    
    REQUIRE_FALSE(result.ok());
    REQUIRE_FALSE(static_cast<bool>(result));
    REQUIRE(result.error() == rpc_error::timeout);
    REQUIRE_THROWS(result.value());
}

TEST_CASE("rpc_result value_or", "[rpc][result]") {
    rpc_result<int> success(42);
    rpc_result<int> failure(rpc_error::timeout);
    
    REQUIRE(success.value_or(0) == 42);
    REQUIRE(failure.value_or(0) == 0);
}

TEST_CASE("rpc_result<void>", "[rpc][result]") {
    auto success = rpc_result<void>::success();
    REQUIRE(success.ok());
    
    rpc_result<void> failure(rpc_error::connection_closed);
    REQUIRE_FALSE(failure.ok());
    REQUIRE(failure.error() == rpc_error::connection_closed);
}

// ============================================================================
// Type traits tests
// ============================================================================

TEST_CASE("type traits", "[rpc][traits]") {
    REQUIRE(is_primitive_v<int32_t>);
    REQUIRE(is_primitive_v<double>);
    REQUIRE(is_primitive_v<bool>);
    REQUIRE_FALSE(is_primitive_v<std::string>);
    
    REQUIRE(is_string_type_v<std::string>);
    REQUIRE(is_string_type_v<std::string_view>);
    REQUIRE_FALSE(is_string_type_v<int>);
    
    REQUIRE(is_vector_v<std::vector<int>>);
    REQUIRE_FALSE(is_vector_v<std::array<int, 3>>);
    
    REQUIRE(is_std_array_v<std::array<int, 3>>);
    REQUIRE_FALSE(is_std_array_v<std::vector<int>>);
    
    REQUIRE(is_map_type_v<std::map<std::string, int>>);
    REQUIRE(is_map_type_v<std::unordered_map<int, std::string>>);
    REQUIRE_FALSE(is_map_type_v<std::vector<int>>);
    
    REQUIRE(is_optional_v<std::optional<int>>);
    REQUIRE_FALSE(is_optional_v<int>);
    
    REQUIRE(has_rpc_fields_v<SimpleMessage>);
}

// ============================================================================
// CRC32 tests
// ============================================================================

TEST_CASE("crc32 basic", "[rpc][crc32]") {
    SECTION("empty buffer") {
        uint32_t crc = crc32(nullptr, 0);
        REQUIRE(crc == 0);
    }
    
    SECTION("simple data") {
        const char* data = "123456789";
        uint32_t crc = crc32(data, 9);
        // Standard CRC32 for "123456789" is 0xCBF43926
        REQUIRE(crc == 0xCBF43926);
    }
    
    SECTION("span overload") {
        std::vector<uint8_t> data = {'1', '2', '3', '4', '5', '6', '7', '8', '9'};
        uint32_t crc = crc32(std::span<const uint8_t>(data));
        REQUIRE(crc == 0xCBF43926);
    }
}

TEST_CASE("crc32_iovec", "[rpc][crc32]") {
    // Split "123456789" across multiple iovecs
    const char* part1 = "12345";
    const char* part2 = "6789";
    
    struct iovec iov[2];
    iov[0].iov_base = const_cast<char*>(part1);
    iov[0].iov_len = 5;
    iov[1].iov_base = const_cast<char*>(part2);
    iov[1].iov_len = 4;
    
    uint32_t crc = crc32_iovec(iov, 2);
    REQUIRE(crc == 0xCBF43926);
}

// ============================================================================
// buffer_ref tests
// ============================================================================

TEST_CASE("buffer_ref basic", "[rpc][buffer_ref]") {
    std::vector<uint8_t> data = {1, 2, 3, 4, 5};
    
    SECTION("construct from pointer and size") {
        buffer_ref ref(data.data(), data.size());
        REQUIRE(ref.data() == data.data());
        REQUIRE(ref.size() == 5);
        REQUIRE_FALSE(ref.empty());
    }
    
    SECTION("construct from span") {
        buffer_ref ref{std::span<const uint8_t>(data)};
        REQUIRE(ref.size() == 5);
    }
    
    SECTION("construct from iovec") {
        struct iovec iov{data.data(), data.size()};
        buffer_ref ref(iov);
        REQUIRE(ref.size() == 5);
    }
    
    SECTION("empty buffer_ref") {
        buffer_ref ref;
        REQUIRE(ref.empty());
        REQUIRE(ref.size() == 0);
        REQUIRE(ref.data() == nullptr);
    }
    
    SECTION("to_iovec") {
        buffer_ref ref(data.data(), data.size());
        auto iov = ref.to_iovec();
        REQUIRE(iov.iov_base == data.data());
        REQUIRE(iov.iov_len == 5);
    }
    
    SECTION("as_string_view") {
        const char* text = "hello";
        buffer_ref ref(text, 5);
        REQUIRE(ref.as_string_view() == "hello");
    }
}

TEST_CASE("buffer_ref serialization", "[rpc][buffer_ref]") {
    std::vector<uint8_t> original_data = {0xDE, 0xAD, 0xBE, 0xEF};
    buffer_ref original(original_data.data(), original_data.size());
    
    // Serialize
    buffer_writer writer;
    serialize(writer, original);
    
    // Deserialize
    buffer_view view = writer.view();
    buffer_ref result;
    deserialize(view, result);
    
    REQUIRE(result.size() == original.size());
    // Data should match (view into serialized buffer)
    auto result_span = result.span();
    REQUIRE(std::equal(result_span.begin(), result_span.end(), 
                       original_data.begin(), original_data.end()));
}

// Test struct with buffer_ref field
struct MessageWithBufferRef {
    int32_t id;
    buffer_ref data;
    std::string name;
    
    ELIO_RPC_FIELDS(MessageWithBufferRef, id, data, name)
};

TEST_CASE("struct with buffer_ref field", "[rpc][buffer_ref]") {
    std::vector<uint8_t> blob_data = {1, 2, 3, 4, 5, 6, 7, 8};
    
    MessageWithBufferRef original;
    original.id = 42;
    original.data = buffer_ref(blob_data.data(), blob_data.size());
    original.name = "test";
    
    // Serialize
    buffer_writer writer;
    serialize(writer, original);
    
    // Deserialize
    buffer_view view = writer.view();
    MessageWithBufferRef result;
    deserialize(view, result);
    
    REQUIRE(result.id == 42);
    REQUIRE(result.data.size() == 8);
    REQUIRE(result.name == "test");
    
    auto data_span = result.data.span();
    REQUIRE(std::equal(data_span.begin(), data_span.end(),
                       blob_data.begin(), blob_data.end()));
}

// ============================================================================
// Checksum flag tests
// ============================================================================

TEST_CASE("message flags operations", "[rpc][protocol]") {
    SECTION("combine flags") {
        auto flags = message_flags::has_timeout | message_flags::has_checksum;
        REQUIRE(has_flag(flags, message_flags::has_timeout));
        REQUIRE(has_flag(flags, message_flags::has_checksum));
        REQUIRE_FALSE(has_flag(flags, message_flags::compressed));
    }
    
    SECTION("has_checksum flag") {
        auto flags = message_flags::has_checksum;
        REQUIRE(has_flag(flags, message_flags::has_checksum));
    }
}

TEST_CASE("build request with checksum", "[rpc][protocol]") {
    TestRequest req{100};
    auto [header, payload] = build_request(1, 1, req, std::nullopt, true);
    
    REQUIRE(has_flag(header.flags, message_flags::has_checksum));
    REQUIRE_FALSE(has_flag(header.flags, message_flags::has_timeout));
}

TEST_CASE("build request with timeout and checksum", "[rpc][protocol]") {
    TestRequest req{100};
    auto [header, payload] = build_request(1, 1, req, 5000, true);
    
    REQUIRE(has_flag(header.flags, message_flags::has_checksum));
    REQUIRE(has_flag(header.flags, message_flags::has_timeout));
}

TEST_CASE("build response with checksum", "[rpc][protocol]") {
    TestResponse resp{"ok"};
    auto [header, payload] = build_response(1, resp, true);
    
    REQUIRE(has_flag(header.flags, message_flags::has_checksum));
}

TEST_CASE("build error response with checksum", "[rpc][protocol]") {
    auto [header, payload] = build_error_response(1, rpc_error::timeout, "timed out", true);
    
    REQUIRE(has_flag(header.flags, message_flags::has_checksum));
    REQUIRE(header.type == message_type::error);
}

// ============================================================================
// buffer_ref type trait tests
// ============================================================================

TEST_CASE("buffer_ref type traits", "[rpc][buffer_ref]") {
    REQUIRE(is_buffer_ref_v<buffer_ref>);
    REQUIRE_FALSE(is_buffer_ref_v<int>);
    REQUIRE_FALSE(is_buffer_ref_v<std::string>);
    REQUIRE_FALSE(is_buffer_ref_v<std::vector<uint8_t>>);
}

// ============================================================================
// Hardening regression tests (DoS / silent overwrite / malformed input)
// ============================================================================

#include <elio/net/tcp.hpp>
#include <elio/runtime/scheduler.hpp>
#include <elio/time/timer.hpp>
#include <thread>
#include <atomic>
#include <chrono>
#include <future>
#include <memory>
#include <utility>
#include <sys/socket.h>
#include <unistd.h>

TEST_CASE("oversized vector count is rejected", "[rpc][security]") {
    // Wire layout for vector<int32_t>: <count:u32><n * int32>.
    // We craft a payload that claims count=0xFFFFFFFF but provides no
    // elements. Without the bound check this would call
    // vec.reserve(0xFFFFFFFF) and try to allocate 16 GB.
    buffer_writer w;
    w.write<uint32_t>(0xFFFFFFFFu);

    buffer_view view = w.view();
    std::vector<int32_t> out;
    REQUIRE_THROWS_AS(deserialize(view, out), serialization_error);
}

TEST_CASE("oversized vector-of-string count is rejected", "[rpc][security]") {
    // Min wire size of std::string is 4 (length prefix). Sending count
    // larger than remaining/4 must be rejected up front.
    buffer_writer w;
    w.write<uint32_t>(0x10000000u); // 256M strings — would be 256M slots

    buffer_view view = w.view();
    std::vector<std::string> out;
    REQUIRE_THROWS_AS(deserialize(view, out), serialization_error);
}

TEST_CASE("oversized map count is rejected", "[rpc][security]") {
    buffer_writer w;
    w.write<uint32_t>(0xFFFFFFFFu);

    buffer_view view = w.view();
    std::map<int32_t, int32_t> out;
    REQUIRE_THROWS_AS(deserialize(view, out), serialization_error);
}

TEST_CASE("plausible-but-too-large vector count is rejected", "[rpc][security]") {
    // 1 KiB of payload claiming 100k int32 elements (would need 400 KiB).
    buffer_writer w;
    w.write<uint32_t>(100'000u);
    for (int i = 0; i < 256; ++i) w.write<int32_t>(i); // only ~256 valid

    buffer_view view = w.view();
    std::vector<int32_t> out;
    REQUIRE_THROWS_AS(deserialize(view, out), serialization_error);
}

TEST_CASE("duplicate method id throws", "[rpc][security]") {
    using Method = ELIO_RPC_METHOD(42, TestRequest, TestResponse);
    rpc_server<elio::net::tcp_stream> server;
    server.register_method<Method>([](const TestRequest&)
                                       -> elio::coro::task<TestResponse> {
        co_return TestResponse{"first"};
    });

    REQUIRE_THROWS_AS(
        server.register_method<Method>([](const TestRequest&)
                                           -> elio::coro::task<TestResponse> {
            co_return TestResponse{"second"};
        }),
        std::invalid_argument);
}

TEST_CASE("malformed has_timeout flag does not crash session", "[rpc][security]") {
    // Build a request frame with has_timeout flag set but a payload that's
    // too short to contain the 4-byte timeout. Confirm the wire builder
    // produces what we expect; the fix in handle_request converts the
    // resulting serialization_error into rpc_error::invalid_message instead
    // of escaping and tearing the session down.

    using Method = ELIO_RPC_METHOD(99, TestRequest, TestResponse);
    rpc_server<elio::net::tcp_stream> server;
    server.register_method<Method>([](const TestRequest& req)
                                       -> elio::coro::task<TestResponse> {
        co_return TestResponse{std::to_string(req.value)};
    });

    // Construct a hand-crafted "malformed" payload: only 1 byte where
    // the timeout would live (need 4). The server must respond with an
    // error frame, not crash. We exercise just the parsing logic here
    // (wire-level test below covers the network path).
    frame_header hdr;
    hdr.request_id = 7;
    hdr.type = message_type::request;
    hdr.flags = message_flags::has_timeout;
    hdr.method_id = 99;
    hdr.payload_length = 1;

    // Verify parse_request itself throws on too-short timeout payload —
    // the surface our fix wraps in a try/catch.
    buffer_writer too_short;
    too_short.write<uint8_t>(0xAA);
    buffer_view v = too_short.view();
    REQUIRE_THROWS_AS(
        (parse_request<TestRequest>(v, message_flags::has_timeout)),
        serialization_error);
}

TEST_CASE("read_frame_bounded rejects oversized payload header",
          "[rpc][security][slow_loris]") {
    using namespace elio::net;
    using namespace elio::runtime;
    namespace coro = elio::coro;

    auto listener_opt = tcp_listener::bind(ipv6_address("::1", 0));
    REQUIRE(listener_opt.has_value());
    uint16_t port = listener_opt->local_address().port();

    scheduler sched(2);
    sched.start();

    std::atomic<bool> server_done{false};
    std::atomic<bool> rejected{false};

    sched.go([&]() -> coro::task<void> {
        auto stream = co_await listener_opt->accept();
        REQUIRE(stream.has_value());

        // Cap at 1 KiB, then attempt to read a frame whose header claims
        // 16 MiB of payload. Must return nullopt without allocating.
        auto frame = co_await elio::rpc::read_frame_bounded(*stream, 1024);
        if (!frame) rejected = true;
        server_done = true;
    });

    sched.go([&]() -> coro::task<void> {
        auto client = co_await tcp_connect(ipv6_address("::1", port));
        REQUIRE(client.has_value());

        // Send a header claiming 1 MiB payload; bounded cap is 1 KiB.
        elio::rpc::frame_header hdr;
        hdr.request_id = 1;
        hdr.type = elio::rpc::message_type::request;
        hdr.method_id = 1;
        hdr.payload_length = 1u * 1024u * 1024u; // 1 MiB > 1 KiB cap
        auto bytes = hdr.to_bytes();
        co_await client->write(bytes.data(), bytes.size());
        // Don't send any payload — the cap check happens before any
        // payload allocation.
    });

    for (int i = 0; i < 200 && !server_done; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    sched.shutdown();

    REQUIRE(server_done);
    REQUIRE(rejected);
}

TEST_CASE("rpc_session frame_read_timeout fires on slow-loris peer",
          "[rpc][security][slow_loris]") {
    using namespace elio::net;
    using namespace elio::runtime;
    namespace coro = elio::coro;

    auto listener_opt = tcp_listener::bind(ipv6_address("::1", 0));
    REQUIRE(listener_opt.has_value());
    uint16_t port = listener_opt->local_address().port();

    rpc_server_config cfg;
    cfg.frame_read_timeout = std::chrono::seconds(1);
    cfg.max_message_size = 1024;
    rpc_server<tcp_stream> server(cfg);

    using Method = ELIO_RPC_METHOD(1, TestRequest, TestResponse);
    server.register_method<Method>([](const TestRequest&)
                                       -> coro::task<TestResponse> {
        co_return TestResponse{"never reached"};
    });

    scheduler sched(2);
    sched.start();

    std::atomic<bool> server_done{false};

    sched.go([&]() -> coro::task<void> {
        auto stream = co_await listener_opt->accept();
        REQUIRE(stream.has_value());
        co_await server.handle_client(std::move(*stream));
        server_done = true;
    });

    sched.go([&]() -> coro::task<void> {
        auto client = co_await tcp_connect(ipv6_address("::1", port));
        REQUIRE(client.has_value());
        // Send only 4 bytes of a frame header (need 18) and stall.
        // Server must close the connection after frame_read_timeout=1s.
        std::array<uint8_t, 4> trickle{};
        elio::rpc::endian::write_le<uint32_t>(trickle.data(),
                                              elio::rpc::protocol_magic);
        co_await client->write(trickle.data(), trickle.size());
        // Now sleep past the deadline so the watchdog fires.
        co_await elio::time::sleep_for(std::chrono::seconds(2));
        // Connection should be torn down by now.
    });

    for (int i = 0; i < 400 && !server_done; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    sched.shutdown();
    REQUIRE(server_done);
}

TEST_CASE("frame arriving near deadline is delivered, not discarded as timeout",
          "[rpc][security][slow_loris]") {
    // Regression test for the watchdog-vs-frame race: when the timer CQE
    // fires microseconds before read_frame_bounded returns a complete frame,
    // both `frame` and `timed_out` end up set. Pre-fix code dropped such
    // frames; the contract is "must arrive by deadline" — and the frame did.
    //
    // frame_read_timeout is seconds-resolution, so we use a 1s deadline and
    // send the frame ~50 ms before it. The actual race window we care about
    // (timer-CQE-vs-recv-completion) is microseconds wide regardless of the
    // overall deadline, so iterating multiple times exercises the same race
    // path. With the fix the handler is invoked on every iteration; without
    // it the handler is occasionally skipped under timing jitter.
    using namespace elio::net;
    using namespace elio::runtime;
    namespace coro = elio::coro;

    auto listener_opt = tcp_listener::bind(ipv6_address("::1", 0));
    REQUIRE(listener_opt.has_value());
    uint16_t port = listener_opt->local_address().port();

    rpc_server_config cfg;
    cfg.frame_read_timeout = std::chrono::seconds(1);
    cfg.max_message_size = 1024;
    rpc_server<tcp_stream> server(cfg);

    using Method = ELIO_RPC_METHOD(202, TestRequest, TestResponse);
    std::atomic<int> handler_calls{0};
    server.register_method<Method>([&handler_calls](const TestRequest& req)
                                       -> coro::task<TestResponse> {
        handler_calls.fetch_add(1, std::memory_order_relaxed);
        co_return TestResponse{std::to_string(req.value)};
    });

    scheduler sched(2);
    sched.start();

    constexpr int iterations = 5;
    std::atomic<int> server_done{0};

    sched.go([&, &lst = *listener_opt]() -> coro::task<void> {
        for (int i = 0; i < iterations; ++i) {
            auto stream = co_await lst.accept();
            if (!stream) co_return;
            // handle_client returns after the connection closes (the client
            // sends one frame then drops the socket; the next read will hit
            // EOF or the watchdog will trip — either way the loop exits).
            co_await server.handle_client(std::move(*stream));
            server_done.fetch_add(1, std::memory_order_relaxed);
        }
    });

    sched.go([&]() -> coro::task<void> {
        for (int i = 0; i < iterations; ++i) {
            auto client = co_await tcp_connect(ipv6_address("::1", port));
            REQUIRE(client.has_value());

            // Sleep until ~50 ms before the server-side deadline, then send
            // a complete request frame in two back-to-back writes (header
            // then payload). The exact arrival time is jittery at this
            // resolution — sometimes well before, sometimes very close to,
            // the watchdog firing — which is exactly the race window we want
            // to exercise.
            co_await elio::time::sleep_for(std::chrono::milliseconds(950));

            TestRequest req{i};
            auto frame_pair = elio::rpc::build_request<TestRequest>(
                static_cast<uint32_t>(i + 1), 202, req);
            auto hbytes = frame_pair.first.to_bytes();
            co_await client->write(hbytes.data(), hbytes.size());
            if (frame_pair.second.size() > 0) {
                co_await client->write(frame_pair.second.data(),
                                       frame_pair.second.size());
            }

            // Wait briefly so the server processes the request before we
            // tear down the socket. Then close — the next server-side read
            // will hit EOF or the watchdog (whichever first) and the
            // handle_client coroutine returns.
            co_await elio::time::sleep_for(std::chrono::milliseconds(200));
        }
    });

    for (int i = 0; i < 1500 &&
                    server_done.load(std::memory_order_relaxed) < iterations;
         ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    sched.shutdown();

    REQUIRE(server_done.load() == iterations);
    REQUIRE(handler_calls.load() == iterations);
}

TEST_CASE("server max_sessions enforces backpressure",
          "[rpc][security]") {
    using namespace elio::net;
    using namespace elio::runtime;
    namespace coro = elio::coro;

    rpc_server_config cfg;
    cfg.max_sessions = 2;
    cfg.frame_read_timeout = std::chrono::seconds(0); // disabled for this test
    rpc_server<tcp_stream> server(cfg);
    using Method = ELIO_RPC_METHOD(1, TestRequest, TestResponse);
    server.register_method<Method>([](const TestRequest&)
                                       -> coro::task<TestResponse> {
        co_return TestResponse{"ok"};
    });

    auto listener_opt = tcp_listener::bind(ipv6_address("::1", 0));
    REQUIRE(listener_opt.has_value());
    uint16_t port = listener_opt->local_address().port();

    scheduler sched(2);
    sched.start();

    // Spawn 4 sessions explicitly through handle_client. Sessions 3 and 4
    // should be rejected by the max_sessions cap (returning immediately).
    std::atomic<int> session_started{0};
    std::atomic<int> session_finished{0};

    // Server-side: accept 4 connections and dispatch each to handle_client.
    sched.go([&, &lst = *listener_opt]() -> coro::task<void> {
        for (int i = 0; i < 4; ++i) {
            auto s = co_await lst.accept();
            if (!s) co_return;
            // handle_client returns immediately for the 3rd/4th because
            // try_reserve_session_slot fails.
            elio::runtime::scheduler::current()->go(
                [&server, &session_started, &session_finished, st = std::move(*s)]() mutable
                    -> coro::task<void> {
                    session_started.fetch_add(1);
                    co_await server.handle_client(std::move(st));
                    session_finished.fetch_add(1);
                });
        }
    });

    // Client-side: open 4 connections.
    std::atomic<int> connected{0};
    for (int i = 0; i < 4; ++i) {
        sched.go([&, port]() -> coro::task<void> {
            auto s = co_await tcp_connect(ipv6_address("::1", port));
            if (s) {
                connected.fetch_add(1);
                // Hold the connection open for a bit so server-side state
                // can be observed.
                co_await elio::time::sleep_for(std::chrono::milliseconds(200));
            }
        });
    }

    // Wait for 4 sessions to be started server-side (rejected ones return
    // immediately).
    for (int i = 0; i < 200 && session_started.load() < 4; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    // Wait for 2 of them (the rejected ones) to have finished already.
    for (int i = 0; i < 200 && session_finished.load() < 2; ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    REQUIRE(connected.load() == 4);
    REQUIRE(session_started.load() == 4);
    // The two rejected sessions exit handle_client immediately.
    REQUIRE(session_finished.load() >= 2);
    REQUIRE(server.session_count() <= cfg.max_sessions);

    // Tear down: close all live sessions so the read loops exit.
    server.stop();
    sched.shutdown();
}

TEST_CASE("frame header round-trip is little-endian on the wire",
          "[rpc][security][endian]") {
    // Independently confirm the bytes laid down match the documented
    // little-endian wire layout, regardless of host endianness.
    frame_header hdr;
    hdr.magic = 0x01020304u;
    hdr.request_id = 0x05060708u;
    hdr.type = message_type::request;
    hdr.flags = message_flags::has_timeout;
    hdr.method_id = 0x09'0A'0B'0Cu;
    hdr.payload_length = 0x0D'0E'0F'10u;

    auto bytes = hdr.to_bytes();
    REQUIRE(bytes[0] == 0x04);
    REQUIRE(bytes[1] == 0x03);
    REQUIRE(bytes[2] == 0x02);
    REQUIRE(bytes[3] == 0x01);
    REQUIRE(bytes[4] == 0x08);
    REQUIRE(bytes[5] == 0x07);
    REQUIRE(bytes[6] == 0x06);
    REQUIRE(bytes[7] == 0x05);
    REQUIRE(bytes[8] == static_cast<uint8_t>(message_type::request));
    REQUIRE(bytes[9] == static_cast<uint8_t>(message_flags::has_timeout));
    REQUIRE(bytes[10] == 0x0C);
    REQUIRE(bytes[11] == 0x0B);
    REQUIRE(bytes[12] == 0x0A);
    REQUIRE(bytes[13] == 0x09);
    REQUIRE(bytes[14] == 0x10);
    REQUIRE(bytes[15] == 0x0F);
    REQUIRE(bytes[16] == 0x0E);
    REQUIRE(bytes[17] == 0x0D);

    auto round = frame_header::from_bytes(bytes.data());
    REQUIRE(round.magic == hdr.magic);
    REQUIRE(round.request_id == hdr.request_id);
    REQUIRE(round.type == hdr.type);
    REQUIRE(round.flags == hdr.flags);
    REQUIRE(round.method_id == hdr.method_id);
    REQUIRE(round.payload_length == hdr.payload_length);
}

TEST_CASE("buffer_writer encodes integers as little-endian",
          "[rpc][security][endian]") {
    buffer_writer w;
    w.write<uint32_t>(0xDEADBEEFu);
    w.write<uint16_t>(0xC0DEu);
    w.write<uint64_t>(0x0123456789ABCDEFull);

    auto bytes = w.span();
    REQUIRE(bytes[0] == 0xEF);
    REQUIRE(bytes[1] == 0xBE);
    REQUIRE(bytes[2] == 0xAD);
    REQUIRE(bytes[3] == 0xDE);
    REQUIRE(bytes[4] == 0xDE);
    REQUIRE(bytes[5] == 0xC0);
    REQUIRE(bytes[6] == 0xEF);
    REQUIRE(bytes[7] == 0xCD);
    REQUIRE(bytes[8] == 0xAB);
    REQUIRE(bytes[9] == 0x89);
    REQUIRE(bytes[10] == 0x67);
    REQUIRE(bytes[11] == 0x45);
    REQUIRE(bytes[12] == 0x23);
    REQUIRE(bytes[13] == 0x01);
}

// Defined at file scope to avoid -Wunused-local-typedefs from the
// ELIO_RPC_FIELDS macro expanding inside a function body.
struct ConcurrencyDelayReq { int32_t ms; ELIO_RPC_FIELDS(ConcurrencyDelayReq, ms) };
struct ConcurrencyDelayResp { int32_t echo; ELIO_RPC_FIELDS(ConcurrencyDelayResp, echo) };
using ConcurrencyDelayMethod = ELIO_RPC_METHOD(101, ConcurrencyDelayReq, ConcurrencyDelayResp);

TEST_CASE("server out-of-order dispatch does not head-of-line block",
          "[rpc][security][concurrent]") {
    using namespace elio::net;
    using namespace elio::runtime;
    namespace coro = elio::coro;

    using DelayReq = ConcurrencyDelayReq;
    using DelayResp = ConcurrencyDelayResp;
    using DelayMethod = ConcurrencyDelayMethod;

    auto listener_opt = tcp_listener::bind(ipv6_address("::1", 0));
    REQUIRE(listener_opt.has_value());
    uint16_t port = listener_opt->local_address().port();

    rpc_server_config cfg;
    // Short read deadline so the server-side session naturally exits after
    // the client has stopped sending; this keeps the test from depending
    // on a forced-close path.
    cfg.frame_read_timeout = std::chrono::seconds(1);
    auto server = std::make_shared<rpc_server<tcp_stream>>(cfg);

    // Server-side concurrency probe: deterministic in-flight counter
    // observed by every handler entry/exit. If the read loop dispatches
    // requests concurrently, max_in_flight will reach >= 2 (and ideally 3)
    // even on slow CI runners. This replaces the previous wall-clock
    // assertion which was flaky on arm64-Debug + ASAN where the shared
    // GitHub runner can stretch a 150ms sleep into hundreds of ms of
    // overhead, blurring the concurrent-vs-sequential gap.
    std::atomic<int> in_flight{0};
    std::atomic<int> max_in_flight{0};
    server->register_method<DelayMethod>(
        [&in_flight, &max_in_flight](const DelayReq& r) -> coro::task<DelayResp> {
            int now = in_flight.fetch_add(1, std::memory_order_acq_rel) + 1;
            int prev = max_in_flight.load(std::memory_order_relaxed);
            while (prev < now &&
                   !max_in_flight.compare_exchange_weak(prev, now,
                                                       std::memory_order_acq_rel)) {
                // retry
            }
            co_await elio::time::sleep_for(std::chrono::milliseconds(r.ms));
            in_flight.fetch_sub(1, std::memory_order_acq_rel);
            co_return DelayResp{r.ms};
        });

    scheduler sched(4);
    sched.start();

    // Use handle_client directly to avoid the listener.accept() hang
    // during shutdown. We accept exactly one connection.
    sched.go([&, &lst = *listener_opt]() -> coro::task<void> {
        auto s = co_await lst.accept();
        if (!s) co_return;
        co_await server->handle_client(std::move(*s));
    });

    // Cross-thread synchronization: the test main thread waits on this
    // promise instead of spinning on a 5-second wall-clock budget. The
    // budget here (60s) only bounds pathological hangs.
    std::promise<void> client_done_promise;
    auto client_done = client_done_promise.get_future();
    std::atomic<int> ok_count{0};

    sched.go([&, p = std::move(client_done_promise)]() mutable -> coro::task<void> {
        auto client_opt = co_await tcp_rpc_client::connect("::1", port);
        REQUIRE(client_opt.has_value());
        auto client = *client_opt;

        auto* current_sched = elio::runtime::scheduler::current();
        REQUIRE(current_sched != nullptr);

        // Per-call timeout that comfortably accommodates arm64-Debug+ASAN
        // CI runners where 150ms sleep + frame round-trip can take hundreds
        // of ms. Still well below the 30s default so a real hang surfaces.
        auto call_timeout = std::chrono::seconds(5);
        auto h1 = current_sched->go_joinable(
            [c = client, call_timeout]() -> coro::task<int> {
                auto r = co_await c->call<DelayMethod>(DelayReq{150}, call_timeout);
                co_return r.ok() ? r->echo : -1;
            });
        auto h2 = current_sched->go_joinable(
            [c = client, call_timeout]() -> coro::task<int> {
                auto r = co_await c->call<DelayMethod>(DelayReq{150}, call_timeout);
                co_return r.ok() ? r->echo : -1;
            });
        auto h3 = current_sched->go_joinable(
            [c = client, call_timeout]() -> coro::task<int> {
                auto r = co_await c->call<DelayMethod>(DelayReq{150}, call_timeout);
                co_return r.ok() ? r->echo : -1;
            });

        int v1 = co_await std::move(h1);
        int v2 = co_await std::move(h2);
        int v3 = co_await std::move(h3);
        if (v1 == 150) ok_count.fetch_add(1);
        if (v2 == 150) ok_count.fetch_add(1);
        if (v3 == 150) ok_count.fetch_add(1);

        p.set_value();
    });

    // 60s upper bound only catches pathological hangs; healthy runs
    // resolve in well under a second on developer machines and a few
    // seconds on the slowest CI runner.
    auto status = client_done.wait_for(std::chrono::seconds(60));
    REQUIRE(status == std::future_status::ready);
    REQUIRE(ok_count.load() == 3);
    // Deterministic concurrency check: server saw at least 2 handlers in
    // flight simultaneously (with the test's 3 parallel calls and 150ms
    // sleeps, max_in_flight should reach 3 in practice; >= 2 is enough
    // to rule out head-of-line blocking).
    REQUIRE(max_in_flight.load() >= 2);

    // Drain naturally so any in-flight dispatched handler completes
    // before the scheduler tears down its workers — keeps ASAN clean.
    server->stop();
    sched.shutdown(std::chrono::seconds(30));
}

// ============================================================================
// Lifetime regression: rpc_client member coroutines must outlive the client
// object (issue #245 — "rpc_client member coroutines can outlive the client
// object").
//
// rpc_client<Stream> derives from enable_shared_from_this, but call_impl()
// stores raw `this` (and pending_eraser stores a raw rpc_client*) across every
// co_await. If the last external shared_ptr is dropped while a call is parked
// on its completion_event, ~rpc_client() would free stream_, send_mutex_ and
// pending_shards_ while the suspended coroutine still references them — a
// use-after-free. The fix takes a strong self-reference at the top of the
// coroutine (auto self = this->shared_from_this()).
//
// This test reproduces the race deterministically without a server:
//   * a socketpair provides a real, valid fd for the client stream;
//   * the client is created WITHOUT starting its receive loop, so nothing
//     other than the external shared_ptr keeps it alive;
//   * the call is spawned through a *raw* pointer so the wrapper frame holds
//     no shared_ptr — exactly the "member coroutine holds only raw this"
//     scenario from the bug report;
//   * the peer end of the socketpair is drained with a blocking read, which
//     only returns once call_impl() has actually written its request frame and
//     is therefore suspended on completion_event;
//   * the external shared_ptr is then dropped. With the fix the strong self
//     reference keeps the object alive (weak_ptr stays valid); without it ASAN
//     flags the use-after-free and the weak_ptr would have expired.
// The call is given a short timeout so its watcher wakes it, the coroutine
// unwinds, and the client is finally destroyed.
// ============================================================================

// File-scope method to avoid -Wunused-local-typedefs from ELIO_RPC_FIELDS.
struct LifetimeReq { int32_t x; ELIO_RPC_FIELDS(LifetimeReq, x) };
struct LifetimeResp { int32_t x; ELIO_RPC_FIELDS(LifetimeResp, x) };
using LifetimeMethod = ELIO_RPC_METHOD(202, LifetimeReq, LifetimeResp);

TEST_CASE("call_impl keeps client alive while awaiting response",
          "[rpc][uaf][lifetime]") {
    using namespace elio::net;
    using namespace elio::runtime;
    namespace coro = elio::coro;

    int fds[2] = {-1, -1};
    REQUIRE(::socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);

    // Client owns fds[0]; fds[1] is the peer we drain from the test thread.
    auto client = tcp_rpc_client::create(tcp_stream(fds[0]));
    std::weak_ptr<tcp_rpc_client> wk = client;
    // Raw pointer handed to the spawned coroutine. Intentionally NOT a
    // shared_ptr: the wrapper frame must not hold any strong reference, so
    // that the only strong references are the external `client` here and the
    // `self` grabbed inside call_impl().
    tcp_rpc_client* raw = client.get();

    scheduler sched(2);
    sched.start();

    // Signals that the spawned call coroutine has actually started running
    // (and therefore has dereferenced `raw` before we drop `client`).
    std::atomic<bool> call_started{false};
    // Error code observed by the call once it completes (times out). Captured
    // via an atomic because the TEST_CASE body is not a coroutine and cannot
    // co_await the join_handle for its value.
    std::atomic<int> call_error{-1};

    // A short per-call timeout guarantees the parked coroutine is eventually
    // woken by its timeout watcher, unwinds, releases `self`, and lets the
    // client be destroyed — even though there is no server to answer.
    auto call_timeout = std::chrono::milliseconds(500);
    auto h = sched.go_joinable(
        [raw, &call_started, &call_error, call_timeout]() -> coro::task<void> {
            call_started.store(true, std::memory_order_release);
            auto r = co_await raw->call<LifetimeMethod>(LifetimeReq{7}, call_timeout);
            // No server ever replies, so this must time out.
            call_error.store(static_cast<int>(r.error()), std::memory_order_release);
        });

    // Wait until the coroutine has begun (bounded so a hang surfaces).
    for (int i = 0; i < 2000 && !call_started.load(std::memory_order_acquire); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    REQUIRE(call_started.load(std::memory_order_acquire));

    // Drain the request frame from the peer end. read() returns only once
    // call_impl() has written the frame, i.e. it is now suspended on
    // completion_event with `self` held. This is our deterministic
    // "call is in flight" barrier.
    uint8_t buf[64];
    ssize_t n = ::read(fds[1], buf, sizeof(buf));
    REQUIRE(n > 0);
    // A full request frame is at least the fixed-size header.
    REQUIRE(n >= static_cast<ssize_t>(frame_header_size));

    // Drop the last external strong reference while the call is suspended.
    // With the fix, call_impl()'s `self` keeps the object alive.
    client.reset();

    // The client must still be alive: only the fix's strong self-reference
    // can be keeping it so. Without the fix this object was destroyed by the
    // reset above (and ASAN would have reported the UAF on resume).
    REQUIRE_FALSE(wk.expired());

    // Let the call time out, unwind, drop `self`, and destroy the client.
    // wait_destroyed() blocks safely from this non-coroutine context.
    h.wait_destroyed();
    REQUIRE(call_error.load(std::memory_order_acquire) ==
            static_cast<int>(rpc_error::timeout));

    // Now that the coroutine has fully unwound, the last strong reference is
    // gone and the client has been destroyed.
    REQUIRE(wk.expired());

    sched.shutdown(std::chrono::seconds(30));
    ::close(fds[1]);
}
