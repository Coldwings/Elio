# RPC Framework

Elio's RPC framework provides high-performance remote procedure calls over TCP and Unix domain sockets. It features zero-copy serialization, out-of-order call support, and per-call timeouts.

## Features

- **Zero-copy serialization**: Binary format with direct memory access where possible
- **Out-of-order calls**: Multiple concurrent requests with response correlation by request ID
- **Per-call timeouts**: Individual timeout per RPC call
- **Nested types**: Support for complex nested structures
- **Variable-length data**: Strings, arrays, maps, and optionals
- **TCP and UDS**: Works over TCP sockets or Unix domain sockets
- **C++ templates**: No code generation needed - define schemas with C++ structs
- **Message integrity**: Optional CRC32 checksum for data verification
- **Zero-copy binary fields**: `buffer_ref` type for referencing external buffers without copying
- **Resource cleanup**: Cleanup callbacks for releasing resources after response is sent

## Quick Start

### Include the Header

```cpp
#include <elio/rpc/rpc.hpp>
```

### Define Message Types

```cpp
// Request message
struct GetUserRequest {
    int32_t user_id;
    ELIO_RPC_FIELDS(GetUserRequest, user_id)
};

// Response message
struct GetUserResponse {
    std::string name;
    int32_t age;
    std::vector<std::string> roles;
    ELIO_RPC_FIELDS(GetUserResponse, name, age, roles)
};
```

### Define RPC Methods

```cpp
// Method ID 1: GetUser
using GetUser = ELIO_RPC_METHOD(1, GetUserRequest, GetUserResponse);

// Method ID 2: CreateUser
using CreateUser = ELIO_RPC_METHOD(2, CreateUserRequest, CreateUserResponse);
```

### Server Implementation

```cpp
using namespace elio;
using namespace elio::rpc;

coro::task<void> run_server(uint16_t port) {
    auto& ctx = io::default_io_context();
    
    // Create listener
    auto listener = net::tcp_listener::bind(net::ipv4_address(port), ctx);
    if (!listener) {
        ELIO_LOG_ERROR("Failed to bind");
        co_return;
    }
    
    // Create server and register handlers
    tcp_rpc_server server;
    
    server.register_method<GetUser>([](const GetUserRequest& req) 
        -> coro::task<GetUserResponse> {
        GetUserResponse resp;
        resp.name = "John Doe";
        resp.age = 30;
        resp.roles = {"admin", "user"};
        co_return resp;
    });
    
    // Start serving
    co_await server.serve(*listener);
}
```

### Client Implementation

```cpp
coro::task<void> run_client(const char* host, uint16_t port) {
    auto& ctx = io::default_io_context();
    
    // Connect to server
    auto client = co_await tcp_rpc_client::connect(ctx, host, port);
    if (!client) {
        ELIO_LOG_ERROR("Failed to connect");
        co_return;
    }
    
    // Make RPC call with 5 second timeout
    GetUserRequest req{42};
    auto result = co_await (*client)->call<GetUser>(req, std::chrono::seconds(5));
    
    if (result.ok()) {
        std::cout << "Name: " << result->name << std::endl;
        std::cout << "Age: " << result->age << std::endl;
    } else {
        std::cerr << "RPC failed: " << result.error_message() << std::endl;
    }
}
```

## Supported Types

### Primitive Types

All arithmetic types and enums are directly serializable:

```cpp
struct PrimitiveExample {
    int8_t a;
    int16_t b;
    int32_t c;
    int64_t d;
    uint8_t e;
    uint16_t f;
    uint32_t g;
    uint64_t h;
    float i;
    double j;
    bool k;
    MyEnum l;  // enums work too
    
    ELIO_RPC_FIELDS(PrimitiveExample, a, b, c, d, e, f, g, h, i, j, k, l)
};
```

### Strings

Both `std::string` and `std::string_view` are supported:

```cpp
struct StringExample {
    std::string name;
    std::string description;
    
    ELIO_RPC_FIELDS(StringExample, name, description)
};
```

### Arrays and Vectors

```cpp
struct ArrayExample {
    std::vector<int32_t> ids;
    std::vector<std::string> names;
    std::array<float, 3> position;  // fixed-size arrays
    
    ELIO_RPC_FIELDS(ArrayExample, ids, names, position)
};
```

### Maps

Both `std::map` and `std::unordered_map` are supported:

```cpp
struct MapExample {
    std::map<std::string, int32_t> scores;
    std::unordered_map<int32_t, std::string> id_to_name;
    
    ELIO_RPC_FIELDS(MapExample, scores, id_to_name)
};
```

### Optionals

```cpp
struct OptionalExample {
    std::optional<std::string> nickname;
    std::optional<int32_t> age;
    
    ELIO_RPC_FIELDS(OptionalExample, nickname, age)
};
```

### Nested Structures

```cpp
struct Address {
    std::string street;
    std::string city;
    std::string country;
    
    ELIO_RPC_FIELDS(Address, street, city, country)
};

struct Person {
    std::string name;
    Address address;                    // nested struct
    std::vector<Address> past_addresses; // array of nested structs
    std::map<std::string, Address> locations; // map with nested values
    
    ELIO_RPC_FIELDS(Person, name, address, past_addresses, locations)
};
```

### Byte Arrays (Blobs)

```cpp
struct BlobExample {
    std::vector<uint8_t> data;  // serialized as length-prefixed blob
    
    ELIO_RPC_FIELDS(BlobExample, data)
};
```

### Zero-Copy Binary References (buffer_ref)

For zero-copy handling of binary data from external sources (e.g., mmap'd files, pre-allocated buffers), use `buffer_ref`:

```cpp
struct FileDataResponse {
    std::string filename;
    buffer_ref content;  // references external buffer without copying
    
    ELIO_RPC_FIELDS(FileDataResponse, filename, content)
};

// Server handler with external buffer
server.register_method_with_cleanup<GetFileData>(
    [](const GetFileDataRequest& req) 
        -> coro::task<std::pair<FileDataResponse, cleanup_callback_t>> {
        
        // Map file into memory
        void* mapped = mmap(nullptr, file_size, PROT_READ, MAP_PRIVATE, fd, 0);
        
        FileDataResponse resp;
        resp.filename = req.path;
        resp.content = buffer_ref(mapped, file_size);
        
        // Cleanup callback runs after response is sent
        auto cleanup = [mapped, file_size]() {
            munmap(mapped, file_size);
        };
        
        co_return std::make_pair(resp, cleanup);
    });
```

`buffer_ref` provides:
- Zero-copy serialization of external memory
- Construction from pointer+size, `std::span`, or `iovec`
- Conversion to `span`, `iovec`, or `string_view`

**Important**: The referenced data must remain valid until:
- For client: the RPC call completes
- For server: the cleanup callback is invoked

## Wire Protocol

### Frame Format

All messages use a binary wire format with little-endian byte order:

```
+----------+----------+-------+--------+------------+---------+
| magic(4) | req_id(4)| type(1)| flags(1)| method(4) | len(4)  |
+----------+----------+-------+--------+------------+---------+
| payload (len bytes)                                         |
+-------------------------------------------------------------+
| checksum(4) - optional, present if has_checksum flag set    |
+-------------------------------------------------------------+
```

- **magic** (4 bytes): `0x454C494F` ("ELIO")
- **request_id** (4 bytes): Correlation ID for matching responses
- **type** (1 byte): Message type (request=0, response=1, error=2, ping=3, pong=4, cancel=5)
- **flags** (1 byte): Message flags (has_timeout=0x01, has_checksum=0x02)
- **method_id** (4 bytes): Method being called (for requests)
- **payload_length** (4 bytes): Length of payload in bytes
- **checksum** (4 bytes, optional): CRC32 checksum of header + payload

Total header size: 18 bytes
Checksum trailer: 4 bytes (optional)

### Message Types

| Type | Value | Description |
|------|-------|-------------|
| request | 0 | RPC request from client |
| response | 1 | Successful response from server |
| error | 2 | Error response from server |
| ping | 3 | Keepalive ping |
| pong | 4 | Keepalive pong |
| cancel | 5 | Cancel pending request |

### Serialization Format

- **Integers**: Native little-endian encoding
- **Floats/Doubles**: IEEE 754 little-endian
- **Strings**: 4-byte length prefix + UTF-8 bytes
- **Arrays**: 4-byte count prefix + elements
- **Maps**: 4-byte count prefix + key-value pairs
- **Optionals**: 1-byte has_value flag + value (if present)
- **Structs**: Fields serialized in declaration order

## Error Handling

### Error Codes

```cpp
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
```

### Using rpc_result

```cpp
auto result = co_await client->call<MyMethod>(request);

// Check success
if (result.ok()) {
    // Access value
    auto& response = result.value();
    // Or use operator->
    std::cout << result->name << std::endl;
}

// Check for specific error
if (result.error() == rpc_error::timeout) {
    // Handle timeout
}

// Get error message
std::cerr << result.error_message() << std::endl;

// Use value_or for default
auto name = result.value_or(MyResponse{}).name;
```

## Advanced Usage

### Handler with Context

```cpp
server.register_method_with_context<GetUser>(
    [](const rpc_context& ctx, const GetUserRequest& req) 
        -> coro::task<GetUserResponse> {
    // Access request context
    ELIO_LOG_DEBUG("Request ID: {}", ctx.request_id);
    if (ctx.has_timeout()) {
        ELIO_LOG_DEBUG("Timeout: {}ms", *ctx.timeout_ms);
    }
    
    GetUserResponse resp;
    // ... populate response
    co_return resp;
});
```

### Synchronous Handlers

For simple handlers that don't need async operations:

```cpp
server.register_sync_method<GetVersion>(
    [](const GetVersionRequest& req) -> GetVersionResponse {
    GetVersionResponse resp;
    resp.version = "1.0.0";
    return resp;
});
```

### Handlers with Cleanup Callbacks

When your response references external resources that must be released after the response is sent, use cleanup callbacks:

```cpp
// Handler returns std::pair<Response, cleanup_callback_t>
server.register_method_with_cleanup<ReadFile>(
    [](const ReadFileRequest& req) 
        -> coro::task<std::pair<ReadFileResponse, cleanup_callback_t>> {
        
        // Acquire resource
        int fd = open(req.path.c_str(), O_RDONLY);
        void* data = mmap(nullptr, size, PROT_READ, MAP_PRIVATE, fd, 0);
        
        ReadFileResponse resp;
        resp.content = buffer_ref(data, size);
        
        // Cleanup runs AFTER response is fully sent
        auto cleanup = [data, size, fd]() {
            munmap(data, size);
            close(fd);
        };
        
        co_return std::make_pair(resp, std::move(cleanup));
    });

// With context access
server.register_method_with_context_and_cleanup<ReadFile>(
    [](const rpc_context& ctx, const ReadFileRequest& req) 
        -> coro::task<std::pair<ReadFileResponse, cleanup_callback_t>> {
        // ... same pattern with ctx available
    });
```

### Message Integrity with CRC32 Checksum

Enable CRC32 checksums for message integrity verification:

```cpp
// Client: enable checksum for a specific call
GetUserRequest req{42};
auto [header, payload] = build_request(
    request_id, GetUser::id, req,
    std::chrono::milliseconds(5000),  // timeout
    true  // enable_checksum
);

// Or when building responses (server-side)
auto [header, payload] = build_response(request_id, response, true);

// Error responses with checksum
auto [header, payload] = build_error_response(
    request_id, rpc_error::internal_error, "message", true);
```

The checksum covers both header and payload. If verification fails on receive, the frame is rejected and `read_frame()` returns `std::nullopt`.

### One-way Messages

Send messages without waiting for response:

```cpp
co_await client->send_oneway<NotifyEvent>(event);
```

### Keepalive Ping

```cpp
bool alive = co_await client->ping(std::chrono::seconds(5));
if (!alive) {
    // Connection may be dead
}
```

### Unix Domain Sockets

```cpp
// Server
auto listener = net::uds_listener::bind(
    net::unix_address("/tmp/my_service.sock"), ctx);
uds_rpc_server server;
// ... register methods
co_await server.serve(*listener);

// Client
auto client = co_await uds_rpc_client::connect(
    ctx, "/tmp/my_service.sock");
```

### Abstract Sockets (Linux)

```cpp
// No filesystem path - automatically cleaned up
auto addr = net::unix_address::abstract("my_service");
auto listener = net::uds_listener::bind(addr, ctx);
```

## Performance Considerations

### Zero-Copy Deserialization

For read-only access, use `buffer_view` directly:

```cpp
// In buffer_view, strings are returned as string_view (no copy)
std::string_view name = view.read_string();

// Blobs are returned as span (no copy)
std::span<const uint8_t> data = view.read_blob();
```

### Discontinuous Buffers

For scatter-gather I/O, use `iovec_buffer`:

```cpp
iovec_buffer iov;
iov.add(header_data, header_size);
iov.add(payload_data, payload_size);

// Use with writev/sendmsg
struct msghdr msg = {};
msg.msg_iov = iov.iovecs();
msg.msg_iovlen = iov.count();
```

### CRC32 Checksum Utilities

The RPC framework uses the hash module for checksums. See [[Hash Functions]] for full documentation.

```cpp
#include <elio/hash/crc32.hpp>

// Single contiguous buffer
uint32_t checksum = elio::hash::crc32(data, length);

// From span
uint32_t checksum = elio::hash::crc32(std::span<const uint8_t>(data));

// Across multiple iovec buffers (scatter-gather)
struct iovec iov[2] = {...};
uint32_t checksum = elio::hash::crc32_iovec(iov, 2);

// Also available via elio::rpc namespace for convenience
uint32_t checksum = elio::rpc::crc32(data, length);
```

### Message Size Limits

Default maximum message size is 16MB. This is defined by `max_message_size` in `rpc_buffer.hpp`.

## Thread Safety

- **rpc_client**: Thread-safe for concurrent calls
- **rpc_server**: Thread-safe for serving multiple clients
- **Handlers**: Called from scheduler worker threads
- **Serialization**: Buffer types are not thread-safe (use separate instances per thread)

## Best Practices

1. **Keep messages small**: Large messages increase latency and memory usage
2. **Use appropriate timeouts**: Set reasonable timeouts for your use case
3. **Handle errors**: Always check `rpc_result::ok()` before accessing values
4. **Use unique method IDs**: Each method should have a unique ID across your service
5. **Version your protocol**: Consider adding version fields to messages for compatibility
