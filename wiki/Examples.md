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
coro::cancel_source g_accept_cancel;

// Signal handler coroutine - waits for SIGINT/SIGTERM
coro::task<void> signal_handler_task() {
    signal_set sigs{SIGINT, SIGTERM};
    signal_fd sigfd(sigs);

    auto info = co_await sigfd.wait();
    if (info) {
        ELIO_LOG_INFO("Received signal: {}", info->full_name());
    }

    g_running = false;
    g_accept_cancel.cancel();
}

coro::task<void> handle_client(net::tcp_stream stream, int id) {
    ELIO_LOG_INFO("[Client {}] Connected", id);

    char buffer[1024];
    while (g_running) {
        auto result = co_await stream.read(buffer, sizeof(buffer));
        if (result.result <= 0) break;

        auto written = co_await stream.write_exactly(buffer, result.result);
        if (written.result <= 0) break;
    }

    ELIO_LOG_INFO("[Client {}] Disconnected", id);
}

coro::task<int> async_main(int argc, char* argv[]) {
    // Spawn signal handler
    elio::go(signal_handler_task);

    auto listener = net::tcp_listener::bind(net::ipv4_address(8080));
    if (!listener) {
        ELIO_LOG_ERROR("Failed to bind");
        co_return 1;
    }

    ELIO_LOG_INFO("Server listening on port 8080");

    int client_id = 0;
    while (g_running) {
        auto stream = co_await listener->accept(g_accept_cancel.get_token());
        if (!stream) {
            if (!g_running || g_accept_cancel.is_cancelled()) break;
            continue;
        }

        elio::go([stream = std::move(*stream), id = ++client_id]() mutable {
            return handle_client(std::move(stream), id);
        });
    }
    co_return 0;
}

int main(int argc, char* argv[]) {
    // Block shutdown signals before scheduler worker threads are created
    signal_set sigs{SIGINT, SIGTERM};
    sigs.block_all_threads();

    return elio::run(async_main, argc, argv);
}
```

## UDS Echo Server

A concurrent Unix Domain Socket server that echoes data back to clients:

```cpp
#include <elio/elio.hpp>
#include <atomic>

using namespace elio;
using namespace elio::signal;

std::atomic<bool> g_running{true};
coro::cancel_source g_accept_cancel;

// Signal handler coroutine
coro::task<void> signal_handler_task() {
    signal_set sigs{SIGINT, SIGTERM};
    signal_fd sigfd(sigs);

    auto info = co_await sigfd.wait();
    if (info) {
        ELIO_LOG_INFO("Received signal: {}", info->full_name());
    }

    g_running = false;
    g_accept_cancel.cancel();
}

coro::task<void> handle_client(net::uds_stream stream, int id) {
    ELIO_LOG_INFO("[Client {}] Connected", id);

    char buffer[1024];
    while (g_running) {
        auto result = co_await stream.read(buffer, sizeof(buffer));
        if (result.result <= 0) break;

        auto written = co_await stream.write_exactly(buffer, result.result);
        if (written.result <= 0) break;
    }

    ELIO_LOG_INFO("[Client {}] Disconnected", id);
}

coro::task<int> async_main(int argc, char* argv[]) {
    // Spawn signal handler
    elio::go(signal_handler_task);

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

    ELIO_LOG_INFO("Server listening on {}", addr.to_string());

    int client_id = 0;
    while (g_running) {
        auto stream = co_await listener->accept(g_accept_cancel.get_token());
        if (!stream) {
            if (!g_running || g_accept_cancel.is_cancelled()) break;
            continue;
        }

        elio::go([stream = std::move(*stream), id = ++client_id]() mutable {
            return handle_client(std::move(stream), id);
        });
    }
    co_return 0;
}

int main(int argc, char* argv[]) {
    // Block shutdown signals before scheduler worker threads are created
    signal_set sigs{SIGINT, SIGTERM};
    sigs.block_all_threads();

    return elio::run(async_main, argc, argv);
}
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
    co_await stream->write_exactly(msg, strlen(msg));

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

Making HTTP/2 requests with sequential connection reuse:

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

    // Sequential requests can reuse pooled HTTP/2 connections.
    ELIO_LOG_INFO("=== HTTP/2 Connection Reuse ===");
    for (int i = 0; i < 5; ++i) {
        auto resp = co_await client.get("https://nghttp2.org/");
        if (resp) {
            ELIO_LOG_INFO("Request {}: {} bytes", i + 1, resp->body().size());
        }
    }
    // Requests above are awaited sequentially; use separate clients for
    // parallel high-level requests until shared-session multiplexing is
    // implemented.

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
    co_await elio::serve(srv, [&]() { return srv.listen(net::ipv4_address(8080)); });

    co_return 0;
}

int main(int argc, char* argv[]) {
    elio::signal::signal_set shutdown_signals(elio::default_shutdown_signals);
    shutdown_signals.block_all_threads();
    return elio::run(async_main, argc, argv);
}
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
        handles.push_back(elio::spawn(compute, i, i + 1));
    }

    // Await all results
    int total = 0;
    for (auto& h : handles) {
        total += co_await h;
    }
    ELIO_LOG_INFO("Sum of squares: {}", total);

    // Fire-and-forget tasks (no result collection)
    for (int i = 0; i < 5; ++i) {
        elio::go(compute, i, i);
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

// Task with affinity to a specific worker
coro::task<void> pinned_worker(size_t target_worker) {
    // Bind to target worker and migrate there
    co_await set_affinity(target_worker);

    std::cout << "Running on worker " << current_worker_id() << std::endl;

    // Do work - steal attempts bounce the task back to this worker
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
        elio::go(pinned_worker, i);
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
        co_await g_mutex.lock();
        ++g_counter;
        ELIO_LOG_DEBUG("Task {} incremented to {}", id, g_counter);
        g_mutex.unlock();
    }
}

coro::task<int> async_main(int argc, char* argv[]) {
    // Spawn multiple incrementers and collect handles
    std::vector<coro::join_handle<void>> handles;
    for (int i = 0; i < 4; ++i) {
        handles.push_back(elio::spawn(increment, i));
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
#include <elio/hash/hash.hpp>

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

## File I/O

### Reading and Writing Files

```cpp
#include <elio/elio.hpp>
#include <iostream>

using namespace elio;

coro::task<void> file_operations() {
    // Read a file
    auto content = co_await io::read_file("/etc/hostname");
    if (content) {
        std::cout << "Hostname: " << *content << std::endl;
    }

    // Write a file
    bool ok = co_await io::write_file("/tmp/hello.txt", "Hello, Elio!\n");
    if (ok) {
        std::cout << "File written successfully" << std::endl;
    }

    // Append to a file
    ok = co_await io::append_file("/tmp/hello.txt", "Appended line\n");

    // Check file metadata
    if (io::file_exists("/tmp/hello.txt")) {
        auto size = io::file_size("/tmp/hello.txt");
        std::cout << "File size: " << (size ? *size : 0) << " bytes" << std::endl;
    }

    // Read a directory
    auto entries = io::read_dir("/tmp");
    if (entries) {
        for (const auto& entry : *entries) {
            if (entry.is_file) {
                std::cout << "[FILE] " << entry.name << std::endl;
            } else if (entry.is_dir) {
                std::cout << "[DIR]  " << entry.name << std::endl;
            }
        }
    }
}
```

### Batch I/O

Read multiple file regions in a single syscall:

```cpp
coro::task<void> batch_read_example(int fd) {
    char header[64] = {0};
    char footer[64] = {0};
    char middle[128] = {0};

    std::array<io::batch_read_segment, 3> segments;
    segments[0] = {0, header, 64};       // Read first 64 bytes
    segments[1] = {1024, middle, 128};   // Read 128 bytes at offset 1024
    segments[2] = {-1, footer, 64};      // Read from current position

    auto results = co_await io::batch_read(fd, segments);

    for (size_t i = 0; i < results.size(); ++i) {
        if (results[i] > 0) {
            std::cout << "Segment " << i << ": read " << results[i] << " bytes" << std::endl;
        } else {
            std::cerr << "Segment " << i << ": error " << -results[i] << std::endl;
        }
    }
}
```

Write multiple file regions simultaneously:

```cpp
coro::task<void> batch_write_example(int fd) {
    const char* part1 = "HEADER";
    const char* part2 = "FOOTER";

    std::array<io::batch_write_segment, 2> segments;
    segments[0] = {0, part1, 6};      // Write at start
    segments[1] = {1024, part2, 6};   // Write at offset 1024

    auto results = co_await io::batch_write(fd, segments);
    // Both writes happen concurrently in the kernel
}
```

## Building Examples

Configure with `ELIO_BUILD_EXAMPLES=ON` to build examples. Some example groups
are only created when their feature targets are enabled:

- HTTP, WebSocket, SSE, and HTTP client examples require `elio_http`.
- HTTP/2 examples require `elio_http2`.
- RDMA examples require the matching `elio_rdma*` targets.
- TCP benchmark comparison programs require `ELIO_BUILD_TCP_BENCHMARKS=ON`;
  install libuv development files (`libuv1-dev` on Debian/Ubuntu) if you need
  `bench_tcp_libuv`.

```bash
cmake -S . -B build -DELIO_BUILD_EXAMPLES=ON
cmake --build build --target hello_world tcp_echo_server uds_echo_server

# Run individual examples
./build/examples/hello_world
./build/examples/tcp_echo_server
./build/examples/uds_echo_server /tmp/echo.sock
./build/examples/uds_echo_server @my_socket    # Abstract socket
./build/examples/uds_echo_client /tmp/echo.sock

# Feature-gated examples are available when their targets were configured
./build/examples/http_client https://httpbin.org/get
./build/examples/http2_client https://nghttp2.org/
./build/examples/signal_handling              # Signal handling example
```

## Autoscaler

Automatic worker thread scaling based on load:

```cpp
#include <elio/elio.hpp>
#include <elio/runtime/autoscaler.hpp>
#include <atomic>
#include <chrono>
#include <random>

using namespace elio;

// Task that simulates work
coro::task<void> workload_task(std::atomic<int>& counter) {
    static thread_local std::mt19937 rng(
        std::hash<std::thread::id>{}(std::this_thread::get_id())
    );
    std::uniform_int_distribution<int> dist(1, 50);
    std::this_thread::sleep_for(std::chrono::milliseconds(dist(rng)));
    counter.fetch_add(1, std::memory_order_relaxed);
    co_return;
}

int main() {
    // Configure autoscaler
    elio::runtime::autoscaler_config config;
    config.tick_interval = std::chrono::milliseconds(200);
    config.overload_threshold = 20;      // Scale up when queue > 20
    config.idle_threshold = 5;          // Scale down when queue < 5
    config.idle_delay = std::chrono::seconds(5);
    config.min_workers = 2;
    config.max_workers = 8;

    // Create scheduler and autoscaler
    elio::runtime::scheduler sched(2);
    sched.start();

    elio::runtime::autoscaler<elio::runtime::scheduler,
        elio::runtime::on_overload<elio::runtime::scale_up<elio::runtime::null>>,
        elio::runtime::on_idle<elio::runtime::scale_down<elio::runtime::null>>,
        elio::runtime::on_block<elio::runtime::log>
    > autoscaler(config);

    autoscaler.start(&sched);

    // Submit workload...
    std::atomic<int> completed{0};
    for (int i = 0; i < 1000; ++i) {
        sched.go(workload_task, std::ref(completed));
    }

    // Monitor autoscaler behavior
    while (completed.load() < 1000) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        std::cout << "Workers: " << sched.num_threads()
                  << ", Pending: " << sched.pending_tasks() << std::endl;
    }

    autoscaler.stop();
    sched.shutdown();
}
```

The autoscaler supports:
- **Triggers**: `on_overload`, `on_idle`, `on_block`
- **Actions**: `scale_up`, `scale_down`, `log`, `null`
- **Combinators**: `on_success`, `on_failure`

## RDMA Ping-Pong (Mock Backend)

A single-process SEND/RECV ping-pong using a mock backend — no RDMA hardware required. Demonstrates `connection`, `dispatcher`, `cq_pump`, and `wc_result`. See [`examples/rdma_pingpong_mock.cpp`](../examples/rdma_pingpong_mock.cpp).

```cpp
// Abridged — see full source for mock_backend and mock_qp wiring.
#include <elio/elio.hpp>
#include <elio/rdma/rdma.hpp>

using elio::rdma::connection;
using elio::rdma::buffer_view;

// A side: send "ping N", await echoed reply.
elio::coro::task<void> ping_side(connection<mock_backend>& conn) {
    std::vector<char> tx(128), rx(128);
    for (int i = 0; i < 16; ++i) {
        auto recv_aw = conn.recv(buffer_view{rx.data(), rx.size(), 0});
        std::string msg = "ping " + std::to_string(i);
        std::memcpy(tx.data(), msg.data(), msg.size());
        co_await conn.send(buffer_view{tx.data(), msg.size(), 0});
        auto wc = co_await recv_aw;
        // wc.byte_len tells us how much was echoed
    }
}
```

Build: `cmake -B build -DELIO_ENABLE_RDMA=ON && cmake --build build --target rdma_pingpong_mock`

## RDMA Request/Response (ibverbs + CM)

Client sends a request via SEND, server writes the response with RDMA WRITE and signals completion with SEND_WITH_IMM. Uses `endpoint`, `acceptor`, `connect` from `elio::rdma_ibverbs`. Requires a working uverbs ABI (Soft-RoCE / real HCA). See [`examples/rdma_req_resp_ibverbs.cpp`](../examples/rdma_req_resp_ibverbs.cpp).

```cpp
// Abridged client side — see full source for the server.
#include <elio/rdma/rdma.hpp>
#include <elio/rdma_cm/rdma_cm.hpp>
#include <elio/rdma_ibverbs/rdma_ibverbs.hpp>

elio::coro::task<void> client(elio::runtime::scheduler& sched) {
    elio::rdma_cm::event_channel cm_ch;
    auto ep = co_await elio::rdma_ibverbs::connect(
        cm_ch, dst_addr, sizeof(*dst_addr),
        {.max_send_wr = 4, .max_recv_wr = 4});
    ep.start_cq_pump(sched);

    auto req_mr  = ep.register_buffer(req_buf, sizeof(req_buf), IBV_ACCESS_LOCAL_WRITE);
    auto resp_mr = ep.register_buffer(resp_buf, sizeof(resp_buf),
                                      IBV_ACCESS_LOCAL_WRITE | IBV_ACCESS_REMOTE_WRITE);

    // Post recv for OOB notify BEFORE sending the request.
    auto notify_aw = ep.conn().recv(notify_mr.view());
    co_await ep.conn().send(req_mr.view(0, sizeof(request_header)));
    auto wc = co_await notify_aw;
    // wc.imm_data carries the response length
}
```

Build: `cmake -B build -DELIO_ENABLE_RDMA=ON -DELIO_ENABLE_RDMA_CM=ON -DELIO_ENABLE_RDMA_IBVERBS=ON && cmake --build build --target rdma_req_resp_ibverbs`

## See Also

- [[RDMA-Guide]] - Full RDMA abstraction layer guide
- [[Signal-Handling]] - Detailed guide on signal handling with signalfd
- [[RPC-Framework]] - RPC system documentation
- [[Hash-Functions]] - Hash function documentation
- [[Core-Concepts]] - Understanding Elio's architecture
- [[Networking]] - TCP and HTTP usage
