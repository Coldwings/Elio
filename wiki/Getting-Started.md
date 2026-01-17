# Getting Started

This guide will help you get Elio up and running in your project.

## Installation

### Prerequisites

```bash
# Ubuntu/Debian (24.04+)
sudo apt install build-essential cmake ninja-build g++-14 liburing-dev libssl-dev

# Fedora
sudo dnf install gcc-c++ cmake ninja-build liburing-devel openssl-devel

# Arch Linux
sudo pacman -S base-devel cmake ninja liburing openssl
```

### Building from Source

```bash
git clone https://github.com/user/elio.git
cd elio
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
    GIT_REPOSITORY https://github.com/user/elio.git
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

coro::task<void> main_task() {
    std::string greeting = co_await get_greeting();
    std::cout << greeting << std::endl;
    co_return;
}

int main() {
    // Create scheduler with 2 worker threads
    runtime::scheduler sched(2);
    sched.start();
    
    // Spawn the main task
    auto t = main_task();
    sched.spawn(t.release());
    
    // Wait for completion
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    sched.shutdown();
    
    return 0;
}
```

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
