# Signal Handling

This page explains how to handle Unix signals safely in Elio coroutines using `signalfd`.

## The Problem with Traditional Signals

Traditional signal handlers have significant limitations in coroutine-based applications:

1. **Async-signal-safety**: Signal handlers can interrupt at any point, even in the middle of coroutine scheduling operations. Only a limited set of functions are safe to call in signal handlers.

2. **Race conditions**: Modifying scheduler state, coroutine handles, or shared data structures from a signal handler is inherently unsafe.

3. **Limited functionality**: You can't use `co_await`, I/O operations, or most standard library functions in signal handlers.

## The Solution: signalfd

Linux's `signalfd(2)` converts signals into file descriptor events. By blocking signals with `sigprocmask` and reading them via `signalfd`, signals become normal I/O events that can be handled in a regular coroutine context.

```cpp
#include <elio/signal/signalfd.hpp>

using namespace elio::signal;
```

## Quick Start

### Basic Signal Handling

```cpp
coro::task<void> handle_shutdown() {
    // Create signal set with signals to handle
    signal_set sigs{SIGINT, SIGTERM};
    
    // Create signalfd - automatically blocks the signals
    signal_fd sigfd(sigs);
    
    // Wait for signal in coroutine context
    while (running) {
        auto info = co_await sigfd.wait();
        if (info) {
            if (info->signo == SIGINT || info->signo == SIGTERM) {
                ELIO_LOG_INFO("Shutdown requested via {}", info->full_name());
                running = false;
            }
        }
    }
    co_return;
}
```

### Waiting for a Single Signal

```cpp
// Simple one-shot signal wait
auto info = co_await wait_signal(SIGTERM);
ELIO_LOG_INFO("Received {}", info.full_name());
```

## API Reference

### signal_set

Manages a set of signals using `sigset_t`.

```cpp
// Create empty set
signal_set sigs;

// Add signals (chainable)
sigs.add(SIGINT).add(SIGTERM).add(SIGUSR1);

// Create with initializer list
signal_set sigs{SIGINT, SIGTERM, SIGUSR1};

// Remove a signal
sigs.remove(SIGUSR1);

// Check membership
if (sigs.contains(SIGINT)) { /* ... */ }

// Clear or fill
sigs.clear();  // Empty set
sigs.fill();   // All signals

// Block/unblock for current thread
sigset_t old_mask;
sigs.block(&old_mask);    // Block these signals
sigs.unblock();           // Unblock these signals
sigs.set_mask(&old_mask); // Replace signal mask
```

### signal_fd

Async-friendly signalfd wrapper.

```cpp
// Create with automatic blocking
signal_fd sigfd(sigs);

// Create with explicit I/O context
signal_fd sigfd(sigs, my_io_context);

// Don't auto-block (caller manages signal mask)
signal_fd sigfd(sigs, ctx, false);

// Check validity
if (sigfd.valid()) { /* ... */ }
if (sigfd) { /* ... */ }  // bool conversion

// Get file descriptor
int fd = sigfd.fd();

// Async wait
auto info = co_await sigfd.wait();

// Sync try-read (non-blocking)
auto info = sigfd.try_read();

// Update signal set
signal_set new_sigs{SIGUSR2};
sigfd.update(new_sigs);

// Restore original signal mask
sigfd.restore_mask();

// Close explicitly
sigfd.close();
```

### signal_info

Information about a received signal.

```cpp
auto info = co_await sigfd.wait();
if (info) {
    int signo = info->signo;          // Signal number
    const char* name = info->name();  // "INT", "TERM", etc.
    std::string full = info->full_name(); // "SIGINT", "SIGTERM"
    
    uint32_t pid = info->pid;  // Sender PID
    uint32_t uid = info->uid;  // Sender UID
    int32_t code = info->code; // Signal code (SI_USER, SI_KERNEL, etc.)
}
```

### signal_block_guard

RAII guard for temporary signal blocking.

```cpp
{
    signal_block_guard guard(sigs);
    // Signals are blocked here
} // Signals restored automatically
```

### Utility Functions

```cpp
// Get signal name from number
const char* name = signal_name(SIGINT);  // "INT"

// Get signal number from name
int signo = signal_number("SIGINT");  // 2
int signo = signal_number("INT");     // 2 (prefix optional)
```

## Best Practices

### 1. Block Signals Early

Block signals **before** creating any threads to ensure all threads inherit the blocked mask:

```cpp
int main() {
    // Block signals FIRST, before anything else
    signal_set sigs{SIGINT, SIGTERM, SIGUSR1};
    sigs.block_all_threads();
    
    // Now create scheduler and start threads
    scheduler sched(4);
    sched.start();
    
    // ...
}
```

### 2. Handle Multiple Signal Types

```cpp
coro::task<void> signal_router(scheduler& sched) {
    signal_set sigs{SIGINT, SIGTERM, SIGUSR1, SIGUSR2, SIGHUP};
    signal_fd sigfd(sigs);
    
    while (running) {
        auto info = co_await sigfd.wait();
        if (!info) continue;
        
        switch (info->signo) {
            case SIGINT:
            case SIGTERM:
                initiate_shutdown();
                break;
            case SIGHUP:
                reload_configuration();
                break;
            case SIGUSR1:
                print_status();
                break;
            case SIGUSR2:
                rotate_logs();
                break;
        }
    }
    co_return;
}
```

### 3. Graceful Shutdown Pattern

```cpp
std::atomic<bool> g_running{true};

coro::task<void> shutdown_handler() {
    signal_set sigs{SIGINT, SIGTERM};
    signal_fd sigfd(sigs);
    
    auto info = co_await sigfd.wait();
    ELIO_LOG_INFO("Shutdown signal received: {}", info->full_name());
    g_running = false;
    co_return;
}

coro::task<void> worker() {
    while (g_running) {
        // Do work
        co_await process_request();
    }
    // Cleanup before exit
    co_return;
}
```

### 4. Signal Information Logging

```cpp
void log_signal(const signal_info& info) {
    ELIO_LOG_INFO("Signal: {} ({})", info.full_name(), info.signo);
    ELIO_LOG_INFO("  Sender: PID={}, UID={}", info.pid, info.uid);
    ELIO_LOG_INFO("  Code: {}", info.code);
}
```

## Complete Example

See `examples/signal_handling.cpp` for a complete example showing:

- Signal blocking before thread creation
- Signal handler coroutine
- Graceful shutdown coordination
- Status requests via SIGUSR1

Build and run:

```bash
cd build
make signal_handling
./signal_handling

# In another terminal:
kill -USR1 <pid>  # Print status
kill -INT <pid>   # or Ctrl+C for graceful shutdown
```

## Comparison with Traditional Approach

### Traditional (Unsafe)

```cpp
// DON'T do this in coroutine applications!
void signal_handler(int signo) {
    // Can't use co_await here
    // Can't safely modify scheduler state
    // Limited to async-signal-safe functions
    g_shutdown_flag = true;  // Only atomic operations are safe
}
```

### With signalfd (Safe)

```cpp
coro::task<void> signal_handler() {
    signal_fd sigfd(signal_set{SIGINT, SIGTERM});
    
    auto info = co_await sigfd.wait();  // Full coroutine support!
    
    // Can use any function here
    co_await cleanup_connections();
    co_await flush_caches();
    
    ELIO_LOG_INFO("Clean shutdown complete");
    co_return;
}
```

## See Also

- [[Core-Concepts]] - Coroutines and scheduler basics
- [[Examples]] - More code examples
- `man signalfd` - Linux signalfd documentation
- `man sigprocmask` - Signal mask manipulation
