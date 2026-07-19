#include <catch2/catch_test_macros.hpp>
#include <elio/rpc/rpc.hpp>
#include <elio/runtime/scheduler.hpp>
#include <elio/time/timer.hpp>

#include "../test_main.cpp"

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstring>
#include <deque>
#include <functional>
#include <future>
#include <memory>
#include <mutex>
#include <sys/uio.h>
#include <thread>
#include <vector>

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

using ExistingStreamCreateMethod = ELIO_RPC_METHOD(306, TestRequest, TestResponse);
using OnewayNoResponseMethod = ELIO_RPC_METHOD(305, TestRequest, TestResponse);

namespace {

struct scripted_client_rpc_stream_state {
    std::mutex mutex;
    std::deque<uint8_t> inbound;
    bool valid = true;
};

void enqueue_client_frame(scripted_client_rpc_stream_state& state,
                          const frame_header& header,
                          const buffer_writer& payload) {
    auto header_bytes = header.to_bytes();
    std::lock_guard<std::mutex> lock(state.mutex);
    for (uint8_t byte : header_bytes) {
        state.inbound.push_back(byte);
    }
    for (size_t i = 0; i < payload.size(); ++i) {
        state.inbound.push_back(payload.data()[i]);
    }
}

void enqueue_client_frame(scripted_client_rpc_stream_state& state,
                          const frame_header& header) {
    buffer_writer empty;
    enqueue_client_frame(state, header, empty);
}

class scripted_client_rpc_stream {
public:
    scripted_client_rpc_stream()
        : state_(std::make_shared<scripted_client_rpc_stream_state>()) {}

    explicit scripted_client_rpc_stream(
        std::shared_ptr<scripted_client_rpc_stream_state> state)
        : state_(std::move(state)) {}

    bool is_valid() const noexcept {
        std::lock_guard<std::mutex> lock(state_->mutex);
        return state_->valid;
    }

    void shutdown_socket() noexcept {
        std::lock_guard<std::mutex> lock(state_->mutex);
        state_->valid = false;
    }

    elio::coro::task<elio::io::io_result> read_exactly(void* buffer, size_t length) {
        auto* out = static_cast<uint8_t*>(buffer);
        while (true) {
            {
                std::lock_guard<std::mutex> lock(state_->mutex);
                if (!state_->valid) {
                    co_return elio::io::io_result{-ECONNRESET, 0};
                }
                if (state_->inbound.size() >= length) {
                    for (size_t i = 0; i < length; ++i) {
                        out[i] = state_->inbound.front();
                        state_->inbound.pop_front();
                    }
                    co_return elio::io::io_result{static_cast<int32_t>(length), 0};
                }
            }
            co_await elio::time::sleep_for(std::chrono::milliseconds(1));
        }
    }

    elio::coro::task<elio::io::io_result> write_exactly(const void* buffer,
                                                        size_t length) {
        std::vector<uint8_t> bytes(length);
        std::memcpy(bytes.data(), buffer, length);
        handle_written_frame(bytes);
        co_return elio::io::io_result{static_cast<int32_t>(length), 0};
    }

    elio::coro::task<elio::io::io_result> writev(struct iovec* iovecs,
                                                 size_t iov_count) {
        size_t total = 0;
        for (size_t i = 0; i < iov_count; ++i) {
            total += iovecs[i].iov_len;
        }

        std::vector<uint8_t> bytes;
        bytes.reserve(total);
        for (size_t i = 0; i < iov_count; ++i) {
            const auto* data = static_cast<const uint8_t*>(iovecs[i].iov_base);
            bytes.insert(bytes.end(), data, data + iovecs[i].iov_len);
        }

        handle_written_frame(bytes);
        co_return elio::io::io_result{static_cast<int32_t>(total), 0};
    }

    elio::coro::task<elio::io::io_result> poll_write() {
        co_return elio::io::io_result{0, 0};
    }

private:
    void handle_written_frame(const std::vector<uint8_t>& bytes) {
        if (bytes.size() < frame_header_size) {
            return;
        }

        auto header = frame_header::from_bytes(bytes.data());
        if (header.type != message_type::request) {
            return;
        }

        enqueue_client_frame(*state_, build_pong(header.request_id));

        TestResponse response{"ok"};
        auto [response_header, response_payload] =
            build_response(header.request_id, response);
        enqueue_client_frame(*state_, response_header, response_payload);
    }

    std::shared_ptr<scripted_client_rpc_stream_state> state_;
};

} // namespace

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

TEST_CASE("RPC client ignores pong frames for normal calls", "[rpc][client]") {
    elio::runtime::scheduler sched(2);
    sched.start();

    std::atomic<bool> completed{false};
    std::atomic<bool> ok{false};

    auto state = std::make_shared<scripted_client_rpc_stream_state>();

    auto driver = [&]() -> elio::coro::task<void> {
        auto client = rpc_client<scripted_client_rpc_stream>::create(
            scripted_client_rpc_stream{state});

        auto result = co_await client->call<ExistingStreamCreateMethod>(
            TestRequest{42}, std::chrono::milliseconds(500));

        if (result.ok()) {
            ok.store(result.value().result == "ok", std::memory_order_release);
        }

        client->close();
        completed.store(true, std::memory_order_release);
    };

    sched.go(driver);

    auto deadline = std::chrono::steady_clock::now() + elio::test::scaled_ms(1000);
    while (!completed.load(std::memory_order_acquire) &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }

    REQUIRE(sched.shutdown(elio::test::scaled_ms(1000)));

    REQUIRE(completed.load(std::memory_order_acquire));
    REQUIRE(ok.load(std::memory_order_acquire));
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

TEST_CASE("empty buffer_ref serialization", "[rpc][buffer_ref]") {
    buffer_writer writer;
    buffer_ref original;

    serialize(writer, original);

    REQUIRE(writer.size() == sizeof(uint32_t));

    buffer_view view = writer.view();
    buffer_ref result;
    deserialize(view, result);

    REQUIRE(result.empty());
    REQUIRE(result.size() == 0);
    REQUIRE(result.data() != nullptr);
}

TEST_CASE("buffer_writer ignores empty raw byte writes",
          "[rpc][buffer][zero-length]") {
    buffer_writer writer;

    writer.write_bytes(nullptr, 0);

    REQUIRE(writer.size() == 0);
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

TEST_CASE("frame header rejects unsupported RPC wire contract bits",
          "[rpc][protocol][contract]") {
    SECTION("accepts supported request flags") {
        frame_header header;
        header.type = message_type::request;
        header.flags = message_flags::has_timeout |
                       message_flags::has_checksum |
                       message_flags::no_response;
        header.payload_length = sizeof(uint32_t);

        REQUIRE(header.is_valid());
    }

    SECTION("accepts checksum on non-request frames") {
        for (auto type : {message_type::response,
                          message_type::error,
                          message_type::ping,
                          message_type::pong,
                          message_type::cancel}) {
            frame_header header;
            header.type = type;
            header.flags = message_flags::has_checksum;

            REQUIRE(header.is_valid());
        }
    }

    SECTION("rejects compressed reserved flag") {
        frame_header header;
        header.type = message_type::request;
        header.flags = message_flags::compressed;

        REQUIRE_FALSE(header.is_valid());
    }

    SECTION("rejects streaming reserved flag") {
        frame_header header;
        header.type = message_type::response;
        header.flags = message_flags::streaming | message_flags::has_checksum;

        REQUIRE_FALSE(header.is_valid());
    }

    SECTION("rejects unknown flag bits") {
        frame_header header;
        header.type = message_type::request;
        header.flags = static_cast<message_flags>(0x80);

        REQUIRE_FALSE(header.is_valid());
    }

    SECTION("rejects request-only flags on non-request frames") {
        for (auto type : {message_type::response,
                          message_type::error,
                          message_type::ping,
                          message_type::pong,
                          message_type::cancel}) {
            frame_header timeout_header;
            timeout_header.type = type;
            timeout_header.flags = message_flags::has_timeout;

            REQUIRE_FALSE(timeout_header.is_valid());

            frame_header no_response_header;
            no_response_header.type = type;
            no_response_header.flags = message_flags::no_response;

            REQUIRE_FALSE(no_response_header.is_valid());
        }
    }

    SECTION("rejects timeout requests with payload too short for timeout") {
        frame_header header;
        header.type = message_type::request;
        header.flags = message_flags::has_timeout;
        header.payload_length = sizeof(uint32_t) - 1;

        REQUIRE_FALSE(header.is_valid());
    }

    SECTION("rejects method ids on non-request frames") {
        for (auto type : {message_type::response,
                          message_type::error,
                          message_type::ping,
                          message_type::pong,
                          message_type::cancel}) {
            frame_header header;
            header.type = type;
            header.method_id = 42;

            REQUIRE_FALSE(header.is_valid());
        }
    }

    SECTION("rejects payload on control frames") {
        for (auto type : {message_type::ping,
                          message_type::pong,
                          message_type::cancel}) {
            frame_header header;
            header.type = type;
            header.payload_length = 1;

            REQUIRE_FALSE(header.is_valid());
        }
    }

    SECTION("rejects unknown message types") {
        frame_header header;
        header.type = static_cast<message_type>(0x80);
        header.flags = message_flags::none;

        REQUIRE_FALSE(header.is_valid());
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

TEST_CASE("build one-way request marks no response", "[rpc][protocol]") {
    TestRequest req{100};
    auto [header, payload] = build_oneway_request(
        1, OnewayNoResponseMethod::id, req, true);

    REQUIRE(header.type == message_type::request);
    REQUIRE(header.method_id == OnewayNoResponseMethod::id);
    REQUIRE(has_flag(header.flags, message_flags::no_response));
    REQUIRE(has_flag(header.flags, message_flags::has_checksum));
    REQUIRE_FALSE(has_flag(header.flags, message_flags::has_timeout));

    buffer_view view = payload.view();
    auto [timeout, parsed] = parse_request<TestRequest>(view, header.flags);
    REQUIRE_FALSE(timeout.has_value());
    REQUIRE(parsed.value == 100);
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

TEST_CASE("typed RPC payload parsers reject trailing bytes",
          "[rpc][protocol][contract]") {
    SECTION("request payload") {
        TestRequest req{100};
        auto [header, payload] = build_request(1, 1, req);
        payload.write<uint8_t>(0xAA);

        buffer_view view = payload.view();
        REQUIRE_THROWS_AS(
            (parse_request<TestRequest>(view, header.flags)),
            serialization_error);
    }

    SECTION("request payload with timeout prefix") {
        TestRequest req{100};
        auto [header, payload] = build_request(1, 1, req, 5000);
        payload.write<uint8_t>(0xAA);

        buffer_view view = payload.view();
        REQUIRE_THROWS_AS(
            (parse_request<TestRequest>(view, header.flags)),
            serialization_error);
    }

    SECTION("response payload") {
        TestResponse resp{"ok"};
        auto frame = build_response(1, resp);
        frame.second.write<uint8_t>(0xAA);

        buffer_view view = frame.second.view();
        REQUIRE_THROWS_AS(
            (parse_response<TestResponse>(view)),
            serialization_error);
    }

    SECTION("error payload") {
        auto frame = build_error_response(1, rpc_error::timeout, "timed out");
        frame.second.write<uint8_t>(0xAA);

        buffer_view view = frame.second.view();
        REQUIRE_THROWS_AS(parse_error(view), serialization_error);
    }
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
#include <array>
#include <thread>
#include <atomic>
#include <chrono>
#include <future>
#include <memory>
#include <unordered_set>
#include <utility>
#include <cerrno>
#include <csignal>
#include <cstring>
#include <cstdlib>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>

namespace {

constexpr bool rpc_close_test_running_under_tsan() {
#if defined(__SANITIZE_THREAD__)
    return true;
#elif defined(__has_feature)
#if __has_feature(thread_sanitizer)
    return true;
#else
    return false;
#endif
#else
    return false;
#endif
}

bool read_exact_fd_for_rpc_close_test(int fd, uint8_t* data, size_t length) {
    size_t done = 0;
    while (done < length) {
        ssize_t n = ::read(fd, data + done, length - done);
        if (n < 0 && errno == EINTR) {
            continue;
        }
        if (n <= 0) {
            return false;
        }
        done += static_cast<size_t>(n);
    }
    return true;
}

[[noreturn]] void rpc_close_pending_alarm_handler(int) {
    ::_exit(124);
}

int run_rpc_close_off_scheduler_pending_child() {
    using namespace elio::net;
    using namespace elio::runtime;
    namespace coro = elio::coro;

    if (std::signal(SIGALRM, rpc_close_pending_alarm_handler) == SIG_ERR) {
        return 1;
    }
    ::alarm(20);

    int fds[2] = {-1, -1};
    if (::socketpair(AF_UNIX, SOCK_STREAM, 0, fds) != 0) {
        return 2;
    }

    auto client = tcp_rpc_client::create(tcp_stream(fds[0]));
    if (client->start()) {
        ::close(fds[1]);
        return 3;
    }

    scheduler sched(2);
    sched.start();

    std::promise<int> result_promise;
    auto result_future = result_promise.get_future();

    sched.go([client, p = std::move(result_promise)]() mutable
                 -> coro::task<void> {
        auto result = co_await client->call<ExistingStreamCreateMethod>(
            TestRequest{17}, elio::test::scaled_sec(30));
        p.set_value(static_cast<int>(result.error()));
    });

    std::array<uint8_t, frame_header_size> header_bytes{};
    if (!read_exact_fd_for_rpc_close_test(
            fds[1], header_bytes.data(), header_bytes.size())) {
        ::close(fds[1]);
        return 4;
    }

    auto header = frame_header::from_bytes(header_bytes.data());
    if (header.type != message_type::request ||
        header.method_id != ExistingStreamCreateMethod::id) {
        ::close(fds[1]);
        return 5;
    }

    std::vector<uint8_t> payload(header.payload_length);
    if (!payload.empty() &&
        !read_exact_fd_for_rpc_close_test(
            fds[1], payload.data(), payload.size())) {
        ::close(fds[1]);
        return 6;
    }

    std::thread close_thread([client] {
        client->close();
    });
    close_thread.join();

    if (result_future.wait_for(elio::test::scaled_sec(5)) !=
        std::future_status::ready) {
        ::close(fds[1]);
        return 7;
    }

    int error = result_future.get();
    if (error != static_cast<int>(rpc_error::connection_closed)) {
        ::close(fds[1]);
        return 8;
    }

    client.reset();
    if (!sched.shutdown(elio::test::scaled_sec(5))) {
        ::close(fds[1]);
        return 9;
    }

    ::close(fds[1]);
    ::alarm(0);
    return 0;
}

} // namespace

TEST_CASE("request id reservation skips occupied pending ids",
          "[rpc][contract][request_id]") {
    SECTION("derives a finite probe bound from occupied pending ids") {
        REQUIRE(detail::request_id_reservation_attempt_limit(0) == 1);
        REQUIRE(detail::request_id_reservation_attempt_limit(3) == 4);
        REQUIRE(detail::request_id_reservation_attempt_limit(
                    detail::request_id_space_size - 1) ==
                detail::request_id_space_size);
        REQUIRE(detail::request_id_reservation_attempt_limit(
                    detail::request_id_space_size) ==
                detail::request_id_space_size);
    }

    SECTION("skips occupied ids instead of overwriting them") {
        std::array<uint32_t, 3> generated{7, 8, 9};
        std::unordered_set<uint32_t> occupied{7, 8};
        size_t index = 0;

        auto reserved = detail::reserve_unique_request_id(
            [&]() {
                REQUIRE(index < generated.size());
                return generated[index++];
            },
            [&](uint32_t candidate) {
                return occupied.insert(candidate).second;
            },
            generated.size());

        REQUIRE(reserved.has_value());
        REQUIRE(*reserved == 9);
        REQUIRE(index == generated.size());
        REQUIRE(occupied.contains(7));
        REQUIRE(occupied.contains(8));
        REQUIRE(occupied.contains(9));
    }

    SECTION("reports exhaustion when no generated id can be reserved") {
        std::array<uint32_t, 3> generated{3, 4, 5};
        std::unordered_set<uint32_t> occupied{3, 4, 5};
        size_t index = 0;

        auto reserved = detail::reserve_unique_request_id(
            [&]() {
                REQUIRE(index < generated.size());
                return generated[index++];
            },
            [&](uint32_t candidate) {
                return occupied.insert(candidate).second;
            },
            generated.size());

        REQUIRE_FALSE(reserved.has_value());
        REQUIRE(index == generated.size());
        REQUIRE(occupied.size() == generated.size());
    }

    SECTION("supports one-way id selection without inserting pending state") {
        std::array<uint32_t, 3> generated{11, 12, 13};
        std::unordered_set<uint32_t> occupied{11, 12};
        size_t index = 0;

        auto selected = detail::reserve_unique_request_id(
            [&]() {
                REQUIRE(index < generated.size());
                return generated[index++];
            },
            [&](uint32_t candidate) {
                return !occupied.contains(candidate);
            },
            generated.size());

        REQUIRE(selected.has_value());
        REQUIRE(*selected == 13);
        REQUIRE(index == generated.size());
        REQUIRE(occupied.size() == 2);
        REQUIRE_FALSE(occupied.contains(13));
    }
}

namespace {

struct scripted_rpc_stream {
    std::vector<elio::io::io_result> writev_results;
    std::vector<elio::io::io_result> poll_write_results;
    size_t writev_calls = 0;
    size_t poll_write_calls = 0;
    std::string written;

    elio::coro::task<elio::io::io_result> read_exactly(void*, size_t) {
        co_return elio::io::io_result{-ENOSYS, 0};
    }

    elio::coro::task<elio::io::io_result> write_exactly(const void*, size_t) {
        co_return elio::io::io_result{-ENOSYS, 0};
    }

    elio::coro::task<elio::io::io_result> writev(struct iovec* iovecs, size_t iov_count) {
        const size_t call_index = writev_calls++;
        if (call_index >= writev_results.size()) {
            co_return elio::io::io_result{-EIO, 0};
        }
        auto result = writev_results[call_index];
        if (result.result > 0) {
            size_t remaining = static_cast<size_t>(result.result);
            for (size_t i = 0; i < iov_count && remaining > 0; ++i) {
                size_t n = std::min(remaining, iovecs[i].iov_len);
                written.append(static_cast<const char*>(iovecs[i].iov_base), n);
                remaining -= n;
            }
        }
        co_return result;
    }

    elio::coro::task<elio::io::io_result> poll_write() {
        ++poll_write_calls;
        if (poll_write_calls <= poll_write_results.size()) {
            co_return poll_write_results[poll_write_calls - 1];
        }
        co_return elio::io::io_result{0, 0};
    }

    bool is_valid() const noexcept { return true; }
};

elio::io::io_result run_scripted_writev(scripted_rpc_stream& stream,
                                        struct iovec* iovecs,
                                        size_t iov_count) {
    elio::runtime::scheduler sched(1);
    sched.start();

    std::atomic<bool> done{false};
    elio::io::io_result observed{-ETIMEDOUT, 0};

    auto handle = sched.go_joinable(
        [&]() -> elio::coro::task<void> {
            observed = co_await writev_exact(stream, iovecs, iov_count);
            done.store(true, std::memory_order_release);
        });
    (void)handle;

    for (int i = 0; i < 5000 && !done.load(std::memory_order_acquire); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    auto drained = sched.shutdown(elio::test::scaled_sec(5));

    REQUIRE(drained);
    REQUIRE(done.load(std::memory_order_acquire));
    return observed;
}

struct fast_response_stream_state {
    std::mutex mutex;
    elio::sync::event response_available;
    elio::sync::event response_dispatched;
    elio::sync::event shutdown_event;
    std::vector<uint8_t> inbound;
    size_t read_offset = 0;
    bool response_loaded = false;
    std::atomic<bool> shutdown{false};
    std::atomic<bool> request_seen{false};
    std::atomic<bool> next_header_read_started{false};
    std::atomic<bool> writev_returned_after_dispatch{false};
    bool fail_write_after_dispatch = false;
    int32_t request_value = 0;
};

struct fast_response_stream {
    std::shared_ptr<fast_response_stream_state> state;

    explicit fast_response_stream(std::shared_ptr<fast_response_stream_state> s)
        : state(std::move(s)) {}

    elio::coro::task<elio::io::io_result> read_exactly(void* data, size_t len) {
        while (true) {
            bool signal_dispatched = false;
            bool wait_for_shutdown = false;
            {
                std::lock_guard<std::mutex> lock(state->mutex);
                if (state->shutdown.load(std::memory_order_acquire)) {
                    co_return elio::io::io_result{-ECANCELED, 0};
                }

                if (state->read_offset + len <= state->inbound.size()) {
                    std::memcpy(data, state->inbound.data() + state->read_offset, len);
                    state->read_offset += len;
                    co_return elio::io::io_result{static_cast<int32_t>(len), 0};
                }

                if (state->response_loaded &&
                    state->read_offset == state->inbound.size() &&
                    len == frame_header_size) {
                    signal_dispatched =
                        !state->next_header_read_started.exchange(
                            true, std::memory_order_acq_rel);
                    wait_for_shutdown = true;
                }
            }

            if (signal_dispatched) {
                state->response_dispatched.set();
            }
            if (wait_for_shutdown) {
                co_await state->shutdown_event.wait();
                continue;
            }

            co_await state->response_available.wait();
        }
    }

    elio::coro::task<elio::io::io_result> write_exactly(const void*, size_t) {
        co_return elio::io::io_result{-ENOSYS, 0};
    }

    elio::coro::task<elio::io::io_result> writev(struct iovec* iovecs, size_t iov_count) {
        std::vector<uint8_t> written;
        size_t total = 0;
        for (size_t i = 0; i < iov_count; ++i) {
            auto* bytes = static_cast<const uint8_t*>(iovecs[i].iov_base);
            written.insert(written.end(), bytes, bytes + iovecs[i].iov_len);
            total += iovecs[i].iov_len;
        }

        if (written.size() < frame_header_size) {
            co_return elio::io::io_result{-EINVAL, 0};
        }

        auto request_header = frame_header::from_bytes(written.data());
        if (request_header.type != message_type::request ||
            request_header.method_id != ExistingStreamCreateMethod::id ||
            written.size() < frame_header_size + request_header.payload_length) {
            co_return elio::io::io_result{-EINVAL, 0};
        }

        buffer_view request_view(written.data() + frame_header_size,
                                 request_header.payload_length);
        auto [timeout_ms, request] =
            parse_request<TestRequest>(request_view, request_header.flags);
        if (!timeout_ms.has_value()) {
            co_return elio::io::io_result{-EINVAL, 0};
        }

        auto response = build_response(
            request_header.request_id, TestResponse{"published-before-wait"});
        auto response_header = response.first.to_bytes();

        {
            std::lock_guard<std::mutex> lock(state->mutex);
            state->request_value = request.value;
            state->inbound.assign(response_header.begin(), response_header.end());
            state->inbound.insert(state->inbound.end(),
                                  response.second.data(),
                                  response.second.data() + response.second.size());
            state->response_loaded = true;
            state->request_seen.store(true, std::memory_order_release);
        }

        state->response_available.set();

        // Keep call_impl suspended inside write_frame until receive_loop has
        // dispatched the response and returned to the next header read.
        co_await state->response_dispatched.wait();
        state->writev_returned_after_dispatch.store(
            state->next_header_read_started.load(std::memory_order_acquire),
            std::memory_order_release);

        if (state->fail_write_after_dispatch) {
            co_return elio::io::io_result{-ECONNRESET, 0};
        }

        if (total > static_cast<size_t>(INT32_MAX)) {
            co_return elio::io::io_result{-EOVERFLOW, 0};
        }
        co_return elio::io::io_result{static_cast<int32_t>(total), 0};
    }

    elio::coro::task<elio::io::io_result> poll_write() {
        co_return elio::io::io_result{0, 0};
    }

    bool is_valid() const noexcept {
        return !state->shutdown.load(std::memory_order_acquire);
    }

    void shutdown_socket() {
        {
            std::lock_guard<std::mutex> lock(state->mutex);
            state->shutdown.store(true, std::memory_order_release);
        }
        state->response_available.set();
        state->response_dispatched.set();
        state->shutdown_event.set();
    }
};

struct stalled_send_stream_state {
    std::atomic<bool> shutdown{false};
    std::atomic<size_t> cancellable_write_calls{0};
    std::atomic<size_t> cancellable_write_cancelled{0};
    std::atomic<size_t> writev_calls{0};
    std::atomic<bool> first_write_started{false};
    std::atomic<bool> accepted_partial_frame{false};
};

struct stalled_send_stream {
    std::shared_ptr<stalled_send_stream_state> state;

    explicit stalled_send_stream(std::shared_ptr<stalled_send_stream_state> s)
        : state(std::move(s)) {}

    elio::coro::task<elio::io::io_result> read_exactly(void*, size_t) {
        while (!state->shutdown.load(std::memory_order_acquire)) {
            co_await elio::time::sleep_for(std::chrono::milliseconds(1));
        }
        co_return elio::io::io_result{-ECANCELED, 0};
    }

    elio::coro::task<elio::io::io_result> write_exactly(const void*, size_t) {
        co_return elio::io::io_result{-ENOSYS, 0};
    }

    elio::coro::task<elio::io::io_result> write_exactly(
        const void*, size_t, elio::coro::cancel_token token) {
        auto call_index = state->cancellable_write_calls.fetch_add(
            1, std::memory_order_acq_rel);
        if (call_index == 0) {
            state->first_write_started.store(true, std::memory_order_release);
        }
        state->accepted_partial_frame.store(true, std::memory_order_release);

        while (!state->shutdown.load(std::memory_order_acquire)) {
            if (token.is_cancelled()) {
                state->cancellable_write_cancelled.fetch_add(
                    1, std::memory_order_acq_rel);
                co_return elio::io::io_result{-ECANCELED, 0};
            }

            auto result = co_await elio::time::sleep_for(
                std::chrono::milliseconds(1), token);
            if (result == elio::coro::cancel_result::cancelled) {
                state->cancellable_write_cancelled.fetch_add(
                    1, std::memory_order_acq_rel);
                co_return elio::io::io_result{-ECANCELED, 0};
            }
        }

        co_return elio::io::io_result{-ECONNRESET, 0};
    }

    elio::coro::task<elio::io::io_result> writev(struct iovec*, size_t) {
        state->writev_calls.fetch_add(1, std::memory_order_acq_rel);
        co_return elio::io::io_result{-ENOSYS, 0};
    }

    elio::coro::task<elio::io::io_result> poll_write() {
        co_return elio::io::io_result{0, 0};
    }

    bool is_valid() const noexcept {
        return !state->shutdown.load(std::memory_order_acquire);
    }

    void shutdown_socket() noexcept {
        state->shutdown.store(true, std::memory_order_release);
    }
};

struct bounded_reject_stream_state {
    std::mutex mutex;
    std::deque<uint8_t> inbound;
    std::function<void()> after_second_read;
    std::atomic<bool> shutdown{false};
    std::atomic<size_t> successful_reads{0};
    std::atomic<bool> allow_writes{false};
    std::atomic<size_t> total_writes_started{0};
    std::atomic<size_t> error_writes_started{0};
    std::atomic<size_t> error_writes_completed{0};
    std::atomic<size_t> pong_writes_started{0};
    std::atomic<size_t> pong_writes_completed{0};
    std::atomic<bool> first_write_started{false};
};

void enqueue_bounded_reject_frame(bounded_reject_stream_state& state,
                                  const frame_header& header,
                                  const buffer_writer& payload) {
    auto header_bytes = header.to_bytes();
    std::lock_guard<std::mutex> lock(state.mutex);
    for (uint8_t byte : header_bytes) {
        state.inbound.push_back(byte);
    }
    for (size_t i = 0; i < payload.size(); ++i) {
        state.inbound.push_back(payload.data()[i]);
    }
}

void enqueue_bounded_reject_frame(bounded_reject_stream_state& state,
                                  const frame_header& header) {
    buffer_writer empty;
    enqueue_bounded_reject_frame(state, header, empty);
}

struct bounded_reject_stream {
    std::shared_ptr<bounded_reject_stream_state> state;

    explicit bounded_reject_stream(std::shared_ptr<bounded_reject_stream_state> s)
        : state(std::move(s)) {}

    elio::coro::task<elio::io::io_result> read_exactly(void* data, size_t len) {
        auto* out = static_cast<uint8_t*>(data);
        while (!state->shutdown.load(std::memory_order_acquire)) {
            std::function<void()> after_read;
            bool completed = false;
            {
                std::lock_guard<std::mutex> lock(state->mutex);
                if (state->inbound.size() >= len) {
                    for (size_t i = 0; i < len; ++i) {
                        out[i] = state->inbound.front();
                        state->inbound.pop_front();
                    }
                    auto read_index = state->successful_reads.fetch_add(
                        1, std::memory_order_acq_rel);
                    if (read_index == 1) {
                        after_read = state->after_second_read;
                    }
                    completed = true;
                }
            }
            if (!completed) {
                co_await elio::time::sleep_for(elio::test::scaled_ms(1));
                continue;
            }
            if (after_read) {
                after_read();
            }
            co_return elio::io::io_result{static_cast<int32_t>(len), 0};
        }
        co_return elio::io::io_result{-ECANCELED, 0};
    }

    elio::coro::task<elio::io::io_result> write_exactly(const void*, size_t) {
        co_return elio::io::io_result{-ENOSYS, 0};
    }

    elio::coro::task<elio::io::io_result> write_exactly(
        const void* data, size_t length, elio::coro::cancel_token token) {
        auto call_index = state->total_writes_started.fetch_add(
            1, std::memory_order_acq_rel);
        if (call_index == 0) {
            state->first_write_started.store(true, std::memory_order_release);
        }

        message_type type = message_type::request;
        if (length >= frame_header_size) {
            auto header = frame_header::from_bytes(
                static_cast<const uint8_t*>(data));
            type = header.type;
            if (type == message_type::error) {
                state->error_writes_started.fetch_add(
                    1, std::memory_order_acq_rel);
            } else if (type == message_type::pong) {
                state->pong_writes_started.fetch_add(
                    1, std::memory_order_acq_rel);
            }
        }

        while (!state->shutdown.load(std::memory_order_acquire)) {
            if (token.is_cancelled()) {
                co_return elio::io::io_result{-ECANCELED, 0};
            }
            if (state->allow_writes.load(std::memory_order_acquire)) {
                if (type == message_type::error) {
                    state->error_writes_completed.fetch_add(
                        1, std::memory_order_acq_rel);
                } else if (type == message_type::pong) {
                    state->pong_writes_completed.fetch_add(
                        1, std::memory_order_acq_rel);
                }
                co_return elio::io::io_result{static_cast<int32_t>(length), 0};
            }
            auto result = co_await elio::time::sleep_for(
                elio::test::scaled_ms(1), token);
            if (result == elio::coro::cancel_result::cancelled) {
                co_return elio::io::io_result{-ECANCELED, 0};
            }
        }

        co_return elio::io::io_result{-ECONNRESET, 0};
    }

    elio::coro::task<elio::io::io_result> writev(struct iovec*, size_t) {
        co_return elio::io::io_result{-ENOSYS, 0};
    }

    elio::coro::task<elio::io::io_result> poll_write() {
        co_return elio::io::io_result{0, 0};
    }

    bool is_valid() const noexcept {
        return !state->shutdown.load(std::memory_order_acquire);
    }

    void shutdown_socket() noexcept {
        state->shutdown.store(true, std::memory_order_release);
        state->allow_writes.store(true, std::memory_order_release);
    }
};

struct disconnect_after_first_valid_stream_state {
    std::atomic<size_t> is_valid_calls{0};
    std::atomic<size_t> cancellable_write_calls{0};
    std::atomic<size_t> writev_calls{0};
    std::atomic<bool> shutdown{false};
};

struct disconnect_after_first_valid_stream {
    std::shared_ptr<disconnect_after_first_valid_stream_state> state;

    explicit disconnect_after_first_valid_stream(
        std::shared_ptr<disconnect_after_first_valid_stream_state> s)
        : state(std::move(s)) {}

    elio::coro::task<elio::io::io_result> read_exactly(void*, size_t) {
        co_return elio::io::io_result{-ECONNRESET, 0};
    }

    elio::coro::task<elio::io::io_result> write_exactly(const void*, size_t) {
        co_return elio::io::io_result{-ENOSYS, 0};
    }

    elio::coro::task<elio::io::io_result> write_exactly(
        const void*, size_t, elio::coro::cancel_token token) {
        state->cancellable_write_calls.fetch_add(1, std::memory_order_acq_rel);
        while (!state->shutdown.load(std::memory_order_acquire)) {
            if (token.is_cancelled()) {
                co_return elio::io::io_result{-ECANCELED, 0};
            }
            auto result = co_await elio::time::sleep_for(
                std::chrono::milliseconds(1), token);
            if (result == elio::coro::cancel_result::cancelled) {
                co_return elio::io::io_result{-ECANCELED, 0};
            }
        }
        co_return elio::io::io_result{-ECONNRESET, 0};
    }

    elio::coro::task<elio::io::io_result> writev(struct iovec*, size_t) {
        state->writev_calls.fetch_add(1, std::memory_order_acq_rel);
        co_return elio::io::io_result{-ENOSYS, 0};
    }

    elio::coro::task<elio::io::io_result> poll_write() {
        co_return elio::io::io_result{0, 0};
    }

    bool is_valid() const noexcept {
        auto call = state->is_valid_calls.fetch_add(1, std::memory_order_acq_rel);
        return call == 0;
    }

    void shutdown_socket() noexcept {
        state->shutdown.store(true, std::memory_order_release);
    }
};

} // namespace

TEST_CASE("rpc writev_exact retries transient writev errors",
          "[rpc][protocol][writev]") {
    SECTION("retries readiness and interruption before completing") {
        char first[] = {'a', 'b', 'c'};
        char second[] = {'d', 'e', 'f'};
        struct iovec iovecs[2] = {
            {first, sizeof(first)},
            {second, sizeof(second)}
        };

        scripted_rpc_stream stream;
        stream.writev_results = {
            {-EAGAIN, 0},
            {2, 0},
            {-EINTR, 0},
            {-EWOULDBLOCK, 0},
            {4, 0}
        };

        auto result = run_scripted_writev(stream, iovecs, 2);

        REQUIRE(result.result == 6);
        REQUIRE(stream.writev_calls == 5);
        REQUIRE(stream.poll_write_calls == 2);
        REQUIRE(stream.written == "abcdef");
    }

    SECTION("returns poll_write errors") {
        char data[] = {'x'};
        struct iovec iovecs[1] = {{data, sizeof(data)}};

        scripted_rpc_stream stream;
        stream.writev_results = {{-EAGAIN, 0}};
        stream.poll_write_results = {{-EPIPE, 0}};

        auto result = run_scripted_writev(stream, iovecs, 1);

        REQUIRE(result.result == -EPIPE);
        REQUIRE(stream.writev_calls == 1);
        REQUIRE(stream.poll_write_calls == 1);
        REQUIRE(stream.written.empty());
    }
}

TEST_CASE("rpc call observes response published before wait path",
          "[rpc][client][contract]") {
    using namespace elio::runtime;
    namespace coro = elio::coro;

    scheduler sched(2);
    sched.start();

    auto state = std::make_shared<fast_response_stream_state>();
    std::promise<std::pair<rpc_error, std::string>> result_promise;
    auto result_future = result_promise.get_future();

    sched.go([state, p = std::move(result_promise)]() mutable -> coro::task<void> {
        auto client = rpc_client<fast_response_stream>::create(
            fast_response_stream{state});
        auto result = co_await client->call<ExistingStreamCreateMethod>(
            TestRequest{17}, elio::test::scaled_sec(30));

        std::pair<rpc_error, std::string> observed{result.error(), {}};
        if (result.ok()) {
            observed.second = result->result;
        }
        client->close();
        p.set_value(std::move(observed));
    });

    REQUIRE(result_future.wait_for(elio::test::scaled_sec(5)) ==
            std::future_status::ready);

    auto [error, value] = result_future.get();
    REQUIRE(error == rpc_error::success);
    REQUIRE(value == "published-before-wait");
    REQUIRE(state->request_seen.load(std::memory_order_acquire));
    REQUIRE(state->request_value == 17);
    REQUIRE(state->next_header_read_started.load(std::memory_order_acquire));
    REQUIRE(state->writev_returned_after_dispatch.load(std::memory_order_acquire));

    REQUIRE(sched.shutdown(elio::test::scaled_sec(5)));
}

TEST_CASE("rpc call parses response published before failed send returns",
          "[rpc][client][contract]") {
    using namespace elio::runtime;
    namespace coro = elio::coro;

    scheduler sched(2);
    sched.start();

    auto state = std::make_shared<fast_response_stream_state>();
    state->fail_write_after_dispatch = true;
    std::promise<std::pair<rpc_error, std::string>> result_promise;
    auto result_future = result_promise.get_future();

    sched.go([state, p = std::move(result_promise)]() mutable -> coro::task<void> {
        auto client = rpc_client<fast_response_stream>::create(
            fast_response_stream{state});
        auto result = co_await client->call<ExistingStreamCreateMethod>(
            TestRequest{23}, elio::test::scaled_sec(30));

        std::pair<rpc_error, std::string> observed{result.error(), {}};
        if (result.ok()) {
            observed.second = result->result;
        }
        p.set_value(std::move(observed));
    });

    REQUIRE(result_future.wait_for(elio::test::scaled_sec(5)) ==
            std::future_status::ready);

    auto [error, value] = result_future.get();
    REQUIRE(error == rpc_error::success);
    REQUIRE(value == "published-before-wait");
    REQUIRE(state->request_seen.load(std::memory_order_acquire));
    REQUIRE(state->request_value == 23);
    REQUIRE(state->next_header_read_started.load(std::memory_order_acquire));
    REQUIRE(state->writev_returned_after_dispatch.load(std::memory_order_acquire));
    REQUIRE(state->shutdown.load(std::memory_order_acquire));

    REQUIRE(sched.shutdown(elio::test::scaled_sec(5)));
}

TEST_CASE("send_oneway writes a no-response request frame",
          "[rpc][contract][request_id]") {
    using namespace elio::net;
    using namespace elio::runtime;
    namespace coro = elio::coro;

    int fds[2] = {-1, -1};
    REQUIRE(::socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);

    auto client = tcp_rpc_client::create(tcp_stream(fds[0]));
    scheduler sched(2);
    sched.start();

    std::atomic<bool> done{false};
    std::atomic<bool> sent{false};
    sched.go([client, &done, &sent]() -> coro::task<void> {
        bool ok = co_await client->send_oneway<OnewayNoResponseMethod>(
            TestRequest{42});
        sent.store(ok, std::memory_order_release);
        done.store(true, std::memory_order_release);
    });

    std::array<uint8_t, frame_header_size> header_bytes{};
    size_t got = 0;
    while (got < header_bytes.size()) {
        ssize_t n = ::read(
            fds[1], header_bytes.data() + got, header_bytes.size() - got);
        REQUIRE(n > 0);
        got += static_cast<size_t>(n);
    }

    auto header = frame_header::from_bytes(header_bytes.data());
    std::vector<uint8_t> payload(header.payload_length);
    got = 0;
    while (got < payload.size()) {
        ssize_t n = ::read(fds[1], payload.data() + got, payload.size() - got);
        REQUIRE(n > 0);
        got += static_cast<size_t>(n);
    }

    for (int i = 0; i < 2000 && !done.load(std::memory_order_acquire); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    REQUIRE(done.load(std::memory_order_acquire));
    REQUIRE(sent.load(std::memory_order_acquire));
    REQUIRE(header.type == message_type::request);
    REQUIRE(header.method_id == OnewayNoResponseMethod::id);
    REQUIRE(has_flag(header.flags, message_flags::no_response));

    client.reset();
    REQUIRE(sched.shutdown(elio::test::scaled_sec(5)));
    ::close(fds[1]);
}

TEST_CASE("rpc_client create starts receive loop for existing streams",
          "[rpc][client][contract]") {
    using namespace elio::net;
    using namespace elio::runtime;
    namespace coro = elio::coro;

    auto listener_opt = tcp_listener::bind(ipv6_address("::1", 0));
    int bind_errno = errno;
    CAPTURE(bind_errno);
    if (!listener_opt && bind_errno == EPERM) {
        SKIP("TCP listener creation is denied by this sandbox");
    }
    REQUIRE(listener_opt.has_value());
    uint16_t port = listener_opt->local_address().port();

    scheduler sched(2);
    sched.start();

    std::promise<bool> server_promise;
    auto server_future = server_promise.get_future();
    std::promise<std::optional<std::string>> result_promise;
    auto result_future = result_promise.get_future();

    sched.go([&lst = *listener_opt, p = std::move(server_promise)]() mutable
                 -> coro::task<void> {
        auto stream = co_await lst.accept();
        if (!stream) {
            p.set_value(false);
            co_return;
        }

        auto frame = co_await read_frame(*stream);
        if (!frame) {
            p.set_value(false);
            co_return;
        }

        auto& [header, payload] = *frame;
        if (header.type != message_type::request ||
            header.method_id != ExistingStreamCreateMethod::id) {
            p.set_value(false);
            co_return;
        }

        buffer_view request_view(payload.data(), payload.size());
        auto [timeout_ms, request] = parse_request<TestRequest>(
            request_view, header.flags);
        if (!timeout_ms.has_value() || request.value != 7) {
            p.set_value(false);
            co_return;
        }

        auto response = build_response(
            header.request_id, TestResponse{"created"});
        bool sent = co_await write_frame(*stream, response.first, response.second);
        co_await stream->close();
        p.set_value(sent);
    });

    sched.go([port, p = std::move(result_promise)]() mutable -> coro::task<void> {
        auto stream = co_await tcp_connect(ipv6_address("::1", port));
        if (!stream) {
            p.set_value(std::nullopt);
            co_return;
        }

        auto client = tcp_rpc_client::create(std::move(*stream));
        auto result = co_await client->call<ExistingStreamCreateMethod>(
            TestRequest{7}, elio::test::scaled_sec(2));

        if (result.ok()) {
            p.set_value(result->result);
        } else {
            p.set_value(std::nullopt);
        }
        client->close();
    });

    REQUIRE(server_future.wait_for(elio::test::scaled_sec(5)) ==
            std::future_status::ready);
    REQUIRE(server_future.get());
    REQUIRE(result_future.wait_for(elio::test::scaled_sec(5)) ==
            std::future_status::ready);
    auto result = result_future.get();
    REQUIRE(result.has_value());
    REQUIRE(*result == "created");

    REQUIRE(sched.shutdown(elio::test::scaled_sec(5)));
}

TEST_CASE("rpc_client start attaches off-scheduler clients once",
          "[rpc][client][contract]") {
    using namespace elio::net;
    using namespace elio::runtime;
    namespace coro = elio::coro;

    int fds[2] = {-1, -1};
    REQUIRE(::socketpair(AF_UNIX, SOCK_STREAM, 0, fds) == 0);

    auto client = tcp_rpc_client::create(tcp_stream(fds[0]));
    REQUIRE_FALSE(client->start());

    scheduler sched(2);
    sched.start();

    std::promise<std::pair<bool, bool>> start_promise;
    auto start_future = start_promise.get_future();

    sched.go([client, p = std::move(start_promise)]() mutable -> coro::task<void> {
        bool first = client->start();
        bool second = client->start();
        p.set_value({first, second});
        client->close();
        co_return;
    });

    REQUIRE(start_future.wait_for(elio::test::scaled_sec(5)) ==
            std::future_status::ready);
    auto [first, second] = start_future.get();
    REQUIRE(first);
    REQUIRE(second);

    REQUIRE(sched.shutdown(elio::test::scaled_sec(5)));
    client.reset();
    ::close(fds[1]);
}

TEST_CASE("rpc_client close completes pending calls off scheduler",
          "[rpc][client][close]") {
    if (rpc_close_test_running_under_tsan()) {
        SKIP("fork-based deadlock regression is skipped under TSAN");
    }

    pid_t pid = ::fork();
    REQUIRE(pid >= 0);

    if (pid == 0) {
        ::_exit(run_rpc_close_off_scheduler_pending_child());
    }

    int status = 0;
    pid_t waited = -1;
    do {
        waited = ::waitpid(pid, &status, 0);
    } while (waited < 0 && errno == EINTR);

    REQUIRE(waited == pid);
    CAPTURE(status);
    if (WIFSIGNALED(status)) {
        CAPTURE(WTERMSIG(status));
    }
    REQUIRE(WIFEXITED(status));
    int exit_code = WEXITSTATUS(status);
    CAPTURE(exit_code);
    REQUIRE(exit_code == 0);
}

TEST_CASE("rpc session suppresses replies for no-response requests",
          "[rpc][contract][request_id]") {
    using namespace elio::net;
    using namespace elio::runtime;
    namespace coro = elio::coro;

    auto listener_opt = tcp_listener::bind(ipv6_address("::1", 0));
    REQUIRE(listener_opt.has_value());
    uint16_t port = listener_opt->local_address().port();

    rpc_server_config cfg;
    cfg.frame_read_timeout = std::chrono::seconds(1);
    auto server = std::make_shared<rpc_server<tcp_stream>>(cfg);

    std::atomic<bool> handler_called{false};
    std::atomic<bool> cleanup_called{false};
    server->register_method_with_cleanup<OnewayNoResponseMethod>(
        [&handler_called, &cleanup_called](const TestRequest& req)
            -> coro::task<std::pair<TestResponse, cleanup_callback_t>> {
            handler_called.store(true, std::memory_order_release);
            co_return std::make_pair(
                TestResponse{std::to_string(req.value)},
                [&cleanup_called] {
                    cleanup_called.store(true, std::memory_order_release);
                });
        });

    scheduler sched(2);
    sched.start();

    sched.go([&, &lst = *listener_opt]() -> coro::task<void> {
        auto stream = co_await lst.accept();
        if (!stream) co_return;
        co_await server->handle_client(std::move(*stream));
    });

    std::promise<bool> done_promise;
    auto done = done_promise.get_future();

    sched.go([&, p = std::move(done_promise), port]() mutable -> coro::task<void> {
        auto client = co_await tcp_connect(ipv6_address("::1", port));
        if (!client) {
            p.set_value(false);
            co_return;
        }

        auto frame = build_oneway_request(
            77, OnewayNoResponseMethod::id, TestRequest{42});
        bool sent_ok = co_await write_frame(*client, frame.first, frame.second);
        if (!sent_ok) {
            p.set_value(false);
            co_return;
        }

        for (int i = 0;
             i < 2000 &&
             !(handler_called.load(std::memory_order_acquire) &&
               cleanup_called.load(std::memory_order_acquire));
             ++i) {
            co_await elio::time::sleep_for(elio::test::scaled_ms(1));
        }

        co_await elio::time::sleep_for(elio::test::scaled_ms(50));

        uint8_t byte = 0;
        ssize_t n = ::recv(client->fd(), &byte, sizeof(byte), MSG_DONTWAIT);
        int recv_errno = errno;
        bool no_reply = n < 0 &&
            (recv_errno == EAGAIN || recv_errno == EWOULDBLOCK);

        co_await client->close();
        p.set_value(
            handler_called.load(std::memory_order_acquire) &&
            cleanup_called.load(std::memory_order_acquire) &&
            no_reply);
    });

    auto status = done.wait_for(std::chrono::seconds(60));
    REQUIRE(status == std::future_status::ready);
    REQUIRE(done.get());
    REQUIRE(sched.shutdown(elio::test::scaled_sec(5)));
}

TEST_CASE("rpc_server stop wakes TCP serve blocked in accept",
          "[rpc][server][shutdown]") {
    using namespace elio::net;
    using namespace elio::runtime;
    namespace coro = elio::coro;

    auto listener_opt = tcp_listener::bind(ipv6_address("::1", 0));
    int bind_errno = errno;
    CAPTURE(bind_errno);
    if (!listener_opt && bind_errno == EPERM) {
        SKIP("TCP listener creation is denied by this sandbox");
    }
    REQUIRE(listener_opt.has_value());

    rpc_server<tcp_stream> server;
    scheduler sched(1);
    sched.start();

    sched.go([&]() -> coro::task<void> {
        co_await server.serve(*listener_opt);
    });

    std::atomic<bool> stop_called{false};
    sched.go([&]() -> coro::task<void> {
        co_await elio::time::sleep_for(elio::test::scaled_ms(50));
        server.stop();
        stop_called.store(true, std::memory_order_release);
    });

    bool drained = sched.shutdown(elio::test::scaled_sec(5));
    REQUIRE(stop_called.load(std::memory_order_acquire));
    REQUIRE(drained);
    REQUIRE_FALSE(server.is_running());
}

TEST_CASE("rpc_server stop wakes UDS serve blocked in accept",
          "[rpc][server][shutdown][uds]") {
    using namespace elio::net;
    using namespace elio::runtime;
    namespace coro = elio::coro;

    auto addr = unix_address::abstract(
        "elio_rpc_stop_accept_" + std::to_string(getpid()));
    auto listener_opt = uds_listener::bind(addr);
    int bind_errno = errno;
    CAPTURE(bind_errno);
    if (!listener_opt && bind_errno == EPERM) {
        SKIP("UDS listener creation is denied by this sandbox");
    }
    REQUIRE(listener_opt.has_value());

    rpc_server<uds_stream> server;
    scheduler sched(1);
    sched.start();

    sched.go([&]() -> coro::task<void> {
        co_await server.serve(*listener_opt);
    });

    std::atomic<bool> stop_called{false};
    sched.go([&]() -> coro::task<void> {
        co_await elio::time::sleep_for(elio::test::scaled_ms(50));
        server.stop();
        stop_called.store(true, std::memory_order_release);
    });

    bool drained = sched.shutdown(elio::test::scaled_sec(5));
    REQUIRE(stop_called.load(std::memory_order_acquire));
    REQUIRE(drained);
    REQUIRE_FALSE(server.is_running());
}

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

TEST_CASE("rpc server rejects typed request trailing bytes before handler",
          "[rpc][server][contract]") {
    using namespace elio::net;
    using namespace elio::runtime;
    namespace coro = elio::coro;

    using Method = ELIO_RPC_METHOD(100, TestRequest, TestResponse);

    auto listener_opt = tcp_listener::bind(ipv6_address("::1", 0));
    int bind_errno = errno;
    CAPTURE(bind_errno);
    if (!listener_opt && bind_errno == EPERM) {
        SKIP("TCP listener creation is denied by this sandbox");
    }
    REQUIRE(listener_opt.has_value());
    uint16_t port = listener_opt->local_address().port();

    rpc_server_config cfg;
    cfg.frame_read_timeout = std::chrono::seconds(1);
    auto server = std::make_shared<rpc_server<tcp_stream>>(cfg);

    std::atomic<bool> handler_called{false};
    server->register_method<Method>(
        [&handler_called](const TestRequest& req)
            -> coro::task<TestResponse> {
            handler_called.store(true, std::memory_order_release);
            co_return TestResponse{std::to_string(req.value)};
        });

    scheduler sched(2);
    sched.start();

    sched.go([&, &lst = *listener_opt]() -> coro::task<void> {
        auto stream = co_await lst.accept();
        if (!stream) co_return;
        co_await server->handle_client(std::move(*stream));
    });

    std::promise<bool> done_promise;
    auto done = done_promise.get_future();

    sched.go([&, port, p = std::move(done_promise)]() mutable
        -> coro::task<void> {
        auto client = co_await tcp_connect(ipv6_address("::1", port));
        if (!client) {
            p.set_value(false);
            co_return;
        }

        auto frame = build_request(11, Method::id, TestRequest{7});
        frame.second.write<uint8_t>(0xAA);
        frame.first.payload_length = static_cast<uint32_t>(frame.second.size());

        bool sent = co_await write_frame(*client, frame.first, frame.second);
        if (!sent) {
            co_await client->close();
            p.set_value(false);
            co_return;
        }

        auto response = co_await read_frame(*client);
        bool rejected = false;
        if (response && response->first.type == message_type::error) {
            buffer_view view = response->second.view();
            auto err = parse_error(view);
            rejected = err.code == rpc_error::serialization_error;
        }

        co_await client->close();
        p.set_value(
            rejected &&
            !handler_called.load(std::memory_order_acquire));
    });

    auto status = done.wait_for(std::chrono::seconds(60));
    REQUIRE(status == std::future_status::ready);
    REQUIRE(done.get());
    REQUIRE_FALSE(handler_called.load(std::memory_order_acquire));
    REQUIRE(sched.shutdown(elio::test::scaled_sec(5)));
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

TEST_CASE("rpc_client frame_read_timeout fires on partial response frame",
          "[rpc][client][security][slow_loris]") {
    using namespace elio::net;
    using namespace elio::runtime;
    namespace coro = elio::coro;

    auto listener_opt = tcp_listener::bind(ipv6_address("::1", 0));
    REQUIRE(listener_opt.has_value());
    uint16_t port = listener_opt->local_address().port();

    rpc_client_config cfg;
    cfg.frame_read_timeout = std::chrono::seconds(1);
    cfg.max_message_size = 1024;

    scheduler sched(2);
    sched.start();

    std::atomic<bool> allow_server_close{false};
    std::promise<bool> server_done_promise;
    auto server_done = server_done_promise.get_future();
    sched.go([&, &lst = *listener_opt,
              p = std::move(server_done_promise)]() mutable -> coro::task<void> {
        auto stream = co_await lst.accept();
        if (!stream) {
            p.set_value(false);
            co_return;
        }

        auto request_frame = co_await read_frame(*stream);
        if (!request_frame ||
            request_frame->first.type != message_type::request) {
            p.set_value(false);
            co_return;
        }

        auto [response_header, response_payload] = build_response(
            request_frame->first.request_id, TestResponse{"late"});
        (void)response_payload;
        auto bytes = response_header.to_bytes();
        co_await stream->write(bytes.data(), 4);

        while (!allow_server_close.load(std::memory_order_acquire)) {
            co_await elio::time::sleep_for(std::chrono::milliseconds(10));
        }
        stream->shutdown_socket();
        co_await stream->close();
        p.set_value(true);
    });

    std::promise<int> call_done_promise;
    auto call_done = call_done_promise.get_future();
    sched.go([cfg, port, p = std::move(call_done_promise)]()
                 mutable -> coro::task<void> {
        auto client_opt = co_await tcp_rpc_client::connect_with_config(
            cfg, "::1", port);
        if (!client_opt) {
            p.set_value(static_cast<int>(rpc_error::connection_closed));
            co_return;
        }

        auto client = *client_opt;
        auto result = co_await client->call<ExistingStreamCreateMethod>(
            TestRequest{9}, elio::test::scaled_sec(30));
        p.set_value(static_cast<int>(result.error()));
        client->close();
    });

    auto call_status = call_done.wait_for(elio::test::scaled_sec(5));
    allow_server_close.store(true, std::memory_order_release);
    if (call_status != std::future_status::ready) {
        (void)call_done.wait_for(elio::test::scaled_sec(5));
        (void)server_done.wait_for(elio::test::scaled_sec(5));
        (void)sched.shutdown(elio::test::scaled_sec(5));
        FAIL("client call did not finish before peer close was allowed");
    }
    REQUIRE(call_status == std::future_status::ready);
    REQUIRE(call_done.get() == static_cast<int>(rpc_error::connection_closed));

    auto server_status = server_done.wait_for(elio::test::scaled_sec(5));
    REQUIRE(server_status == std::future_status::ready);
    REQUIRE(server_done.get());

    REQUIRE(sched.shutdown(elio::test::scaled_sec(5)));
}

TEST_CASE("rpc_client max_message_size rejects oversized response header",
          "[rpc][client][security]") {
    using namespace elio::net;
    using namespace elio::runtime;
    namespace coro = elio::coro;

    auto listener_opt = tcp_listener::bind(ipv6_address("::1", 0));
    REQUIRE(listener_opt.has_value());
    uint16_t port = listener_opt->local_address().port();

    rpc_client_config cfg;
    cfg.frame_read_timeout = std::chrono::seconds(0);
    cfg.max_message_size = 16;

    scheduler sched(2);
    sched.start();

    std::atomic<bool> allow_server_close{false};
    std::promise<bool> server_done_promise;
    auto server_done = server_done_promise.get_future();
    sched.go([&, &lst = *listener_opt,
              p = std::move(server_done_promise)]() mutable -> coro::task<void> {
        auto stream = co_await lst.accept();
        if (!stream) {
            p.set_value(false);
            co_return;
        }

        auto request_frame = co_await read_frame(*stream);
        if (!request_frame ||
            request_frame->first.type != message_type::request) {
            p.set_value(false);
            co_return;
        }

        auto [response_header, response_payload] = build_response(
            request_frame->first.request_id, TestResponse{"too large"});
        (void)response_payload;
        response_header.payload_length = cfg.max_message_size + 1;
        auto bytes = response_header.to_bytes();
        co_await stream->write(bytes.data(), bytes.size());

        while (!allow_server_close.load(std::memory_order_acquire)) {
            co_await elio::time::sleep_for(std::chrono::milliseconds(10));
        }
        stream->shutdown_socket();
        co_await stream->close();
        p.set_value(true);
    });

    std::promise<int> call_done_promise;
    auto call_done = call_done_promise.get_future();
    sched.go([cfg, port, p = std::move(call_done_promise)]()
                 mutable -> coro::task<void> {
        auto client_opt = co_await tcp_rpc_client::connect_with_config(
            cfg, "::1", port);
        if (!client_opt) {
            p.set_value(static_cast<int>(rpc_error::connection_closed));
            co_return;
        }

        auto client = *client_opt;
        auto result = co_await client->call<ExistingStreamCreateMethod>(
            TestRequest{10}, elio::test::scaled_sec(30));
        p.set_value(static_cast<int>(result.error()));
        client->close();
    });

    auto call_status = call_done.wait_for(elio::test::scaled_sec(5));
    allow_server_close.store(true, std::memory_order_release);
    if (call_status != std::future_status::ready) {
        (void)call_done.wait_for(elio::test::scaled_sec(5));
        (void)server_done.wait_for(elio::test::scaled_sec(5));
        (void)sched.shutdown(elio::test::scaled_sec(5));
        FAIL("client call did not finish before peer close was allowed");
    }
    REQUIRE(call_status == std::future_status::ready);
    REQUIRE(call_done.get() == static_cast<int>(rpc_error::connection_closed));

    auto server_status = server_done.wait_for(elio::test::scaled_sec(5));
    REQUIRE(server_status == std::future_status::ready);
    REQUIRE(server_done.get());

    REQUIRE(sched.shutdown(elio::test::scaled_sec(5)));
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

TEST_CASE("server per-session in-flight limit rejects excess request",
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
    cfg.max_in_flight_requests_per_session = 1;
    cfg.request_overload_policy = rpc_request_overload_policy::reject_request;
    cfg.frame_read_timeout = std::chrono::seconds(0);
    auto server = std::make_shared<rpc_server<tcp_stream>>(cfg);

    elio::sync::event release_first;
    std::atomic<int> handler_started{0};
    std::atomic<int> handler_finished{0};
    server->register_method<DelayMethod>(
        [&release_first, &handler_started, &handler_finished](const DelayReq& r)
            -> coro::task<DelayResp> {
            handler_started.fetch_add(1, std::memory_order_acq_rel);
            co_await release_first.wait();
            handler_finished.fetch_add(1, std::memory_order_acq_rel);
            co_return DelayResp{r.ms};
        });

    scheduler sched(4);
    sched.start();

    sched.go([&, &lst = *listener_opt]() -> coro::task<void> {
        auto s = co_await lst.accept();
        if (!s) co_return;
        co_await server->handle_client(std::move(*s));
    });

    std::promise<void> client_done_promise;
    auto client_done = client_done_promise.get_future();
    std::atomic<int> second_error{static_cast<int>(rpc_error::success)};
    std::atomic<int64_t> second_elapsed_ms{-1};
    std::atomic<bool> first_sent{false};
    std::atomic<bool> overlimit_oneway_sent{false};
    std::atomic<bool> ping_ok{false};

    sched.go([&, p = std::move(client_done_promise)]() mutable -> coro::task<void> {
        auto client_opt = co_await tcp_rpc_client::connect("::1", port);
        REQUIRE(client_opt.has_value());
        auto client = *client_opt;

        auto call_timeout = elio::test::scaled_sec(30);
        first_sent.store(co_await client->send_oneway<DelayMethod>(DelayReq{111}),
                         std::memory_order_release);
        REQUIRE(first_sent.load(std::memory_order_acquire));

        for (int i = 0;
             i < 200 && handler_started.load(std::memory_order_acquire) == 0;
             ++i) {
            co_await elio::time::sleep_for(std::chrono::milliseconds(5));
        }
        REQUIRE(handler_started.load(std::memory_order_acquire) == 1);

        overlimit_oneway_sent.store(
            co_await client->send_oneway<DelayMethod>(DelayReq{222}),
            std::memory_order_release);
        REQUIRE(overlimit_oneway_sent.load(std::memory_order_acquire));

        ping_ok.store(co_await client->ping(elio::test::scaled_sec(30)),
                      std::memory_order_release);
        REQUIRE(ping_ok.load(std::memory_order_acquire));

        auto second_start = std::chrono::steady_clock::now();
        auto second = co_await client->call<DelayMethod>(
            DelayReq{333}, call_timeout);
        auto second_elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - second_start);
        second_elapsed_ms.store(second_elapsed.count(), std::memory_order_release);
        second_error.store(
            static_cast<int>(second.error()), std::memory_order_release);

        release_first.set();
        for (int i = 0;
             i < 200 && handler_finished.load(std::memory_order_acquire) == 0;
             ++i) {
            co_await elio::time::sleep_for(std::chrono::milliseconds(5));
        }
        p.set_value();
    });

    auto status = client_done.wait_for(std::chrono::seconds(60));
    REQUIRE(status == std::future_status::ready);
    REQUIRE(second_error.load(std::memory_order_acquire) ==
            static_cast<int>(rpc_error::resource_exhausted));
    REQUIRE(first_sent.load(std::memory_order_acquire));
    REQUIRE(overlimit_oneway_sent.load(std::memory_order_acquire));
    REQUIRE(ping_ok.load(std::memory_order_acquire));
    REQUIRE(second_elapsed_ms.load(std::memory_order_acquire) >= 0);
    REQUIRE(second_elapsed_ms.load(std::memory_order_acquire) <
            elio::test::scaled_ms(500).count());
    REQUIRE(handler_started.load(std::memory_order_acquire) == 1);
    REQUIRE(handler_finished.load(std::memory_order_acquire) == 1);

    server->stop();
    sched.shutdown(std::chrono::seconds(30));
}

TEST_CASE("server per-session overload rejection is single-flight",
          "[rpc][security][concurrent]") {
    using namespace elio::runtime;
    namespace coro = elio::coro;

    using DelayReq = ConcurrencyDelayReq;
    using DelayResp = ConcurrencyDelayResp;
    using DelayMethod = ConcurrencyDelayMethod;

    rpc_server_config cfg;
    cfg.max_in_flight_requests_per_session = 1;
    cfg.request_overload_policy = rpc_request_overload_policy::reject_request;
    cfg.frame_read_timeout = std::chrono::seconds(0);

    auto server = std::make_shared<rpc_server<bounded_reject_stream>>(cfg);
    elio::sync::event release_handler;
    std::atomic<bool> handler_started{false};
    std::atomic<bool> handler_cancelled{false};
    server->register_method_with_context<DelayMethod>(
        [&release_handler, &handler_started, &handler_cancelled](
            const rpc_context& ctx,
            const DelayReq& req) -> coro::task<DelayResp> {
            handler_started.store(true, std::memory_order_release);

            for (int i = 0; i < 2000; ++i) {
                if (ctx.cancel_token.is_cancelled()) {
                    handler_cancelled.store(true, std::memory_order_release);
                    break;
                }
                co_await elio::time::sleep_for(elio::test::scaled_ms(1));
            }

            co_await release_handler.wait();
            co_return DelayResp{req.ms};
        });

    auto state = std::make_shared<bounded_reject_stream_state>();
    auto first = build_oneway_request(
        1, DelayMethod::id, DelayReq{1});
    enqueue_bounded_reject_frame(*state, first.first, first.second);

    for (uint32_t request_id = 2; request_id <= 4; ++request_id) {
        auto over_limit = build_request<DelayReq>(
            request_id, DelayMethod::id,
            DelayReq{static_cast<int32_t>(request_id)});
        enqueue_bounded_reject_frame(
            *state, over_limit.first, over_limit.second);
    }

    scheduler sched(4);
    sched.start();

    std::atomic<bool> server_done{false};
    sched.go([&, state]() -> coro::task<void> {
        co_await server->handle_client(bounded_reject_stream{state});
        server_done.store(true, std::memory_order_release);
    });

    for (int i = 0;
         i < 2000 &&
             state->error_writes_started.load(std::memory_order_acquire) == 0;
         ++i) {
        std::this_thread::sleep_for(elio::test::scaled_ms(1));
    }

    REQUIRE(handler_started.load(std::memory_order_acquire));
    REQUIRE(state->error_writes_started.load(std::memory_order_acquire) == 1);
    REQUIRE(state->error_writes_completed.load(std::memory_order_acquire) == 0);

    enqueue_bounded_reject_frame(*state, build_ping(99));
    enqueue_bounded_reject_frame(*state, build_cancel(1));

    for (int i = 0;
         i < 2000 && !handler_cancelled.load(std::memory_order_acquire);
         ++i) {
        std::this_thread::sleep_for(elio::test::scaled_ms(1));
    }

    REQUIRE(handler_cancelled.load(std::memory_order_acquire));
    REQUIRE(state->error_writes_started.load(std::memory_order_acquire) == 1);
    REQUIRE(state->pong_writes_started.load(std::memory_order_acquire) == 0);

    state->allow_writes.store(true, std::memory_order_release);
    for (int i = 0;
         i < 2000 &&
             (state->error_writes_completed.load(std::memory_order_acquire) < 1 ||
              state->pong_writes_completed.load(std::memory_order_acquire) < 1);
         ++i) {
        std::this_thread::sleep_for(elio::test::scaled_ms(1));
    }

    REQUIRE(state->error_writes_completed.load(std::memory_order_acquire) == 1);
    REQUIRE(state->pong_writes_completed.load(std::memory_order_acquire) == 1);

    auto after_reset = build_request<DelayReq>(
        5, DelayMethod::id, DelayReq{5});
    enqueue_bounded_reject_frame(
        *state, after_reset.first, after_reset.second);
    for (int i = 0;
         i < 2000 &&
             state->error_writes_completed.load(std::memory_order_acquire) < 2;
         ++i) {
        std::this_thread::sleep_for(elio::test::scaled_ms(1));
    }
    REQUIRE(state->error_writes_started.load(std::memory_order_acquire) == 2);
    REQUIRE(state->error_writes_completed.load(std::memory_order_acquire) == 2);

    release_handler.set();
    server->stop();
    for (int i = 0;
         i < 2000 && !server_done.load(std::memory_order_acquire);
         ++i) {
        std::this_thread::sleep_for(elio::test::scaled_ms(1));
    }
    REQUIRE(server_done.load(std::memory_order_acquire));
    REQUIRE(sched.shutdown(std::chrono::seconds(30)));
}

TEST_CASE("server session teardown cancels and joins handler tasks",
          "[rpc][cancel][lifetime]") {
    using namespace elio::runtime;
    namespace coro = elio::coro;

    using DelayReq = ConcurrencyDelayReq;
    using DelayResp = ConcurrencyDelayResp;
    using DelayMethod = ConcurrencyDelayMethod;

    rpc_server_config cfg;
    cfg.frame_read_timeout = std::chrono::seconds(0);

    auto server = std::make_shared<rpc_server<bounded_reject_stream>>(cfg);
    elio::sync::event release_handler;
    elio::sync::event explicit_wait;
    elio::sync::event runtime_wait;
    std::atomic<bool> handler_started{false};
    std::atomic<bool> explicit_cancel_seen{false};
    std::atomic<bool> runtime_cancel_seen{false};
    std::atomic<bool> reentrant_callback_done{false};
    std::atomic<size_t> reentrant_session_count{0};
    std::atomic<bool> handler_finished{false};
    auto* server_ptr = server.get();

    server->register_method_with_context<DelayMethod>(
        [&, server_ptr](const rpc_context& ctx,
                        const DelayReq& req) -> coro::task<DelayResp> {
            handler_started.store(true, std::memory_order_release);

            [[maybe_unused]] auto reentrant_callback =
                ctx.cancel_token.on_cancel(
                    [&, server_ptr] {
                        reentrant_session_count.store(
                            server_ptr->session_count(),
                            std::memory_order_release);
                        reentrant_callback_done.store(
                            true, std::memory_order_release);
                    });

            auto explicit_result = co_await explicit_wait.wait(ctx.cancel_token);
            explicit_cancel_seen.store(
                explicit_result == coro::cancel_result::cancelled,
                std::memory_order_release);

            auto runtime_result = co_await runtime_wait.wait(
                coro::this_coro::cancel_token());
            runtime_cancel_seen.store(
                runtime_result == coro::cancel_result::cancelled,
                std::memory_order_release);

            // Deliberately stop observing cancellation after recording it. The
            // session must retain and join this accepted handler until it exits.
            co_await release_handler.wait();
            handler_finished.store(true, std::memory_order_release);
            co_return DelayResp{req.ms};
        });

    auto state = std::make_shared<bounded_reject_stream_state>();
    auto request = build_oneway_request(
        1, DelayMethod::id, DelayReq{1});
    enqueue_bounded_reject_frame(*state, request.first, request.second);

    scheduler sched(3);
    sched.start();

    std::atomic<bool> server_done{false};
    sched.go([&, state]() -> coro::task<void> {
        co_await server->handle_client(bounded_reject_stream{state});
        server_done.store(true, std::memory_order_release);
    });

    for (int i = 0;
         i < 2000 && !handler_started.load(std::memory_order_acquire);
         ++i) {
        std::this_thread::sleep_for(elio::test::scaled_ms(1));
    }
    CHECK(handler_started.load(std::memory_order_acquire));

    server->stop();
    for (int i = 0;
         i < 2000 &&
             (!explicit_cancel_seen.load(std::memory_order_acquire) ||
              !runtime_cancel_seen.load(std::memory_order_acquire) ||
              !reentrant_callback_done.load(std::memory_order_acquire));
         ++i) {
        std::this_thread::sleep_for(elio::test::scaled_ms(1));
    }

    CHECK(explicit_cancel_seen.load(std::memory_order_acquire));
    CHECK(runtime_cancel_seen.load(std::memory_order_acquire));
    CHECK(reentrant_callback_done.load(std::memory_order_acquire));
    CHECK(reentrant_session_count.load(std::memory_order_acquire) == 1);
    CHECK_FALSE(server_done.load(std::memory_order_acquire));

    release_handler.set();
    for (int i = 0;
         i < 2000 && !server_done.load(std::memory_order_acquire);
         ++i) {
        std::this_thread::sleep_for(elio::test::scaled_ms(1));
    }

    auto shutdown_ok = sched.shutdown(std::chrono::seconds(30));
    REQUIRE(handler_finished.load(std::memory_order_acquire));
    REQUIRE(server_done.load(std::memory_order_acquire));
    REQUIRE(shutdown_ok);
}

TEST_CASE("server close rejects a frame that completed during teardown",
          "[rpc][cancel][lifetime][race]") {
    using namespace elio::runtime;
    namespace coro = elio::coro;

    using DelayReq = ConcurrencyDelayReq;
    using DelayResp = ConcurrencyDelayResp;
    using DelayMethod = ConcurrencyDelayMethod;

    rpc_server_config cfg;
    cfg.frame_read_timeout = std::chrono::seconds(0);

    auto server = std::make_shared<rpc_server<bounded_reject_stream>>(cfg);
    std::atomic<bool> handler_started{false};
    server->register_method<DelayMethod>(
        [&](const DelayReq& req) -> coro::task<DelayResp> {
            handler_started.store(true, std::memory_order_release);
            co_return DelayResp{req.ms};
        });

    auto state = std::make_shared<bounded_reject_stream_state>();
    state->after_second_read = [server] {
        server->stop();
    };
    auto request = build_oneway_request(1, DelayMethod::id, DelayReq{1});
    enqueue_bounded_reject_frame(*state, request.first, request.second);

    scheduler sched(2);
    sched.start();

    std::atomic<bool> server_done{false};
    sched.go([&, state]() -> coro::task<void> {
        co_await server->handle_client(bounded_reject_stream{state});
        server_done.store(true, std::memory_order_release);
    });

    for (int i = 0;
         i < 2000 && !server_done.load(std::memory_order_acquire);
         ++i) {
        std::this_thread::sleep_for(elio::test::scaled_ms(1));
    }

    auto shutdown_ok = sched.shutdown(std::chrono::seconds(30));
    REQUIRE(state->successful_reads.load(std::memory_order_acquire) == 2);
    REQUIRE_FALSE(handler_started.load(std::memory_order_acquire));
    REQUIRE(server_done.load(std::memory_order_acquire));
    REQUIRE(shutdown_ok);
}

TEST_CASE("cancelling handle_client owner closes its pending frame read",
          "[rpc][cancel][lifetime]") {
    using namespace elio::runtime;
    namespace coro = elio::coro;

    rpc_server_config cfg;
    cfg.frame_read_timeout = std::chrono::seconds(0);

    auto server = std::make_shared<rpc_server<bounded_reject_stream>>(cfg);
    auto state = std::make_shared<bounded_reject_stream_state>();

    scheduler sched(2);
    sched.start();

    std::atomic<bool> owner_done{false};
    auto owner = sched.go_joinable([&, state]() -> coro::task<void> {
        co_await server->handle_client(bounded_reject_stream{state});
        owner_done.store(true, std::memory_order_release);
    });

    for (int i = 0; i < 2000 && server->session_count() == 0; ++i) {
        std::this_thread::sleep_for(elio::test::scaled_ms(1));
    }
    CHECK(server->session_count() == 1);

    owner.request_cancel();
    owner.wait_destroyed();
    CHECK_NOTHROW(owner.await_resume());

    auto shutdown_ok = sched.shutdown(std::chrono::seconds(30));
    REQUIRE(state->shutdown.load(std::memory_order_acquire));
    REQUIRE(owner_done.load(std::memory_order_acquire));
    REQUIRE(server->session_count() == 0);
    REQUIRE(shutdown_ok);
}

TEST_CASE("server facade may be released after stopped serve returns",
          "[rpc][cancel][lifetime][tcp]") {
    using namespace elio::net;
    using namespace elio::runtime;
    namespace coro = elio::coro;

    using DelayReq = ConcurrencyDelayReq;
    using DelayResp = ConcurrencyDelayResp;
    using DelayMethod = ConcurrencyDelayMethod;

    auto listener = tcp_listener::bind(ipv6_address("::1", 0));
    REQUIRE(listener.has_value());
    auto port = listener->local_address().port();

    rpc_server_config cfg;
    cfg.frame_read_timeout = std::chrono::seconds(0);
    auto server = std::make_shared<tcp_rpc_server>(cfg);

    elio::sync::event release_handler;
    elio::sync::event release_client;
    std::atomic<bool> handler_started{false};
    std::atomic<bool> handler_finished{false};
    std::atomic<bool> request_sent{false};
    std::atomic<bool> client_done{false};

    server->register_method<DelayMethod>(
        [&](const DelayReq& req) -> coro::task<DelayResp> {
            handler_started.store(true, std::memory_order_release);
            co_await release_handler.wait();
            handler_finished.store(true, std::memory_order_release);
            co_return DelayResp{req.ms};
        });

    scheduler sched(4);
    sched.start();

    auto serve_owner = sched.go_joinable(
        [server, &listener]() -> coro::task<void> {
            co_await server->serve(*listener);
        });

    sched.go([&, port]() -> coro::task<void> {
        auto stream = co_await tcp_connect(ipv6_address("::1", port));
        if (!stream) {
            client_done.store(true, std::memory_order_release);
            co_return;
        }

        auto request = build_oneway_request(1, DelayMethod::id, DelayReq{1});
        request_sent.store(
            co_await write_frame(*stream, request.first, request.second),
            std::memory_order_release);
        co_await release_client.wait();
        stream->shutdown_socket();
        co_await stream->close();
        client_done.store(true, std::memory_order_release);
    });

    for (int i = 0;
         i < 2000 && !handler_started.load(std::memory_order_acquire);
         ++i) {
        std::this_thread::sleep_for(elio::test::scaled_ms(1));
    }
    CHECK(request_sent.load(std::memory_order_acquire));
    CHECK(handler_started.load(std::memory_order_acquire));

    server->stop();
    serve_owner.wait_destroyed();
    CHECK_NOTHROW(serve_owner.await_resume());
    CHECK(server->session_count() == 1);
    CHECK_FALSE(handler_finished.load(std::memory_order_acquire));

    std::weak_ptr server_lifetime = server;
    server.reset();
    CHECK(server_lifetime.expired());

    release_handler.set();
    release_client.set();
    for (int i = 0;
         i < 2000 &&
             (!handler_finished.load(std::memory_order_acquire) ||
              !client_done.load(std::memory_order_acquire));
         ++i) {
        std::this_thread::sleep_for(elio::test::scaled_ms(1));
    }

    auto shutdown_ok = sched.shutdown(std::chrono::seconds(30));
    REQUIRE(handler_finished.load(std::memory_order_acquire));
    REQUIRE(client_done.load(std::memory_order_acquire));
    REQUIRE(shutdown_ok);
}

TEST_CASE("server per-session in-flight limit can close the session",
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
    cfg.max_in_flight_requests_per_session = 1;
    cfg.request_overload_policy = rpc_request_overload_policy::close_session;
    cfg.frame_read_timeout = std::chrono::seconds(0);
    auto server = std::make_shared<rpc_server<tcp_stream>>(cfg);

    elio::sync::event release_first;
    std::atomic<int> handler_started{0};
    server->register_method<DelayMethod>(
        [&release_first, &handler_started](const DelayReq& r)
            -> coro::task<DelayResp> {
            handler_started.fetch_add(1, std::memory_order_acq_rel);
            co_await release_first.wait();
            co_return DelayResp{r.ms};
        });

    scheduler sched(4);
    sched.start();

    sched.go([&, &lst = *listener_opt]() -> coro::task<void> {
        auto s = co_await lst.accept();
        if (!s) co_return;
        co_await server->handle_client(std::move(*s));
    });

    std::promise<void> client_done_promise;
    auto client_done = client_done_promise.get_future();
    std::atomic<int> second_error{static_cast<int>(rpc_error::success)};
    std::atomic<int> first_error{static_cast<int>(rpc_error::success)};

    sched.go([&, p = std::move(client_done_promise)]() mutable -> coro::task<void> {
        auto client_opt = co_await tcp_rpc_client::connect("::1", port);
        REQUIRE(client_opt.has_value());
        auto client = *client_opt;
        auto* current_sched = elio::runtime::scheduler::current();
        REQUIRE(current_sched != nullptr);

        auto call_timeout = std::chrono::seconds(5);
        auto first = current_sched->go_joinable(
            [c = client, call_timeout]() -> coro::task<int> {
                auto r = co_await c->call<DelayMethod>(DelayReq{111}, call_timeout);
                co_return static_cast<int>(r.error());
            });

        for (int i = 0;
             i < 200 && handler_started.load(std::memory_order_acquire) == 0;
             ++i) {
            co_await elio::time::sleep_for(std::chrono::milliseconds(5));
        }
        REQUIRE(handler_started.load(std::memory_order_acquire) == 1);

        auto second = co_await client->call<DelayMethod>(
            DelayReq{222}, call_timeout);
        second_error.store(
            static_cast<int>(second.error()), std::memory_order_release);

        release_first.set();
        first_error.store(co_await std::move(first), std::memory_order_release);
        p.set_value();
    });

    auto status = client_done.wait_for(std::chrono::seconds(60));
    REQUIRE(status == std::future_status::ready);
    REQUIRE(second_error.load(std::memory_order_acquire) ==
            static_cast<int>(rpc_error::connection_closed));
    REQUIRE(first_error.load(std::memory_order_acquire) ==
            static_cast<int>(rpc_error::connection_closed));
    REQUIRE(handler_started.load(std::memory_order_acquire) == 1);

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

// ============================================================================
// call() timeout-watcher cancellation regression (issue #246)
// ============================================================================
//
// rpc_client::call_impl() spawns a detached timeout watcher that sleeps for
// the full per-call timeout. Before the fix, that watcher observed ONLY the
// caller-supplied cancellation token, so a call that completed normally (fast
// server response) left the watcher sleeping until the timeout expired. Each
// such watcher is a tracked scheduler task (spawned via go()) that also holds
// a shared_ptr<pending_request>, so it retained state and an idle task for the
// full timeout window.
//
// This test drives a fast round-trip with a deliberately LONG per-call
// timeout, then asserts the scheduler drains well within a budget far shorter
// than that timeout. Because the watcher is a tracked task, an un-cancelled
// watcher keeps active_tasks() > 0, so shutdown(budget) would return false
// (and block for the whole long timeout). With the fix the watcher is
// cancelled the moment the call completes, and the scheduler drains promptly.

// File-scope method types (ELIO_RPC_FIELDS inside a function body triggers
// -Wunused-local-typedefs; matches the pattern used earlier in this file).
struct WatcherEchoReq { int32_t value; ELIO_RPC_FIELDS(WatcherEchoReq, value) };
struct WatcherEchoResp { int32_t value; ELIO_RPC_FIELDS(WatcherEchoResp, value) };
using WatcherEchoMethod = ELIO_RPC_METHOD(303, WatcherEchoReq, WatcherEchoResp);

struct CancelProbeReq { int32_t value; ELIO_RPC_FIELDS(CancelProbeReq, value) };
struct CancelProbeResp { int32_t value; ELIO_RPC_FIELDS(CancelProbeResp, value) };
using CancelProbeMethod = ELIO_RPC_METHOD(304, CancelProbeReq, CancelProbeResp);

TEST_CASE("call timeout watcher is cancelled after completion",
          "[rpc][timeout][cancel]") {
    using namespace elio::net;
    using namespace elio::runtime;
    namespace coro = elio::coro;

    auto listener_opt = tcp_listener::bind(ipv6_address("::1", 0));
    REQUIRE(listener_opt.has_value());
    uint16_t port = listener_opt->local_address().port();

    auto server = std::make_shared<rpc_server<tcp_stream>>();
    server->register_method<WatcherEchoMethod>(
        [](const WatcherEchoReq& r) -> coro::task<WatcherEchoResp> {
            // Respond immediately — the call completes long before its
            // per-call timeout would ever fire.
            co_return WatcherEchoResp{r.value};
        });

    scheduler sched(2);
    sched.start();

    // Server: accept exactly one connection and serve it.
    sched.go([&, &lst = *listener_opt]() -> coro::task<void> {
        auto s = co_await lst.accept();
        if (!s) co_return;
        co_await server->handle_client(std::move(*s));
    });

    // A per-call timeout that is enormous relative to the round-trip. If the
    // watcher is not cancelled, it sleeps this long and keeps the scheduler
    // from draining.
    const auto call_timeout = elio::test::scaled_sec(30);

    std::promise<bool> call_ok_promise;
    auto call_ok = call_ok_promise.get_future();

    sched.go([&, p = std::move(call_ok_promise)]() mutable -> coro::task<void> {
        auto client_opt = co_await tcp_rpc_client::connect("::1", port);
        if (!client_opt) { p.set_value(false); co_return; }
        auto client = *client_opt;

        auto r = co_await client->call<WatcherEchoMethod>(
            WatcherEchoReq{42}, call_timeout);

        bool ok = r.ok() && r->value == 42;

        // Close the client so its receive loop's blocked read returns and the
        // loop exits. This does NOT touch the per-call timeout watcher, which
        // is what we are exercising: with the bug it keeps sleeping for
        // call_timeout regardless of close().
        client->close();

        p.set_value(ok);
    });

    // The call itself must succeed quickly (generous ceiling for slow CI).
    auto status = call_ok.wait_for(std::chrono::seconds(60));
    REQUIRE(status == std::future_status::ready);
    REQUIRE(call_ok.get() == true);

    // Let the server-side session loop exit too, so the only thing that could
    // still be keeping the scheduler busy is a leaked timeout watcher.
    server->stop();

    // Drain budget is comfortably larger than any legitimate teardown yet far
    // smaller than call_timeout. With the fix the watcher is already
    // cancelled, so this drains fast and returns true. Without the fix the
    // sleeping watcher is still a tracked task, so drain would only complete
    // after call_timeout (30s+) — i.e. this budget expires and drained==false.
    const auto drain_budget = elio::test::scaled_sec(5);
    auto drain_start = std::chrono::steady_clock::now();
    bool drained = sched.shutdown(drain_budget);
    auto drain_elapsed = std::chrono::steady_clock::now() - drain_start;

    REQUIRE(drained);
    // Sanity: draining must be far quicker than the per-call timeout, proving
    // we did not simply wait out the watcher's sleep.
    REQUIRE(drain_elapsed < call_timeout);
}

TEST_CASE("rpc call timeout cancels a stalled request-frame write",
          "[rpc][timeout][cancel]") {
    using namespace elio::runtime;
    namespace coro = elio::coro;

    auto state = std::make_shared<stalled_send_stream_state>();
    auto client = rpc_client<stalled_send_stream>::create(
        stalled_send_stream{state});

    scheduler sched(2);
    sched.start();

    std::promise<int> done_promise;
    auto done = done_promise.get_future();

    sched.go([client, p = std::move(done_promise)]() mutable -> coro::task<void> {
        auto result = co_await client->call<ExistingStreamCreateMethod>(
            TestRequest{7}, elio::test::scaled_ms(100));
        p.set_value(static_cast<int>(result.error()));
    });

    auto status = done.wait_for(elio::test::scaled_sec(5));
    REQUIRE(status == std::future_status::ready);
    REQUIRE(done.get() == static_cast<int>(rpc_error::timeout));
    REQUIRE(state->cancellable_write_calls.load(std::memory_order_acquire) == 1);
    REQUIRE(state->cancellable_write_cancelled.load(std::memory_order_acquire) == 1);
    REQUIRE(state->writev_calls.load(std::memory_order_acquire) == 0);
    REQUIRE(state->accepted_partial_frame.load(std::memory_order_acquire));
    REQUIRE(state->shutdown.load(std::memory_order_acquire));

    REQUIRE(sched.shutdown(elio::test::scaled_sec(5)));
}

TEST_CASE("rpc call timeout covers waiting for the request send mutex",
          "[rpc][timeout][cancel]") {
    using namespace elio::runtime;
    namespace coro = elio::coro;

    auto state = std::make_shared<stalled_send_stream_state>();
    auto client = rpc_client<stalled_send_stream>::create(
        stalled_send_stream{state});

    scheduler sched(2);
    sched.start();

    std::promise<int> first_done_promise;
    auto first_done = first_done_promise.get_future();
    sched.go([client, p = std::move(first_done_promise)]() mutable -> coro::task<void> {
        auto result = co_await client->call<ExistingStreamCreateMethod>(
            TestRequest{1}, elio::test::scaled_sec(30));
        p.set_value(static_cast<int>(result.error()));
    });

    for (int i = 0;
         i < 2000 && !state->first_write_started.load(std::memory_order_acquire);
         ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    REQUIRE(state->first_write_started.load(std::memory_order_acquire));

    std::promise<int> second_done_promise;
    auto second_done = second_done_promise.get_future();
    sched.go([client, p = std::move(second_done_promise)]() mutable -> coro::task<void> {
        auto result = co_await client->call<ExistingStreamCreateMethod>(
            TestRequest{2}, elio::test::scaled_ms(100));
        p.set_value(static_cast<int>(result.error()));
    });

    auto second_status = second_done.wait_for(elio::test::scaled_sec(5));
    REQUIRE(second_status == std::future_status::ready);
    REQUIRE(second_done.get() == static_cast<int>(rpc_error::timeout));
    REQUIRE(state->cancellable_write_calls.load(std::memory_order_acquire) == 1);
    REQUIRE(state->writev_calls.load(std::memory_order_acquire) == 0);
    REQUIRE_FALSE(state->shutdown.load(std::memory_order_acquire));

    client->close();

    auto first_status = first_done.wait_for(elio::test::scaled_sec(5));
    REQUIRE(first_status == std::future_status::ready);

    REQUIRE(sched.shutdown(elio::test::scaled_sec(5)));
}

TEST_CASE("rpc call failed send closes before queued senders run",
          "[rpc][timeout][cancel]") {
    using namespace elio::runtime;
    namespace coro = elio::coro;

    auto state = std::make_shared<stalled_send_stream_state>();
    auto client = rpc_client<stalled_send_stream>::create(
        stalled_send_stream{state});

    scheduler sched(2);
    sched.start();

    std::promise<int> first_done_promise;
    auto first_done = first_done_promise.get_future();
    sched.go([client, p = std::move(first_done_promise)]() mutable -> coro::task<void> {
        auto result = co_await client->call<ExistingStreamCreateMethod>(
            TestRequest{1}, elio::test::scaled_ms(200));
        p.set_value(static_cast<int>(result.error()));
    });

    for (int i = 0;
         i < 2000 && !state->first_write_started.load(std::memory_order_acquire);
         ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    REQUIRE(state->first_write_started.load(std::memory_order_acquire));

    std::promise<int> second_done_promise;
    auto second_done = second_done_promise.get_future();
    sched.go([client, p = std::move(second_done_promise)]() mutable -> coro::task<void> {
        auto result = co_await client->call<ExistingStreamCreateMethod>(
            TestRequest{2}, elio::test::scaled_sec(30));
        p.set_value(static_cast<int>(result.error()));
    });

    auto first_status = first_done.wait_for(elio::test::scaled_sec(5));
    REQUIRE(first_status == std::future_status::ready);
    REQUIRE(first_done.get() == static_cast<int>(rpc_error::timeout));

    auto second_status = second_done.wait_for(elio::test::scaled_sec(5));
    REQUIRE(second_status == std::future_status::ready);
    REQUIRE(second_done.get() == static_cast<int>(rpc_error::connection_closed));

    REQUIRE(state->cancellable_write_calls.load(std::memory_order_acquire) == 1);
    REQUIRE(state->writev_calls.load(std::memory_order_acquire) == 0);
    REQUIRE(state->shutdown.load(std::memory_order_acquire));

    REQUIRE(sched.shutdown(elio::test::scaled_sec(5)));
}

TEST_CASE("rpc call observes disconnect after reservation before writing",
          "[rpc][timeout][cancel]") {
    using namespace elio::runtime;
    namespace coro = elio::coro;

    auto state = std::make_shared<disconnect_after_first_valid_stream_state>();
    auto client = rpc_client<disconnect_after_first_valid_stream>::create(
        disconnect_after_first_valid_stream{state});
    REQUIRE(state->is_valid_calls.load(std::memory_order_acquire) == 0);

    scheduler sched(2);
    sched.start();

    std::promise<int> done_promise;
    auto done = done_promise.get_future();
    sched.go([client, p = std::move(done_promise)]() mutable -> coro::task<void> {
        auto result = co_await client->call<ExistingStreamCreateMethod>(
            TestRequest{3}, elio::test::scaled_ms(200));
        p.set_value(static_cast<int>(result.error()));
    });

    auto status = done.wait_for(elio::test::scaled_sec(5));
    REQUIRE(status == std::future_status::ready);
    REQUIRE(done.get() == static_cast<int>(rpc_error::connection_closed));

    REQUIRE(state->is_valid_calls.load(std::memory_order_acquire) >= 2);
    REQUIRE(state->cancellable_write_calls.load(std::memory_order_acquire) == 0);
    REQUIRE(state->writev_calls.load(std::memory_order_acquire) == 0);

    client->close();
    REQUIRE(sched.shutdown(elio::test::scaled_sec(5)));
}

TEST_CASE("rpc ping succeeds on pong frame",
          "[rpc][ping][protocol]") {
    using namespace elio::net;
    using namespace elio::runtime;
    namespace coro = elio::coro;

    auto listener_opt = tcp_listener::bind(ipv6_address("::1", 0));
    REQUIRE(listener_opt.has_value());
    uint16_t port = listener_opt->local_address().port();

    scheduler sched(3);
    sched.start();

    std::promise<bool> server_replied_promise;
    auto server_replied = server_replied_promise.get_future();

    sched.go([&, &lst = *listener_opt,
              p = std::move(server_replied_promise)]() mutable
        -> coro::task<void> {
        auto stream = co_await lst.accept();
        if (!stream) {
            p.set_value(false);
            co_return;
        }

        auto frame = co_await read_frame(*stream);
        if (!frame || frame->first.type != message_type::ping) {
            p.set_value(false);
            co_return;
        }

        auto pong = build_pong(frame->first.request_id);
        buffer_writer empty;
        bool sent = co_await write_frame(*stream, pong, empty);
        p.set_value(sent);

        stream->shutdown_socket();
        co_await stream->close();
    });

    std::promise<std::pair<bool, bool>> ping_done_promise;
    auto ping_done = ping_done_promise.get_future();

    sched.go([&, port, p = std::move(ping_done_promise)]() mutable
        -> coro::task<void> {
        auto client_opt = co_await tcp_rpc_client::connect("::1", port);
        if (!client_opt) {
            p.set_value({false, false});
            co_return;
        }

        auto client = *client_opt;
        bool ping_ok = co_await client->ping(elio::test::scaled_sec(30));
        client->close();
        p.set_value({true, ping_ok});
    });

    auto reply_status = server_replied.wait_for(std::chrono::seconds(60));
    REQUIRE(reply_status == std::future_status::ready);
    REQUIRE(server_replied.get());

    auto ping_status = ping_done.wait_for(elio::test::scaled_sec(5));
    REQUIRE(ping_status == std::future_status::ready);
    auto [connected, ping_ok] = ping_done.get();
    REQUIRE(connected);
    REQUIRE(ping_ok);

    REQUIRE(sched.shutdown(elio::test::scaled_sec(5)));
}

TEST_CASE("rpc ping fails when connection closes before pong",
          "[rpc][ping][connection]") {
    using namespace elio::net;
    using namespace elio::runtime;
    namespace coro = elio::coro;

    auto listener_opt = tcp_listener::bind(ipv6_address("::1", 0));
    REQUIRE(listener_opt.has_value());
    uint16_t port = listener_opt->local_address().port();

    scheduler sched(3);
    sched.start();

    std::promise<bool> server_seen_ping_promise;
    auto server_seen_ping = server_seen_ping_promise.get_future();

    sched.go([&, &lst = *listener_opt,
              p = std::move(server_seen_ping_promise)]() mutable
        -> coro::task<void> {
        auto stream = co_await lst.accept();
        if (!stream) {
            p.set_value(false);
            co_return;
        }

        auto frame = co_await read_frame(*stream);
        bool saw_ping = frame && frame->first.type == message_type::ping;
        p.set_value(saw_ping);

        // Close before sending a pong. The client's receive loop should
        // complete the pending ping as connection_closed, not success.
        stream->shutdown_socket();
        co_await stream->close();
    });

    std::promise<std::pair<bool, bool>> ping_done_promise;
    auto ping_done = ping_done_promise.get_future();

    sched.go([&, port, p = std::move(ping_done_promise)]() mutable
        -> coro::task<void> {
        auto client_opt = co_await tcp_rpc_client::connect("::1", port);
        if (!client_opt) {
            p.set_value({false, false});
            co_return;
        }

        auto client = *client_opt;
        // The test waits far less than this below, so a false result proves
        // close-before-pong completion rather than timeout completion.
        bool ping_ok = co_await client->ping(elio::test::scaled_sec(30));
        client->close();
        p.set_value({true, ping_ok});
    });

    auto server_status = server_seen_ping.wait_for(std::chrono::seconds(60));
    REQUIRE(server_status == std::future_status::ready);
    REQUIRE(server_seen_ping.get());

    auto ping_status = ping_done.wait_for(elio::test::scaled_sec(5));
    REQUIRE(ping_status == std::future_status::ready);
    auto [connected, ping_ok] = ping_done.get();
    REQUIRE(connected);
    REQUIRE_FALSE(ping_ok);

    REQUIRE(sched.shutdown(elio::test::scaled_sec(5)));
}

TEST_CASE("rpc ping fails on non-pong response frame",
          "[rpc][ping][protocol]") {
    using namespace elio::net;
    using namespace elio::runtime;
    namespace coro = elio::coro;

    auto listener_opt = tcp_listener::bind(ipv6_address("::1", 0));
    REQUIRE(listener_opt.has_value());
    uint16_t port = listener_opt->local_address().port();

    scheduler sched(3);
    sched.start();

    std::promise<bool> server_replied_promise;
    auto server_replied = server_replied_promise.get_future();

    sched.go([&, &lst = *listener_opt,
              p = std::move(server_replied_promise)]() mutable
        -> coro::task<void> {
        auto stream = co_await lst.accept();
        if (!stream) {
            p.set_value(false);
            co_return;
        }

        auto frame = co_await read_frame(*stream);
        if (!frame || frame->first.type != message_type::ping) {
            p.set_value(false);
            co_return;
        }

        auto error = build_error_response(
            frame->first.request_id,
            rpc_error::invalid_message,
            "pong expected");
        bool sent = co_await write_frame(*stream, error.first, error.second);
        p.set_value(sent);

        stream->shutdown_socket();
        co_await stream->close();
    });

    std::promise<std::pair<bool, bool>> ping_done_promise;
    auto ping_done = ping_done_promise.get_future();

    sched.go([&, port, p = std::move(ping_done_promise)]() mutable
        -> coro::task<void> {
        auto client_opt = co_await tcp_rpc_client::connect("::1", port);
        if (!client_opt) {
            p.set_value({false, false});
            co_return;
        }

        auto client = *client_opt;
        bool ping_ok = co_await client->ping(elio::test::scaled_sec(30));
        client->close();
        p.set_value({true, ping_ok});
    });

    auto reply_status = server_replied.wait_for(std::chrono::seconds(60));
    REQUIRE(reply_status == std::future_status::ready);
    REQUIRE(server_replied.get());

    auto ping_status = ping_done.wait_for(elio::test::scaled_sec(5));
    REQUIRE(ping_status == std::future_status::ready);
    auto [connected, ping_ok] = ping_done.get();
    REQUIRE(connected);
    REQUIRE_FALSE(ping_ok);

    REQUIRE(sched.shutdown(elio::test::scaled_sec(5)));
}

TEST_CASE("rpc ping ignores non-pong response before pong",
          "[rpc][ping][protocol]") {
    using namespace elio::net;
    using namespace elio::runtime;
    namespace coro = elio::coro;

    auto listener_opt = tcp_listener::bind(ipv6_address("::1", 0));
    REQUIRE(listener_opt.has_value());
    uint16_t port = listener_opt->local_address().port();

    scheduler sched(3);
    sched.start();

    std::promise<bool> server_replied_promise;
    auto server_replied = server_replied_promise.get_future();

    sched.go([&, &lst = *listener_opt,
              p = std::move(server_replied_promise)]() mutable
        -> coro::task<void> {
        auto stream = co_await lst.accept();
        if (!stream) {
            p.set_value(false);
            co_return;
        }

        auto frame = co_await read_frame(*stream);
        if (!frame || frame->first.type != message_type::ping) {
            p.set_value(false);
            co_return;
        }

        TestResponse response{"not a pong"};
        auto response_frame = build_response(frame->first.request_id, response);
        bool response_sent = co_await write_frame(
            *stream, response_frame.first, response_frame.second);

        auto pong = build_pong(frame->first.request_id);
        buffer_writer empty;
        bool pong_sent = co_await write_frame(*stream, pong, empty);
        p.set_value(response_sent && pong_sent);

        stream->shutdown_socket();
        co_await stream->close();
    });

    std::promise<std::pair<bool, bool>> ping_done_promise;
    auto ping_done = ping_done_promise.get_future();

    sched.go([&, port, p = std::move(ping_done_promise)]() mutable
        -> coro::task<void> {
        auto client_opt = co_await tcp_rpc_client::connect("::1", port);
        if (!client_opt) {
            p.set_value({false, false});
            co_return;
        }

        auto client = *client_opt;
        bool ping_ok = co_await client->ping(elio::test::scaled_sec(30));
        client->close();
        p.set_value({true, ping_ok});
    });

    auto reply_status = server_replied.wait_for(std::chrono::seconds(60));
    REQUIRE(reply_status == std::future_status::ready);
    REQUIRE(server_replied.get());

    auto ping_status = ping_done.wait_for(elio::test::scaled_sec(5));
    REQUIRE(ping_status == std::future_status::ready);
    auto [connected, ping_ok] = ping_done.get();
    REQUIRE(connected);
    REQUIRE(ping_ok);

    REQUIRE(sched.shutdown(elio::test::scaled_sec(5)));
}

TEST_CASE("rpc call cancellation reaches server context token",
          "[rpc][cancel][regression]") {
    using namespace elio::net;
    using namespace elio::runtime;
    namespace coro = elio::coro;

    auto listener_opt = tcp_listener::bind(ipv6_address("::1", 0));
    REQUIRE(listener_opt.has_value());
    uint16_t port = listener_opt->local_address().port();

    auto server = std::make_shared<rpc_server<tcp_stream>>();
    std::atomic<bool> handler_started{false};
    std::atomic<bool> server_cancel_seen{false};

    server->register_method_with_context<CancelProbeMethod>(
        [&handler_started, &server_cancel_seen](
            const rpc_context& ctx,
            const CancelProbeReq& req) -> coro::task<CancelProbeResp> {
            handler_started.store(true, std::memory_order_release);

            for (int i = 0; i < 2000; ++i) {
                if (ctx.cancel_token.is_cancelled()) {
                    server_cancel_seen.store(true, std::memory_order_release);
                    co_return CancelProbeResp{-1};
                }

                auto result = co_await elio::time::sleep_for(
                    elio::test::scaled_ms(5),
                    ctx.cancel_token);
                if (result == coro::cancel_result::cancelled ||
                    ctx.cancel_token.is_cancelled()) {
                    server_cancel_seen.store(true, std::memory_order_release);
                    co_return CancelProbeResp{-1};
                }
            }

            co_return CancelProbeResp{req.value};
        });

    scheduler sched(3);
    sched.start();

    sched.go([&, &lst = *listener_opt]() -> coro::task<void> {
        auto s = co_await lst.accept();
        if (!s) co_return;
        co_await server->handle_client(std::move(*s));
    });

    coro::cancel_source cancel_source;
    std::promise<std::pair<int, bool>> done_promise;
    auto done = done_promise.get_future();

    sched.go([&, p = std::move(done_promise)]() mutable -> coro::task<void> {
        auto client_opt = co_await tcp_rpc_client::connect("::1", port);
        if (!client_opt) {
            p.set_value({static_cast<int>(rpc_error::connection_closed), false});
            co_return;
        }
        auto client = *client_opt;

        auto* current_sched = scheduler::current();
        REQUIRE(current_sched != nullptr);

        auto call = current_sched->go_joinable(
            [client, token = cancel_source.get_token()]() mutable
                -> coro::task<int> {
                auto r = co_await client->call<CancelProbeMethod>(
                    CancelProbeReq{7},
                    elio::test::scaled_sec(30),
                    std::move(token));
                co_return static_cast<int>(r.error());
            });

        for (int i = 0;
             i < 2000 && !handler_started.load(std::memory_order_acquire);
             ++i) {
            co_await elio::time::sleep_for(elio::test::scaled_ms(1));
        }
        if (!handler_started.load(std::memory_order_acquire)) {
            client->close();
            p.set_value({static_cast<int>(rpc_error::timeout), false});
            co_return;
        }

        cancel_source.cancel();
        int call_error = co_await std::move(call);

        for (int i = 0;
             i < 2000 && !server_cancel_seen.load(std::memory_order_acquire);
             ++i) {
            co_await elio::time::sleep_for(elio::test::scaled_ms(1));
        }

        bool observed = server_cancel_seen.load(std::memory_order_acquire);
        client->close();
        p.set_value({call_error, observed});
    });

    auto status = done.wait_for(std::chrono::seconds(60));
    REQUIRE(status == std::future_status::ready);
    auto [call_error, observed] = done.get();
    REQUIRE(call_error == static_cast<int>(rpc_error::cancelled));
    REQUIRE(observed);

    server->stop();
    REQUIRE(sched.shutdown(std::chrono::seconds(30)));
}

TEST_CASE("rpc session rejects duplicate active request ids",
          "[rpc][contract][request_id]") {
    using namespace elio::net;
    using namespace elio::runtime;
    namespace coro = elio::coro;

    auto listener_opt = tcp_listener::bind(ipv6_address("::1", 0));
    REQUIRE(listener_opt.has_value());
    uint16_t port = listener_opt->local_address().port();

    auto server = std::make_shared<rpc_server<tcp_stream>>();
    std::atomic<int> handler_started{0};
    std::atomic<bool> handler_cancelled{false};
    std::atomic<bool> client_sent_duplicate{false};
    std::atomic<bool> release_client{false};
    std::atomic<bool> server_done{false};

    server->register_method_with_context<CancelProbeMethod>(
        [&handler_started, &handler_cancelled](
            const rpc_context& ctx,
            const CancelProbeReq& req) -> coro::task<CancelProbeResp> {
            handler_started.fetch_add(1, std::memory_order_acq_rel);

            for (int i = 0; i < 2000; ++i) {
                if (ctx.cancel_token.is_cancelled()) {
                    handler_cancelled.store(true, std::memory_order_release);
                    co_return CancelProbeResp{-1};
                }
                auto result = co_await elio::time::sleep_for(
                    elio::test::scaled_ms(1),
                    ctx.cancel_token);
                if (result == coro::cancel_result::cancelled ||
                    ctx.cancel_token.is_cancelled()) {
                    handler_cancelled.store(true, std::memory_order_release);
                    co_return CancelProbeResp{-1};
                }
            }

            co_return CancelProbeResp{req.value};
        });

    scheduler sched(3);
    sched.start();

    sched.go([&, &lst = *listener_opt]() -> coro::task<void> {
        auto s = co_await lst.accept();
        if (!s) co_return;
        co_await server->handle_client(std::move(*s));
        server_done.store(true, std::memory_order_release);
    });

    sched.go([&, port]() -> coro::task<void> {
        auto client = co_await tcp_connect(ipv6_address("::1", port));
        if (!client) co_return;

        constexpr uint32_t duplicate_id = 77;
        auto first = build_request<CancelProbeReq>(
            duplicate_id, CancelProbeMethod::id, CancelProbeReq{1});
        (void)co_await write_frame(*client, first.first, first.second);

        for (int i = 0;
             i < 2000 && handler_started.load(std::memory_order_acquire) == 0;
             ++i) {
            co_await elio::time::sleep_for(elio::test::scaled_ms(1));
        }

        auto second = build_request<CancelProbeReq>(
            duplicate_id, CancelProbeMethod::id, CancelProbeReq{2});
        (void)co_await write_frame(*client, second.first, second.second);
        client_sent_duplicate.store(true, std::memory_order_release);

        for (int i = 0;
             i < 10000 && !release_client.load(std::memory_order_acquire);
             ++i) {
            co_await elio::time::sleep_for(elio::test::scaled_ms(1));
        }
    });

    for (int i = 0;
         i < 5000 &&
             !(handler_cancelled.load(std::memory_order_acquire) &&
               server_done.load(std::memory_order_acquire));
         ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    bool cancelled_before_stop = handler_cancelled.load(std::memory_order_acquire);
    bool server_done_before_stop = server_done.load(std::memory_order_acquire);
    release_client.store(true, std::memory_order_release);
    server->stop();
    bool drained = sched.shutdown(elio::test::scaled_sec(5));

    REQUIRE(drained);
    REQUIRE(client_sent_duplicate.load(std::memory_order_acquire));
    REQUIRE(server_done_before_stop);
    REQUIRE(handler_started.load(std::memory_order_acquire) == 1);
    REQUIRE(cancelled_before_stop);
}
