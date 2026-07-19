# RPC Framework

Elio's RPC framework provides high-performance remote procedure calls over TCP and Unix domain sockets. It features zero-copy deserialization, out-of-order call support, and per-call timeouts.

## Features

- **Zero-copy deserialization**: Binary format with direct memory access for reads; `buffer_ref` avoids owning a second source buffer on the send path, but the current writer still copies the bytes into the outgoing wire buffer
- **Out-of-order calls**: Multiple concurrent requests with response correlation by request ID
- **Per-call timeouts**: Individual timeout per RPC call
- **Nested types**: Support for complex nested structures
- **Variable-length data**: Strings, arrays, maps, and optionals
- **TCP and UDS**: Works over TCP sockets or Unix domain sockets
- **C++ templates**: No code generation needed - define schemas with C++ structs
- **Corruption detection**: Optional non-cryptographic CRC32 checksum for
  accidental transport/storage error detection
- **External binary references**: `buffer_ref` type for referencing external buffers while building messages
- **Resource cleanup**: Cleanup callbacks for releasing resources after response is sent

## Design Choices

### Why a custom wire format instead of protobuf/gRPC

The RPC framework uses a custom binary wire format to maintain zero external dependencies for the core protocol. The 19-byte fixed header enables fast parsing without schema negotiation -- the receiver always knows exactly how many bytes to read before it can dispatch a message. Optional CRC32 checksums provide fast non-cryptographic corruption detection without the overhead of a full serialization framework. This keeps the library header-only and avoids pulling in protobuf's code generator toolchain or gRPC's runtime.

### Why buffer_ref

Large payloads such as file contents or binary data can be referenced without first copying them into an application-owned `std::vector` or `std::string`. The `buffer_ref` type holds a pointer and size, referencing memory that lives elsewhere (e.g., an mmap'd file or a pre-allocated buffer). On the send path, the current serializer still copies those bytes into the outgoing wire buffer; true zero-copy iovec-tail send is planned but not currently implemented.

### Why cleanup callbacks

When a response references data that must outlive serialization and the send operation (e.g., memory-mapped files, shared buffers), cleanup callbacks ensure that the referenced data is released only after the response is fully transmitted. For `no_response` one-way requests, there is no send operation; cleanup runs after the handler result has been serialized and the unsent response payload can be discarded. Without this mechanism, the server could release the backing memory while the RPC layer is still building or sending the response.

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
    // Create listener
    auto listener = net::tcp_listener::bind(net::ipv4_address(port));
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
    // Connect to server
    auto client = co_await tcp_rpc_client::connect(host, port);
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

### External Binary References (buffer_ref)

For binary data from external sources (e.g., mmap'd files, pre-allocated buffers), use `buffer_ref`:

```cpp
struct FileDataResponse {
    std::string filename;
    buffer_ref content;  // references external buffer while the response is serialized
    
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
- A lightweight pointer+size reference to external memory
- Construction from pointer+size, `std::span`, or `iovec`
- Conversion to `span`, `iovec`, or `string_view`

**Note**: `buffer_ref` is zero-copy when deserializing into a view of the
received payload. When serializing a response or request, the current
implementation copies the referenced bytes into the outgoing wire buffer.

**Important**: The referenced data must remain valid until:
- For client: the RPC call completes
- For server: the cleanup callback is invoked

## Wire Protocol

### Frame Format

All messages use a binary wire format with little-endian byte order:

```
+----------+----------+-------+--------+------------+---------+--------+
| magic(4) | req_id(4)| type(1)| flags(1)| method(4) | len(4)  | ver(1) |
+----------+----------+-------+--------+------------+---------+--------+
| payload (len bytes)                                         |
+-------------------------------------------------------------+
| checksum(4) - optional, present if has_checksum flag set    |
+-------------------------------------------------------------+
```

- **magic** (4 bytes): `0x454C494F` ("ELIO")
- **request_id** (4 bytes): Correlation ID for matching responses
- **type** (1 byte): Message type (request=0, response=1, error=2, ping=3, pong=4, cancel=5)
- **flags** (1 byte): Message flags (`has_timeout=0x01`,
  `has_checksum=0x02`, `no_response=0x10`)
- **method_id** (4 bytes): Method being called (for requests)
- **payload_length** (4 bytes): Length of payload in bytes
- **version** (1 byte): Protocol version, currently `protocol_version` (1)
- **checksum** (4 bytes, optional): CRC32 checksum of header + payload

Total header size: 19 bytes, including the final version byte
Checksum trailer: 4 bytes (optional)

Frame headers are validated by message type before dispatch. Request frames may
use `has_timeout`, `has_checksum`, and `no_response`; all other frame types may
only use `has_checksum`. Response, error, ping, pong, and cancel frames must use
`method_id=0`. Ping, pong, and cancel frames also have no payload. A request
with `has_timeout` must include at least the 4-byte timeout prefix before the
typed request payload.

### Message Types

| Type | Value | Description |
|------|-------|-------------|
| request | 0 | RPC request from client |
| response | 1 | Successful response from server |
| error | 2 | Error response from server |
| ping | 3 | Keepalive ping |
| pong | 4 | Keepalive pong |
| cancel | 5 | Cancel pending request; cancels `rpc_context::cancel_token` for context-aware handlers |

### Serialization Format

- **Integers**: Native little-endian encoding
- **Floats/Doubles**: IEEE 754 little-endian
- **Strings**: 4-byte length prefix + UTF-8 bytes
- **Arrays**: 4-byte count prefix + elements
- **Maps**: 4-byte count prefix + key-value pairs
- **Optionals**: 1-byte has_value flag + value (if present)
- **Structs**: Fields serialized in declaration order

Typed request, response, and error payloads must be fully consumed after the
declared fields are deserialized. The RPC wire format does not reserve trailing
extension bytes inside typed payloads; future extensions require an explicit
flag or versioned negotiation before receivers may accept additional bytes.

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
    resource_exhausted = 8,
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
    if (ctx.cancel_token.is_cancelled()) {
        co_return GetUserResponse{};
    }
    
    GetUserResponse resp;
    // ... populate response
    co_return resp;
});
```

RPC call cancellation is cooperative. Cancelling the token passed to
`rpc_client::call()` completes the client call with `rpc_error::cancelled` and,
if the request frame was already sent, sends a best-effort cancel frame to the
server. Context-aware handlers can observe that request-specific cancellation
through `ctx.cancel_token` and should pass it to cancellable operations or poll
it before long-running work.

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

### Non-Cryptographic CRC32 Checksum

Enable CRC32 checksums to detect accidental corruption of RPC frame bytes:

```cpp
// Client: enable checksum for a specific call
GetUserRequest req{42};
auto [header, payload] = build_request(
    request_id, GetUser::id, req,
    5000,  // timeout in milliseconds
    true  // enable_checksum
);

// Or when building responses (server-side)
auto [header, payload] = build_response(request_id, response, true);

// Error responses with checksum
auto [header, payload] = build_error_response(
    request_id, rpc_error::internal_error, "message", true);
```

The checksum covers both header and payload. If verification fails on receive,
the frame is rejected and `read_frame()` returns `std::nullopt`.

CRC32 is not a security boundary. It does not authenticate the peer, authorize
methods, or protect against intentional tampering by an adversary who can modify
traffic. Use TLS/mTLS, an application authentication layer, or a MAC/signature
scheme when adversarial integrity or peer authentication is required.

### One-way Messages

Send messages without waiting for response:

```cpp
co_await client->send_oneway<NotifyEvent>(event);
```

One-way sends use the protocol `no_response` request flag. Servers that
understand this flag still run the handler, but they do not emit a response or
error frame for that request ID, preventing delayed one-way replies from being
matched with unrelated future calls after request-ID wrap. This guarantee
requires both peers to understand `no_response`; older or non-compliant servers
that ignore the flag may still send late replies.

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
    net::unix_address("/tmp/my_service.sock"));
uds_rpc_server server;
// ... register methods
co_await server.serve(*listener);

// Client
auto client = co_await uds_rpc_client::connect(
    "/tmp/my_service.sock");
```

### Abstract Sockets (Linux)

```cpp
// No filesystem path - automatically cleaned up
auto addr = net::unix_address::abstract("my_service");
auto listener = net::uds_listener::bind(addr);
```

## Performance Considerations

### Scatter-Gather Frame Writing

The RPC framework uses scatter-gather I/O (`writev`) plus retry handling to write complete frames efficiently. Each `writev()` call is still a single write attempt and may return a positive short write; the RPC protocol layer advances the iovec array and retries until the header, already-serialized contiguous payload buffer, and optional checksum are written. This:

- Reduces the number of syscalls when the kernel accepts the whole frame in one attempt
- Minimizes context switching under high concurrency
- Preserves full-frame delivery by handling partial scatter-gather writes

This is handled automatically by `write_frame()` - no special configuration needed.

Custom stream types used with `rpc::rpc_stream` must still provide the full RPC
stream surface: `read_exactly()`, `write_exactly()`, `writev()`, `poll_write()`,
and `is_valid()`. The exact-length helpers are used for full-frame reads and
cancellable contiguous writes; `writev()` and `poll_write()` remain required for
the scatter-gather fast path and its readiness retry loop.

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

The RPC framework uses the hash module for non-cryptographic checksums. See
[[Hash Functions]] for full documentation.

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

### Client Configuration

`rpc_client_config` controls client-side receive-loop limits:

```cpp
rpc_client_config config;
config.frame_read_timeout = std::chrono::seconds(30); // 0s = disabled
config.max_message_size = elio::rpc::max_message_size; // 16 MiB default

auto client = co_await tcp_rpc_client::connect_with_config(
    config, "127.0.0.1", 9000);
```

- **frame_read_timeout**: Inactivity deadline for each receive-loop frame read.
  The timer starts while waiting for the next frame header and continues until
  the complete response/control frame, including payload and optional checksum,
  is read. This mitigates peers that send only part of a frame and keep the
  connection open, and also closes idle/no-response connections after the
  configured interval. Use a larger value or `0s` for long-lived idle clients or
  calls whose server may be silent longer than the default.
- **max_message_size**: Maximum payload bytes accepted per frame. The default
  is the protocol-wide 16 MiB limit; reduce it when an application has smaller
  messages.

### Server Configuration

`rpc_server_config` controls the per-server operational limits:

```cpp
rpc_server_config config;
config.max_sessions = 1024;                         // 0 = unlimited
config.frame_read_timeout = std::chrono::seconds(30); // 0s = disabled
config.max_message_size = elio::rpc::max_message_size; // 16 MiB default
config.max_in_flight_requests_per_session = 64;      // 0 = unlimited
config.request_overload_policy =
    rpc_request_overload_policy::reject_request;

tcp_rpc_server server(config);
```

- **max_sessions**: Maximum concurrent sessions accepted by `serve()`.
  Connections beyond the cap are accepted and immediately closed so the
  kernel listen backlog does not fill.
- **max_in_flight_requests_per_session**: Maximum active request slots for one
  client session. `0` preserves legacy unlimited concurrency. A response-capable
  request holds its slot until its handler has produced a result and the
  response/error send path has completed or failed; a `no_response` request
  holds its slot until its handler finishes. Exposed services should choose a
  finite value that matches handler cost, response size, and downstream resource
  limits.
- **request_overload_policy**: Strategy when one session reaches its in-flight
  request cap:
  - `reject_request`: send `rpc_error::resource_exhausted` for a
    response-capable excess request when no previous overload rejection is still
    being written. Over-limit `no_response` requests are dropped because the
    protocol forbids a response/error frame for them. If the peer is not
    draining responses and an overload rejection is already in flight, further
    excess requests may also be dropped so the rejection path remains bounded;
    a waiting client call for such a dropped request completes only through its
    own timeout or connection close.
  - `close_session`: close the session immediately.
  Accepted `no_response` requests count against the cap until their handlers
  finish. Duplicate active request IDs close the session and do not consume
  capacity.
- **frame_read_timeout**: Deadline for receiving one complete frame
  (header, payload, and optional checksum). This mitigates peers that
  trickle bytes indefinitely.
- **max_message_size**: Maximum payload bytes accepted per frame. The
  default is the protocol-wide 16 MiB limit defined by `max_message_size`
  in `rpc_buffer.hpp`; reduce it when an application has smaller messages.
- Server pong writes are also bounded: each session has at most one in-flight
  pong write. If a peer sends more pings while that pong is still being written,
  later pings may not receive a pong; clients should rely on their own ping
  timeout and connection policy.

Each server session owns its accepted request handlers, pong writes, and overload
responses in one structured task scope. Disconnect, timeout, duplicate request
ID, `close_session` overload handling, cancellation of the task awaiting
`handle_client()`, and `rpc_server::stop()` close that scope:

- active handlers receive cancellation through both
  `rpc_context::cancel_token` and `coro::this_coro::cancel_token()`;
- pending frame reads are woken through stream shutdown, while pending frame
  writes and waits for the session response lock are cancelled;
- `handle_client()` and the server's session slot remain active until every
  accepted child task has left the scope.

Cancellation is cooperative. Handler code should pass one of the supplied
tokens to every wait that must stop during disconnect. A handler that ignores
both tokens, blocks a worker thread, or deliberately continues after observing
cancellation can delay session teardown and slot release. This lifetime rule
does not add another concurrency limit and does not change
`max_in_flight_requests_per_session` admission or overload behavior.

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
