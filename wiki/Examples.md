# Examples

This page provides complete, runnable examples demonstrating Elio's features.

## Hello World

The simplest Elio program using `ELIO_ASYNC_MAIN`:

```cpp
#include <elio/elio.hpp>
#include <iostream>

using namespace elio;

coro::task<std::string> get_greeting() {
    co_return "Hello from Elio!";
}

coro::task<int> async_main(int argc, char* argv[]) {
    std::string greeting = co_await get_greeting();
    std::cout << greeting << std::endl;
    co_return 0;
}

ELIO_ASYNC_MAIN(async_main)
```

## Chained Coroutines

Demonstrating coroutine composition:

```cpp
#include <elio/elio.hpp>

using namespace elio;

coro::task<int> step1() {
    ELIO_LOG_INFO("Step 1");
    co_return 10;
}

coro::task<int> step2(int input) {
    ELIO_LOG_INFO("Step 2: input={}", input);
    co_return input * 2;
}

coro::task<int> step3(int input) {
    ELIO_LOG_INFO("Step 3: input={}", input);
    co_return input + 5;
}

coro::task<int> async_main(int argc, char* argv[]) {
    int a = co_await step1();        // 10
    int b = co_await step2(a);       // 20
    int c = co_await step3(b);       // 25

    ELIO_LOG_INFO("Final result: {}", c);
    co_return 0;
}

ELIO_ASYNC_MAIN(async_main)
```

## TCP Echo Server

A concurrent TCP server that echoes data back to clients, using signalfd for graceful shutdown:

```cpp
#include <elio/elio.hpp>
#include <atomic>

using namespace elio;
using namespace elio::signal;

std::atomic<bool> g_running{true};
std::atomic<int> g_listener_fd{-1};

// Signal handler coroutine - waits for SIGINT/SIGTERM
coro::task<void> signal_handler_task() {
    signal_set sigs{SIGINT, SIGTERM};
    signal_fd sigfd(sigs);

    auto info = co_await sigfd.wait();
    if (info) {
        ELIO_LOG_INFO("Received signal: {}", info->full_name());
    }

    g_running = false;

    // Close listener to interrupt pending accept
    int fd = g_listener_fd.exchange(-1);
    if (fd >= 0) ::close(fd);
}

coro::task<void> handle_client(net::tcp_stream stream, int id) {
    ELIO_LOG_INFO("[Client {}] Connected", id);

    char buffer[1024];
    while (g_running) {
        auto result = co_await stream.read(buffer, sizeof(buffer));
        if (result.result <= 0) break;

        co_await stream.write(buffer, result.result);
    }

    ELIO_LOG_INFO("[Client {}] Disconnected", id);
}

coro::task<int> async_main(int argc, char* argv[]) {
    // Block signals before they can be delivered to worker threads
    signal_set sigs{SIGINT, SIGTERM};
    sigs.block_all_threads();

    // Spawn signal handler
    signal_handler_task().go();

    auto listener = net::tcp_listener::bind(net::ipv4_address(8080));
    if (!listener) {
        ELIO_LOG_ERROR("Failed to bind");
        co_return 1;
    }

    g_listener_fd.store(listener->fd());
    ELIO_LOG_INFO("Server listening on port 8080");

    int client_id = 0;
    while (g_running) {
        auto stream = co_await listener->accept();
        if (!stream) continue;

        handle_client(std::move(*stream), ++client_id).go();
    }
    co_return 0;
}

ELIO_ASYNC_MAIN(async_main)
```

## UDS Echo Server

A concurrent Unix Domain Socket server that echoes data back to clients:

```cpp
#include <elio/elio.hpp>
#include <atomic>

using namespace elio;
using namespace elio::signal;

std::atomic<bool> g_running{true};
std::atomic<int> g_listener_fd{-1};

// Signal handler coroutine
coro::task<void> signal_handler_task() {
    signal_set sigs{SIGINT, SIGTERM};
    signal_fd sigfd(sigs);

    auto info = co_await sigfd.wait();
    if (info) {
        ELIO_LOG_INFO("Received signal: {}", info->full_name());
    }

    g_running = false;
    int fd = g_listener_fd.exchange(-1);
    if (fd >= 0) ::close(fd);
}

coro::task<void> handle_client(net::uds_stream stream, int id) {
    ELIO_LOG_INFO("[Client {}] Connected", id);

    char buffer[1024];
    while (g_running) {
        auto result = co_await stream.read(buffer, sizeof(buffer));
        if (result.result <= 0) break;

        co_await stream.write(buffer, result.result);
    }

    ELIO_LOG_INFO("[Client {}] Disconnected", id);
}

coro::task<int> async_main(int argc, char* argv[]) {
    // Block signals before they can be delivered to worker threads
    signal_set sigs{SIGINT, SIGTERM};
    sigs.block_all_threads();

    // Spawn signal handler
    signal_handler_task().go();

    // Use filesystem socket
    net::unix_address addr("/tmp/echo.sock");
    // Or use abstract socket (Linux-specific):
    // auto addr = net::unix_address::abstract("echo_server");

    net::uds_options opts;
    opts.unlink_on_bind = true;

    auto listener = net::uds_listener::bind(addr, opts);
    if (!listener) {
        ELIO_LOG_ERROR("Failed to bind to {}", addr.to_string());
        co_return 1;
    }

    g_listener_fd.store(listener->fd());
    ELIO_LOG_INFO("Server listening on {}", addr.to_string());

    int client_id = 0;
    while (g_running) {
        auto stream = co_await listener->accept();
        if (!stream) continue;

        handle_client(std::move(*stream), ++client_id).go();
    }
    co_return 0;
}

ELIO_ASYNC_MAIN(async_main)
```

## UDS Client

A Unix Domain Socket client that connects to a UDS server:

```cpp
#include <elio/elio.hpp>

using namespace elio;

coro::task<void> client_main(const net::unix_address& addr) {
    ELIO_LOG_INFO("Connecting to {}...", addr.to_string());

    auto stream = co_await net::uds_connect(addr);
    if (!stream) {
        ELIO_LOG_ERROR("Connect failed: {}", strerror(errno));
        co_return;
    }

    ELIO_LOG_INFO("Connected!");

    // Send message
    const char* msg = "Hello via Unix Domain Socket!";
    co_await stream->write(msg, strlen(msg));

    // Receive echo
    char buffer[1024];
    auto result = co_await stream->read(buffer, sizeof(buffer) - 1);
    if (result.result > 0) {
        buffer[result.result] = '\0';
        ELIO_LOG_INFO("Received: {}", buffer);
    }
}

coro::task<int> async_main(int argc, char* argv[]) {
    // Match server's socket path
    net::unix_address addr("/tmp/echo.sock");
    // Or abstract socket:
    // auto addr = net::unix_address::abstract("echo_server");

    co_await client_main(addr);
    co_return 0;
}

ELIO_ASYNC_MAIN(async_main)
```

## HTTP Client

Making HTTP requests with various methods:

```cpp
#include <elio/elio.hpp>
#include <elio/http/http.hpp>
#include <elio/tls/tls.hpp>

using namespace elio;
using namespace elio::http;

coro::task<int> async_main(int argc, char* argv[]) {
    client_config config;
    config.user_agent = "elio-example/1.0";
    config.follow_redirects = true;

    client c(config);

    // GET request
    ELIO_LOG_INFO("=== GET ===");
    auto get_resp = co_await c.get("https://httpbin.org/get");
    if (get_resp) {
        ELIO_LOG_INFO("Status: {}", get_resp->status_code());
    }

    // POST JSON
    ELIO_LOG_INFO("=== POST JSON ===");
    auto post_resp = co_await c.post(
        "https://httpbin.org/post",
        R"({"name": "Elio"})",
        mime::application_json
    );
    if (post_resp) {
        ELIO_LOG_INFO("Status: {}", post_resp->status_code());
    }

    // POST Form
    ELIO_LOG_INFO("=== POST Form ===");
    auto form_resp = co_await c.post(
        "https://httpbin.org/post",
        "key=value&foo=bar",
        mime::application_form_urlencoded
    );
    if (form_resp) {
        ELIO_LOG_INFO("Status: {}", form_resp->status_code());
    }

    co_return 0;
}

ELIO_ASYNC_MAIN(async_main)
```

## HTTP/2 Client

Making HTTP/2 requests with multiplexed streams:

```cpp
#include <elio/elio.hpp>
#include <elio/http/http2.hpp>

using namespace elio;
using namespace elio::http;

coro::task<int> async_main(int argc, char* argv[]) {
    h2_client_config config;
    config.user_agent = "elio-example/1.0";
    config.max_concurrent_streams = 100;

    h2_client client(config);

    // GET request (HTTP/2 requires HTTPS)
    ELIO_LOG_INFO("=== HTTP/2 GET ===");
    auto get_resp = co_await client.get("https://nghttp2.org/");
    if (get_resp) {
        ELIO_LOG_INFO("Status: {}", static_cast<int>(get_resp->get_status()));
        ELIO_LOG_INFO("Body size: {} bytes", get_resp->body().size());
    }

    // POST JSON
    ELIO_LOG_INFO("=== HTTP/2 POST JSON ===");
    auto post_resp = co_await client.post(
        "https://httpbin.org/post",
        R"({"name": "Elio", "protocol": "h2"})",
        mime::application_json
    );
    if (post_resp) {
        ELIO_LOG_INFO("Status: {}", static_cast<int>(post_resp->get_status()));
    }

    // Multiple requests on same connection (HTTP/2 multiplexing)
    ELIO_LOG_INFO("=== HTTP/2 Multiplexing ===");
    for (int i = 0; i < 5; ++i) {
        auto resp = co_await client.get("https://nghttp2.org/");
        if (resp) {
            ELIO_LOG_INFO("Request {}: {} bytes", i + 1, resp->body().size());
        }
    }
    // All requests above reused the same underlying connection

    co_return 0;
}

ELIO_ASYNC_MAIN(async_main)
```

## HTTP Server

A REST API server using the router and `elio::serve` for graceful shutdown:

```cpp
#include <elio/elio.hpp>
#include <elio/http/http.hpp>

using namespace elio;
using namespace elio::http;

response hello_handler(context& ctx) {
    return response(status::ok, "<h1>Welcome to Elio!</h1>", mime::text_html);
}

coro::task<response> status_handler(context& ctx) {
    co_return response(status::ok,
        R"({"status": "ok", "version": "1.0"})",
        mime::application_json);
}

coro::task<response> echo_handler(context& ctx) {
    auto& req = ctx.req();
    response resp(status::ok);
    resp.set_header("Content-Type", req.content_type());
    resp.set_body(req.body());
    co_return resp;
}

coro::task<int> async_main(int argc, char* argv[]) {
    router r;
    r.get("/", hello_handler);
    r.get("/api/status", status_handler);
    r.post("/api/echo", echo_handler);

    server srv(r);
    co_await elio::serve(srv, srv.listen(net::ipv4_address(8080)));

    co_return 0;
}

ELIO_ASYNC_MAIN(async_main)
```

## Parallel Tasks

Running multiple tasks concurrently using `spawn()` and `go()`:

```cpp
#include <elio/elio.hpp>
#include <vector>

using namespace elio;

coro::task<int> compute(int id, int value) {
    ELIO_LOG_INFO("Task {} computing...", id);
    co_return value * value;
}

coro::task<int> async_main(int argc, char* argv[]) {
    // Spawn tasks and collect join handles to await results
    std::vector<coro::join_handle<int>> handles;
    for (int i = 0; i < 10; ++i) {
        handles.push_back(compute(i, i + 1).spawn());
    }

    // Await all results
    int total = 0;
    for (auto& h : handles) {
        total += co_await h;
    }
    ELIO_LOG_INFO("Sum of squares: {}", total);

    // Fire-and-forget tasks (no result collection)
    for (int i = 0; i < 5; ++i) {
        compute(i, i).go();
    }

    co_await time::sleep_for(std::chrono::milliseconds(100));
    co_return 0;
}

ELIO_ASYNC_MAIN(async_main)
```

## Thread Affinity

Binding vthreads to specific worker threads:

```cpp
#include <elio/elio.hpp>
#include <iostream>

using namespace elio;

// Task that stays pinned to a specific worker
coro::task<void> pinned_worker(size_t target_worker) {
    // Bind to target worker and migrate there
    co_await set_affinity(target_worker);

    std::cout << "Running on worker " << current_worker_id() << std::endl;

    // Do work - will not be stolen by other workers
    for (int i = 0; i < 5; ++i) {
        co_await time::yield();
        // Still on the same worker
    }

    // Clear affinity to allow migration
    co_await clear_affinity();
}

// Task that pins to its current worker
coro::task<void> stay_here() {
    co_await bind_to_current_worker();

    // Will remain on this worker for rest of execution
    std::cout << "Pinned to worker " << current_worker_id() << std::endl;
}

coro::task<int> async_main(int argc, char* argv[]) {
    auto* sched = runtime::scheduler::current();

    // Spawn tasks with different affinities
    for (size_t i = 0; i < sched->num_threads(); ++i) {
        pinned_worker(i).go();
    }

    co_await time::sleep_for(std::chrono::milliseconds(100));
    co_return 0;
}

ELIO_ASYNC_MAIN(async_main)
```

## Timer Example

Using timers for delays:

```cpp
#include <elio/elio.hpp>

using namespace elio;

coro::task<int> async_main(int argc, char* argv[]) {
    ELIO_LOG_INFO("Starting...");

    co_await time::sleep_for(std::chrono::seconds(1));
    ELIO_LOG_INFO("1 second passed");

    co_await time::sleep_for(std::chrono::milliseconds(500));
    ELIO_LOG_INFO("500ms more passed");

    co_return 0;
}

ELIO_ASYNC_MAIN(async_main)
```

## Exception Handling

Handling errors in coroutines:

```cpp
#include <elio/elio.hpp>
#include <stdexcept>

using namespace elio;

coro::task<int> may_fail(bool should_fail) {
    if (should_fail) {
        throw std::runtime_error("Something went wrong!");
    }
    co_return 42;
}

coro::task<int> async_main(int argc, char* argv[]) {
    try {
        int result = co_await may_fail(false);
        ELIO_LOG_INFO("Success: {}", result);

        int fail = co_await may_fail(true);  // Throws
        ELIO_LOG_INFO("Never reached: {}", fail);
    } catch (const std::exception& e) {
        ELIO_LOG_ERROR("Caught: {}", e.what());
    }
    co_return 0;
}

ELIO_ASYNC_MAIN(async_main)
```

## Synchronization

Using async mutex:

```cpp
#include <elio/elio.hpp>

using namespace elio;

sync::mutex g_mutex;
int g_counter = 0;

coro::task<void> increment(int id) {
    for (int i = 0; i < 100; ++i) {
        auto lock = co_await g_mutex.lock();
        ++g_counter;
        ELIO_LOG_DEBUG("Task {} incremented to {}", id, g_counter);
    }
}

coro::task<int> async_main(int argc, char* argv[]) {
    // Spawn multiple incrementers and collect handles
    std::vector<coro::join_handle<void>> handles;
    for (int i = 0; i < 4; ++i) {
        handles.push_back(increment(i).spawn());
    }

    // Wait for all to finish
    for (auto& h : handles) {
        co_await h;
    }

    ELIO_LOG_INFO("Final counter: {}", g_counter);
    co_return 0;
}

ELIO_ASYNC_MAIN(async_main)
```

## RPC Framework

A minimal RPC server and client using Elio's binary RPC framework:

```cpp
#include <elio/elio.hpp>
#include <elio/rpc/rpc.hpp>

using namespace elio;

// Define messages
struct GreetRequest {
    std::string name;
    ELIO_RPC_FIELDS(GreetRequest, name);
};

struct GreetResponse {
    std::string message;
    ELIO_RPC_FIELDS(GreetResponse, message);
};

// Define method (id=1)
using Greet = ELIO_RPC_METHOD(1, GreetRequest, GreetResponse);

// Server
coro::task<void> run_server() {
    auto listener = net::tcp_listener::bind(net::ipv4_address("0.0.0.0", 9000));
    if (!listener) co_return;

    rpc::tcp_rpc_server server;
    server.register_method<Greet>([](const GreetRequest& req) -> coro::task<GreetResponse> {
        co_return GreetResponse{.message = "Hello, " + req.name + "!"};
    });

    co_await server.serve(*listener);
}

// Client
coro::task<void> run_client() {
    auto client = co_await rpc::tcp_rpc_client::connect("127.0.0.1", 9000);
    if (!client) co_return;

    auto result = co_await (*client)->call<Greet>(GreetRequest{.name = "World"});
    if (result) {
        ELIO_LOG_INFO("Response: {}", result->message);
    }
}
```

## Hash Functions

Using CRC32 and SHA-256 hash functions:

```cpp
#include <elio/hash/sha256.hpp>
#include <elio/hash/crc32.hpp>

using namespace elio::hash;

void hash_example() {
    // One-shot SHA-256
    std::string hex = sha256_hex("Hello, World!");

    // Incremental hashing
    sha256_context ctx;
    ctx.update("Hello, ");
    ctx.update("World!");
    auto digest = ctx.finalize();
    std::string hex2 = to_hex(digest);

    // CRC32 checksum
    uint32_t crc = crc32("data", 4);

    // CRC32 over scatter-gather buffers
    struct iovec iov[2] = {
        {(void*)"Hello", 5},
        {(void*)" World", 6}
    };
    uint32_t sg_crc = crc32_iovec(iov, 2);
}
```

## Building Examples

All examples are built automatically with CMake:

```bash
cd build
make

# Run individual examples
./examples/hello_world
./examples/tcp_echo_server
./examples/uds_echo_server /tmp/echo.sock
./examples/uds_echo_server @my_socket    # Abstract socket
./examples/uds_echo_client /tmp/echo.sock
./examples/http_client https://httpbin.org/get
./examples/http2_client https://nghttp2.org/
./examples/signal_handling              # Signal handling example
```

## See Also

- [[Signal-Handling]] - Detailed guide on signal handling with signalfd
- [[RPC-Framework]] - RPC system documentation
- [[Hash-Functions]] - Hash function documentation
- [[Core-Concepts]] - Understanding Elio's architecture
- [[Networking]] - TCP and HTTP usage
