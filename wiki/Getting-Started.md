# Getting Started

This guide will help you get Elio up and running in your project.

## Installation

### Prerequisites

```bash
# Ubuntu/Debian (22.04+)
sudo apt install build-essential cmake ninja-build g++-12 liburing-dev libssl-dev

# Fedora
sudo dnf install gcc-c++ cmake ninja-build liburing-devel openssl-devel

# Arch Linux
sudo pacman -S base-devel cmake ninja liburing openssl
```

### Building from Source

```bash
git clone https://github.com/Coldwings/Elio.git
cd Elio
cmake -B build -G Ninja
cmake --build build
```

### Running Tests

```bash
cd build
ctest --output-on-failure

# With AddressSanitizer
./tests/elio_tests_asan
```

### CMake Integration

#### Using FetchContent

```cmake
include(FetchContent)

FetchContent_Declare(elio
    GIT_REPOSITORY https://github.com/Coldwings/Elio.git
    GIT_TAG main
)
FetchContent_MakeAvailable(elio)

target_link_libraries(your_target PRIVATE elio)

# For HTTP/TLS support
target_link_libraries(your_target PRIVATE elio_http)
```

#### Using add_subdirectory

```cmake
add_subdirectory(path/to/elio)
target_link_libraries(your_target PRIVATE elio)
```

## Your First Program

Create a simple "Hello World" program:

```cpp
#include <elio/elio.hpp>
#include <iostream>

using namespace elio;

coro::task<std::string> get_greeting() {
    co_return "Hello from Elio!";
}

coro::task<int> async_main(int argc, char* argv[]) {
    std::cout << "Program: " << argv[0] << std::endl;
    if (argc > 1) {
        std::cout << "First argument: " << argv[1] << std::endl;
    }
    
    std::string greeting = co_await get_greeting();
    std::cout << greeting << std::endl;
    co_return 0;
}

ELIO_ASYNC_MAIN(async_main)
```

The `ELIO_ASYNC_MAIN` macro handles scheduler setup, I/O context initialization, and cleanup automatically. It passes `argc` and `argv` to your async_main function.

### Using I/O Context

The scheduler automatically creates and manages an I/O context. Access it via `io::default_io_context()`:

```cpp
coro::task<int> async_main(int argc, char* argv[]) {
    // Get the I/O context for async operations
    auto& ctx = io::default_io_context();
    
    // Use ctx for networking, timers, etc.
    co_await time::sleep_for(std::chrono::seconds(1));
    
    co_return 0;
}

ELIO_ASYNC_MAIN(async_main)
```

### Alternative: Using elio::run()

For more control, use `elio::run()` directly:

```cpp
int main(int argc, char* argv[]) {
    return elio::run(async_main(argc, argv));
}
```

Or configure via `run_config`:

```cpp
int main(int argc, char* argv[]) {
    elio::run_config config;
    config.num_threads = 4;  // Use 4 worker threads
    
    return elio::run(async_main(argc, argv), config);
}
```

### Macros Reference

| Macro | async_main signature | Description |
|-------|---------------------|-------------|
| `ELIO_ASYNC_MAIN` | `task<int>(int, char**)` | With args, returns exit code |
| `ELIO_ASYNC_MAIN_VOID` | `task<void>(int, char**)` | With args, always exits 0 |
| `ELIO_ASYNC_MAIN_NOARGS` | `task<int>()` | No args, returns exit code |
| `ELIO_ASYNC_MAIN_VOID_NOARGS` | `task<void>()` | No args, always exits 0 |

## Running the Examples

```bash
cd build

# Hello World
./examples/hello_world

# TCP Echo Server (run in one terminal)
./examples/tcp_echo_server 8080

# TCP Echo Client (run in another terminal)
./examples/tcp_echo_client localhost 8080

# HTTP Server
./examples/http_server 8080

# HTTP Client
./examples/http_client https://httpbin.org/get
```

## Project Structure

```
elio/
├── include/elio/          # Header files
│   ├── coro/              # Coroutine primitives
│   ├── runtime/           # Scheduler and workers
│   ├── io/                # I/O backends
│   ├── net/               # TCP networking
│   ├── http/              # HTTP client/server
│   ├── tls/               # TLS/SSL support
│   ├── sync/              # Synchronization primitives
│   ├── time/              # Timers
│   └── log/               # Logging
├── examples/              # Example programs
├── tests/                 # Unit tests
└── CMakeLists.txt
```

## Next Steps

- Read [[Core Concepts]] to understand how Elio works
- Explore [[Networking]] for TCP and HTTP usage
- Check out [[Examples]] for more code samples
