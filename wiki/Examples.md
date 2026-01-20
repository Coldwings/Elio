# Examples

This page provides complete, runnable examples demonstrating Elio's features.

## Hello World

The simplest Elio program:

```cpp
#include <elio/elio.hpp>
#include <iostream>

using namespace elio;

coro::task<std::string> get_greeting() {
    co_return "Hello from Elio!";
}

coro::task<void> main_task() {
    std::string greeting = co_await get_greeting();
    std::cout << greeting << std::endl;
    co_return;
}

int main() {
    runtime::scheduler sched(2);
    sched.start();
    
    auto t = main_task();
    sched.spawn(t.release());
    
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    sched.shutdown();
    return 0;
}
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

coro::task<void> pipeline() {
    int a = co_await step1();        // 10
    int b = co_await step2(a);       // 20
    int c = co_await step3(b);       // 25
    
    ELIO_LOG_INFO("Final result: {}", c);
    co_return;
}
```

## TCP Echo Server

A concurrent TCP server that echoes data back to clients:

```cpp
#include <elio/elio.hpp>
#include <csignal>
#include <atomic>

using namespace elio;

std::atomic<bool> g_running{true};

void signal_handler(int) { g_running = false; }

coro::task<void> handle_client(net::tcp_stream stream, int id) {
    ELIO_LOG_INFO("[Client {}] Connected", id);
    
    char buffer[1024];
    while (g_running) {
        auto result = co_await stream.read(buffer, sizeof(buffer));
        if (result.result <= 0) break;
        
        co_await stream.write(buffer, result.result);
    }
    
    ELIO_LOG_INFO("[Client {}] Disconnected", id);
    co_return;
}

coro::task<void> server(uint16_t port, runtime::scheduler& sched) {
    auto& ctx = io::default_io_context();
    auto listener = net::tcp_listener::bind(net::ipv4_address(port), ctx);
    
    if (!listener) {
        ELIO_LOG_ERROR("Failed to bind");
        co_return;
    }
    
    ELIO_LOG_INFO("Server listening on port {}", port);
    
    int client_id = 0;
    while (g_running) {
        auto stream = co_await listener->accept();
        if (!stream) continue;
        
        auto handler = handle_client(std::move(*stream), ++client_id);
        sched.spawn(handler.release());
    }
    co_return;
}

int main() {
    std::signal(SIGINT, signal_handler);
    
    runtime::scheduler sched(4);
    sched.set_io_context(&io::default_io_context());
    sched.start();
    
    auto srv = server(8080, sched);
    sched.spawn(srv.release());
    
    while (g_running) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    
    sched.shutdown();
    return 0;
}
```

## UDS Echo Server

A concurrent Unix Domain Socket server that echoes data back to clients:

```cpp
#include <elio/elio.hpp>
#include <csignal>
#include <atomic>

using namespace elio;

std::atomic<bool> g_running{true};

void signal_handler(int) { g_running = false; }

coro::task<void> handle_client(net::uds_stream stream, int id) {
    ELIO_LOG_INFO("[Client {}] Connected", id);
    
    char buffer[1024];
    while (g_running) {
        auto result = co_await stream.read(buffer, sizeof(buffer));
        if (result.result <= 0) break;
        
        co_await stream.write(buffer, result.result);
    }
    
    ELIO_LOG_INFO("[Client {}] Disconnected", id);
    co_return;
}

coro::task<void> server(const net::unix_address& addr, runtime::scheduler& sched) {
    auto& ctx = io::default_io_context();
    
    net::uds_options opts;
    opts.unlink_on_bind = true;
    
    auto listener = net::uds_listener::bind(addr, ctx, opts);
    
    if (!listener) {
        ELIO_LOG_ERROR("Failed to bind to {}", addr.to_string());
        co_return;
    }
    
    ELIO_LOG_INFO("Server listening on {}", addr.to_string());
    
    int client_id = 0;
    while (g_running) {
        auto stream = co_await listener->accept();
        if (!stream) continue;
        
        auto handler = handle_client(std::move(*stream), ++client_id);
        sched.spawn(handler.release());
    }
    co_return;
}

int main() {
    std::signal(SIGINT, signal_handler);
    
    // Use filesystem socket
    net::unix_address addr("/tmp/echo.sock");
    // Or use abstract socket (Linux-specific):
    // auto addr = net::unix_address::abstract("echo_server");
    
    runtime::scheduler sched(4);
    sched.set_io_context(&io::default_io_context());
    sched.start();
    
    auto srv = server(addr, sched);
    sched.spawn(srv.release());
    
    while (g_running) {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    
    sched.shutdown();
    return 0;
}
```

## UDS Client

A Unix Domain Socket client that connects to a UDS server:

```cpp
#include <elio/elio.hpp>

using namespace elio;

coro::task<void> client_main(const net::unix_address& addr) {
    auto& ctx = io::default_io_context();
    
    ELIO_LOG_INFO("Connecting to {}...", addr.to_string());
    
    auto stream = co_await net::uds_connect(ctx, addr);
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
    
    co_return;
}

int main() {
    // Match server's socket path
    net::unix_address addr("/tmp/echo.sock");
    // Or abstract socket:
    // auto addr = net::unix_address::abstract("echo_server");
    
    runtime::scheduler sched(2);
    sched.set_io_context(&io::default_io_context());
    sched.start();
    
    std::atomic<bool> done{false};
    auto run = [&]() -> coro::task<void> {
        co_await client_main(addr);
        done = true;
    };
    
    auto t = run();
    sched.spawn(t.release());
    
    while (!done) {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    
    sched.shutdown();
    return 0;
}
```

## HTTP Client

Making HTTP requests with various methods:

```cpp
#include <elio/elio.hpp>
#include <elio/http/http.hpp>
#include <elio/tls/tls.hpp>

using namespace elio;
using namespace elio::http;

coro::task<void> http_examples(io::io_context& ctx) {
    client_config config;
    config.user_agent = "elio-example/1.0";
    config.follow_redirects = true;
    
    client c(ctx, config);
    
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
    
    co_return;
}
```

## HTTP/2 Client

Making HTTP/2 requests with multiplexed streams:

```cpp
#include <elio/elio.hpp>
#include <elio/http/http2.hpp>

using namespace elio;
using namespace elio::http;

coro::task<void> http2_examples(io::io_context& ctx) {
    h2_client_config config;
    config.user_agent = "elio-example/1.0";
    config.max_concurrent_streams = 100;
    
    h2_client client(ctx, config);
    
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
    
    co_return;
}
```

## HTTP Server

A simple REST API server:

```cpp
#include <elio/elio.hpp>
#include <elio/http/http.hpp>

using namespace elio;
using namespace elio::http;

coro::task<void> router(request& req, response& resp) {
    auto path = req.path();
    auto method = req.method();
    
    if (method == method::GET && path == "/") {
        resp.set_status(status::ok);
        resp.set_header("Content-Type", "text/html");
        resp.set_body("<h1>Welcome to Elio!</h1>");
    }
    else if (method == method::GET && path == "/api/status") {
        resp.set_status(status::ok);
        resp.set_header("Content-Type", "application/json");
        resp.set_body(R"({"status": "ok", "version": "1.0"})");
    }
    else if (method == method::POST && path == "/api/echo") {
        resp.set_status(status::ok);
        resp.set_header("Content-Type", req.content_type());
        resp.set_body(req.body());
    }
    else {
        resp.set_status(status::not_found);
        resp.set_header("Content-Type", "application/json");
        resp.set_body(R"({"error": "Not Found"})");
    }
    
    co_return;
}

int main() {
    runtime::scheduler sched(4);
    sched.set_io_context(&io::default_io_context());
    sched.start();
    
    server_config config;
    config.port = 8080;
    config.handler = router;
    
    // Start server...
    
    sched.shutdown();
    return 0;
}
```

## Parallel Tasks

Running multiple tasks concurrently:

```cpp
#include <elio/elio.hpp>
#include <vector>

using namespace elio;

coro::task<int> compute(int id, int value) {
    ELIO_LOG_INFO("Task {} computing...", id);
    // Simulate work
    co_return value * value;
}

coro::task<void> parallel_compute(runtime::scheduler& sched) {
    std::vector<coro::task<int>> tasks;
    
    // Create multiple tasks
    for (int i = 0; i < 10; ++i) {
        tasks.push_back(compute(i, i + 1));
    }
    
    // Spawn all tasks
    for (auto& t : tasks) {
        sched.spawn(t.release());
    }
    
    co_return;
}
```

## Timer Example

Using timers for delays:

```cpp
#include <elio/elio.hpp>

using namespace elio;

coro::task<void> delayed_task(io::io_context& ctx) {
    ELIO_LOG_INFO("Starting...");
    
    co_await time::sleep(ctx, std::chrono::seconds(1));
    ELIO_LOG_INFO("1 second passed");
    
    co_await time::sleep(ctx, std::chrono::milliseconds(500));
    ELIO_LOG_INFO("500ms more passed");
    
    co_return;
}
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

coro::task<void> error_handling() {
    try {
        int result = co_await may_fail(false);
        ELIO_LOG_INFO("Success: {}", result);
        
        int fail = co_await may_fail(true);  // Throws
        ELIO_LOG_INFO("Never reached: {}", fail);
    } catch (const std::exception& e) {
        ELIO_LOG_ERROR("Caught: {}", e.what());
    }
    co_return;
}
```

## Synchronization

Using mutex and condition variables:

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
    co_return;
}

coro::task<void> run_concurrent(runtime::scheduler& sched) {
    // Spawn multiple incrementers
    for (int i = 0; i < 4; ++i) {
        auto task = increment(i);
        sched.spawn(task.release());
    }
    co_return;
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
```
