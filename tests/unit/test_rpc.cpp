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
