# Performance Tuning Guide

This guide covers performance optimization techniques for Elio applications.

## Overview

Elio is designed for high performance through:
- Lock-free data structures (Chase-Lev deque)
- Work-stealing scheduler
- Efficient I/O backends (io_uring, epoll)
- Custom coroutine frame allocator
- Minimal synchronization overhead

## Actual Performance Numbers

### Scheduling Benchmarks

| Operation | Typical | Best Case | Notes |
|-----------|---------|-----------|-------|
| Task Spawn | ~1300 ns | ~570 ns | Best with pre-allocated frames |
| Context Switch | ~230 ns | ~212 ns | Suspend and resume |
| Yield | ~30 ns | ~16 ns | Per 1000 vthreads |
| MPSC push | ~5 ns | - | Cross-thread scheduling |
| Chase-Lev push | ~13 ns | - | Local queue operation |
| Frame alloc (cold) | ~250 ns | - | First allocation |
| Frame alloc (hot) | ~72 ns | - | Pool hit |

### I/O Benchmarks

| Scenario | Latency | Throughput |
|----------|---------|------------|
| Single-thread file read | 1.46 μs/read | 685K IOPS |
| 4-thread concurrent read | 0.93 μs/read | 1.07M IOPS |

### Scalability

CPU-bound workload with 100K iterations per task:

| Threads | Throughput | Speedup |
|---------|-----------|---------|
| 1 | ~18K tasks/sec | 1.0x |
| 2 | ~33K tasks/sec | 1.9x |
| 4 | ~56K tasks/sec | 3.2x |
| 8 | ~86K tasks/sec | 4.9x |

Scaling efficiency depends on workload characteristics. Tasks with more computation relative to scheduling overhead will show better scaling.

### Wake-up Mechanism

Elio uses an **eventfd embedded in each worker's I/O backend** (epoll/io_uring) for cross-thread notifications. This provides a single unified wait point — both I/O completions and task wake-ups unblock the same `poll()` call, eliminating the latency gap that exists with separate wait mechanisms. Combined with **Lazy Wake optimization**, which avoids unnecessary syscalls when workers are busy, this minimizes scheduling overhead.

## Built-in Optimizations

### Lazy Wake

Workers track their idle state. Task submissions only trigger wake syscalls when the target worker is actually sleeping:

```cpp
// In worker_thread::schedule()
if (inbox_.push(handle.address())) {
    // Only wake if worker is idle (sleeping)
    if (idle_.load(std::memory_order_relaxed)) {
        wake();  // eventfd write → interrupts I/O poll
    }
}
```

This eliminates unnecessary syscalls when workers are busy processing tasks.

### Unified Wake Mechanism

Each worker's I/O backend (epoll or io_uring) contains an embedded `eventfd`. When a task is submitted to a worker from another thread, the submitter writes to that worker's eventfd. Because the eventfd is registered with the same epoll/io_uring instance that handles I/O completions, both I/O events and task wake-ups unblock the same `poll()` call.

This unified design has two key benefits:

1. **Single wait point.** A worker blocked on I/O poll is immediately woken by a cross-thread task submission. There is no separate condition variable or futex that could introduce a latency gap between "I/O ready" and "task ready" paths.

2. **No redundant syscalls.** Combined with Lazy Wake, an eventfd write is only issued when the target worker is actually idle. If the worker is busy executing tasks or processing I/O completions, the submitter skips the write entirely. The submitted task is picked up when the worker next drains its inbox.

The result is that cross-thread scheduling latency equals one `eventfd_write` plus one `epoll_wait`/`io_uring_enter` return — typically under 5 microseconds — while avoiding any syscall overhead when the worker is already active.

### Wait Strategy

Elio supports configurable wait strategies to balance latency vs CPU usage:

```cpp
#include <elio/runtime/scheduler.hpp>
#include <elio/runtime/wait_strategy.hpp>

using namespace elio::runtime;

// Pure blocking (default) - lowest CPU usage
scheduler sched(4, wait_strategy::blocking());

// Hybrid spin-then-block - good for low-latency workloads
// Spins for 1000 iterations with yield, then blocks on I/O poll
scheduler sched(4, wait_strategy::hybrid(1000));

// Aggressive spinning - ultra-low latency (uses pause instruction)
scheduler sched(4, wait_strategy::spinning(1000));

// Custom strategy
wait_strategy custom{
    .spin_iterations = 500,   // Spin count before blocking
    .spin_yield = true       // Yield during spin (friendlier to other threads)
};
scheduler sched(4, custom);
```

**Strategy Selection Guide:**

| Strategy | CPU Usage | Wake Latency | Use Case |
|----------|-----------|--------------|----------|
| `blocking()` | Lowest | ~1-10 μs | General workloads (default) |
| `hybrid(N)` | Low-Medium | ~1-5 μs | Latency-sensitive with mixed load |
| `spinning(N)` | High | ~100-500 ns | Ultra-low latency, dedicated CPUs |
| `aggressive(N)` | Medium-High | ~100-1000 ns | Low latency, shared CPUs |

The `spin_yield` flag controls whether the spin phase uses `std::this_thread::yield()` (true) or the CPU pause instruction (false). Yielding is friendlier to other threads but slightly slower.

**Runtime Configuration:**

```cpp
// Change per-worker strategy at runtime
auto* worker = sched.get_worker(0);
worker->set_wait_strategy(wait_strategy::spinning(2000));
```

### io_uring Batch Submit

I/O operations are automatically batched:

```cpp
// In io_uring_backend::poll()
// Auto-submit any pending operations before waiting
if (io_uring_sq_ready(&ring_) > 0) {
    io_uring_submit(&ring_);
}
```

This reduces the number of `io_uring_submit` syscalls by batching multiple operations.

### Lazy Debug ID Allocation

Debug IDs for coroutines are only allocated when actually accessed, reducing creation overhead in production:

```cpp
// debug_id_ initialized to 0, allocated on first id() call
uint64_t id() const noexcept {
    if (debug_id_.load(std::memory_order_relaxed) == 0) {
        debug_id_.store(id_allocator::allocate(), std::memory_order_relaxed);
    }
    return debug_id_.load(std::memory_order_relaxed);
}
```

### Optimized Yield Path

Yielding skips affinity checks and scheduler lookups for better performance:

```cpp
// In yield_awaitable::await_suspend()
auto* worker = runtime::worker_thread::current();
if (worker) {
    // Fast path: directly schedule to local queue
    worker->schedule_local(awaiter);
    return;
}
// Slow path only when no current worker
```

## Scheduler Tuning

### Thread Count

```cpp
#include <elio/runtime/scheduler.hpp>

// Default: matches hardware concurrency
scheduler sched;

// Custom thread count
scheduler sched(8);  // 8 worker threads

// For I/O-bound workloads, consider more threads than cores
scheduler sched(std::thread::hardware_concurrency() * 2);

// For CPU-bound workloads, match core count
scheduler sched(std::thread::hardware_concurrency());
```

### Dynamic Thread Adjustment

The scheduler supports changing the worker thread count at runtime. There is no automatic min/max scaling — you control the thread count explicitly:

```cpp
// Adjust thread count at runtime
sched.set_thread_count(8);  // Grow to 8 workers
sched.set_thread_count(2);  // Shrink to 2 workers
// Note: set_thread_count handles starting/stopping workers dynamically
```

This is useful for adapting to load changes detected by your own monitoring logic (see [Scheduler Statistics](#scheduler-statistics)).

### Thread Affinity

Pin coroutines to specific workers for cache locality:

```cpp
#include <elio/runtime/affinity.hpp>

coro::task<void> cache_sensitive_work() {
    // Bind to current worker for cache locality
    co_await elio::bind_to_current_worker();

    // All subsequent work stays on this worker
    process_data();
}

// Or set affinity to a specific worker
coro::task<void> pinned_work() {
    co_await elio::set_affinity(2);  // Bind to worker 2 and migrate there
    co_await elio::set_affinity(2, false);  // Bind without migrating

    // Later, allow free migration again
    co_await elio::clear_affinity();
}
```

`set_affinity` is an awaitable. When called with `migrate=true` (the default), the coroutine is immediately rescheduled on the target worker. With `migrate=false`, the affinity is recorded but migration is deferred until the next scheduling point. `clear_affinity` removes the binding so the task can be freely stolen by any worker again.

## I/O Backend Selection

### io_uring vs epoll

Elio auto-detects the best available backend:

```cpp
#include <elio/io/io_context.hpp>

// Auto-detect (prefers io_uring)
io::io_context ctx;

// Force specific backend
io::io_context ctx(io::io_context::backend_type::io_uring);
io::io_context ctx(io::io_context::backend_type::epoll);

// Check active backend
std::cout << "Backend: " << ctx.get_backend_name() << std::endl;
```

**Why io_uring is preferred:**
- **Submission batching.** Multiple I/O operations can be queued in the submission ring before a single `io_uring_enter` syscall, amortizing syscall overhead across many operations.
- **Completion batching.** Completions accumulate in the completion ring and can be reaped in bulk without per-operation syscalls, unlike epoll where each I/O still requires a separate `read`/`write`/`accept` call after readiness notification.
- **Registered resources.** File descriptors and buffers can be pre-registered with the kernel, reducing per-operation kernel crossing cost by avoiding repeated `fget`/`fput` and page table walks.
- **Native async semantics.** Operations are inherently asynchronous — submit and forget until completion — which aligns naturally with coroutine suspension and resumption. There is no "readiness" vs "completion" mismatch as with epoll.

**epoll fallback:**
- Works on older kernels (pre-5.1)
- Lower memory overhead (no shared ring buffers)
- Adequate for moderate workloads where per-operation syscall cost is not the bottleneck

### io_uring Kernel Requirements

For best io_uring performance:
- Linux 5.1+: Basic io_uring
- Linux 5.6+: Full features
- Linux 5.11+: Multi-shot accept

## Memory Management

### Coroutine Frame Allocator

Elio uses a thread-local pool allocator for coroutine frames:

```cpp
// Configured in frame_allocator.hpp
static constexpr size_t MAX_FRAME_SIZE = 256;  // Max pooled size
static constexpr size_t POOL_SIZE = 1024;       // Pool capacity

// Statistics (if enabled)
auto stats = coro::frame_allocator::get_stats();
std::cout << "Allocations: " << stats.allocations << std::endl;
std::cout << "Pool hits: " << stats.pool_hits << std::endl;
```

### Avoiding Allocations

Keep coroutine frames small for pool allocation:

```cpp
// Bad: Large array in coroutine frame (can't use pool)
coro::task<void> large_frame() {
    char buffer[8192];  // Too large for pool
    co_await read_data(buffer);
}

// Good: Allocate separately
coro::task<void> small_frame() {
    auto buffer = std::make_unique<char[]>(8192);
    co_await read_data(buffer.get());
}
```

## Synchronization Primitives

### Mutex Performance

Elio's mutex uses atomic fast-path for uncontended cases:

```cpp
#include <elio/sync/primitives.hpp>

sync::mutex mtx;

// Fast path: atomic CAS (~10ns)
// Slow path: suspend and queue (~100ns + context switch)

coro::task<void> critical_section() {
    co_await mtx.lock();
    // ... critical section ...
    mtx.unlock();
}

// Use try_lock to avoid blocking
if (mtx.try_lock()) {
    // Got lock immediately
    mtx.unlock();
} else {
    // Skip or retry later
}
```

### Reader-Writer Lock

For read-heavy workloads:

```cpp
sync::shared_mutex rw_mtx;

// Multiple concurrent readers (atomic counter, no blocking)
coro::task<void> reader() {
    co_await rw_mtx.lock_shared();
    auto data = read_data();
    rw_mtx.unlock_shared();
}

// Exclusive writers
coro::task<void> writer() {
    co_await rw_mtx.lock();
    write_data();
    rw_mtx.unlock();
}
```

### Channel Selection

Choose appropriate channel type:

```cpp
// Bounded channel: back-pressure, bounded memory
sync::channel<int> ch(100);

// Unbounded channel: faster but can grow indefinitely
sync::unbounded_channel<int> uch;

// SPSC queue: single producer/consumer (fastest)
runtime::spsc_queue<int> spsc(1000);
```

## Network Performance

### Connection Pooling

HTTP client uses connection pooling by default:

```cpp
http::client_config config;
config.max_connections_per_host = 10;  // Pool size per host
config.pool_idle_timeout = std::chrono::seconds(60);

http::client client(ctx, config);
```

### Buffer Sizes

Tune read buffer sizes for your workload:

```cpp
http::client_config config;
config.read_buffer_size = 16384;  // 16KB (default: 8KB)

// For large payloads
config.read_buffer_size = 65536;  // 64KB
```

### TCP Settings

Configure TCP options for performance:

```cpp
// Enable TCP_NODELAY for latency-sensitive applications
net::tcp_stream stream = /* ... */;
stream.set_nodelay(true);

// Adjust send/receive buffers
stream.set_send_buffer_size(65536);
stream.set_recv_buffer_size(65536);
```

## Profiling and Monitoring

### Scheduler Statistics

The scheduler exposes individual metric accessors rather than a single stats struct:

```cpp
// Available scheduler metrics
size_t total = sched.total_tasks_executed();     // Total across all workers
size_t w0 = sched.worker_tasks_executed(0);      // Worker 0's count
size_t pending = sched.pending_tasks();          // Currently pending tasks
size_t threads = sched.num_threads();            // Current thread count
```

These are lightweight atomic reads suitable for periodic monitoring in production. Combine with `set_thread_count` to implement your own adaptive scaling.

### Logging Overhead

Debug logging has overhead; disable in production:

```cpp
// Set at compile time
// cmake -DELIO_DEBUG=OFF ..

// Or at runtime
elio::log::set_level(elio::log::level::warning);
```

### Coroutine Stack Tracing

Use virtual stack for debugging without significant overhead:

```cpp
// Enable in debug builds only
#ifdef ELIO_DEBUG
    auto* frame = coro::current_frame();
    print_stack_trace(frame);
#endif
```

## Benchmarking Tips

### Warm-up

```cpp
// Warm up allocators and caches
for (int i = 0; i < 1000; i++) {
    warmup_task().go();
}
sched.sync();

// Now measure
auto start = std::chrono::steady_clock::now();
// ... actual benchmark ...
auto end = std::chrono::steady_clock::now();
```

### Avoid Measurement Overhead

```cpp
// Bad: timing inside hot loop
for (int i = 0; i < 1000000; i++) {
    auto start = now();  // Overhead!
    do_work();
    auto end = now();
    record(end - start);
}

// Good: time the whole batch
auto start = now();
for (int i = 0; i < 1000000; i++) {
    do_work();
}
auto end = now();
auto avg = (end - start) / 1000000;
```

### Use Release Builds

Always benchmark with optimizations:

```bash
cmake -DCMAKE_BUILD_TYPE=Release ..
cmake --build .
```

## Common Performance Issues

### Problem: High Latency Spikes

**Causes:**
- Work stealing delays
- GC pauses in other processes
- Kernel scheduling

**Solutions:**
- Pin critical tasks to workers
- Use CPU affinity for scheduler threads
- Consider real-time scheduling

### Problem: Low Throughput

**Causes:**
- Lock contention
- Inefficient I/O batching
- Small buffer sizes

**Solutions:**
- Profile lock contention
- Use io_uring for batching
- Increase buffer sizes

### Problem: High Memory Usage

**Causes:**
- Unbounded channels
- Large coroutine frames
- Connection pool growth

**Solutions:**
- Use bounded channels
- Allocate large buffers separately
- Limit connection pool size

## Running Benchmarks

Elio includes several benchmark tools:

```bash
cd build
cmake --build .

# Quick benchmark - measures spawn, context switch, yield
./quick_benchmark

# Microbenchmarks - individual operation timing
./microbench

# I/O benchmark - file read throughput
./io_benchmark

# Full benchmark suite
./benchmark

# Scalability test - multi-thread scaling
./scalability_test
```

### Interpreting Results

Benchmark results can vary significantly (min/max differ by 2-7x) due to:
- CPU frequency scaling
- System load
- Cache state
- Memory allocation patterns

Run benchmarks multiple times and use minimum values for best-case analysis.

## Quick Reference

| Scenario | Recommendation |
|----------|----------------|
| I/O-bound | 2x core count threads |
| CPU-bound | 1x core count threads |
| Latency-critical | Pin to workers, io_uring |
| Throughput-critical | Large buffers, batching |
| Memory-constrained | Bounded channels, small pools |
| Read-heavy sync | Use shared_mutex |

## See Also

- [Core Concepts](Core-Concepts.md)
- [Debugging Guide](Debugging.md)
- [HTTP/2 Guide](HTTP2-Guide.md)
