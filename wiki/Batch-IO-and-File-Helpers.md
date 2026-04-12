# Batch I/O and File Helpers

Elio provides high-level file I/O abstractions built on top of io_uring for maximum throughput.

## Batch I/O

Batch I/O allows you to submit multiple `pread`/`pwrite` operations in a **single syscall** (`io_uring_submit()`), reducing kernel round-trip overhead and improving throughput for scattered reads/writes.

### Batch Read

```cpp
#include <elio/elio.hpp>

using namespace elio::io;

coro::task<void> read_multiple_segments(int fd) {
    char buf1[16] = {0}, buf2[16] = {0}, buf3[16] = {0};

    std::array<batch_read_segment, 3> segments;
    segments[0] = {0, buf1, 5};      // Read 5 bytes at offset 0
    segments[1] = {10, buf2, 5};     // Read 5 bytes at offset 10
    segments[2] = {20, buf3, 5};     // Read 5 bytes at offset 20

    auto results = co_await batch_read(fd, segments);

    for (size_t i = 0; i < results.size(); ++i) {
        if (results[i] > 0) {
            // Success: bytes read
        } else {
            // Error: negative errno
        }
    }
}
```

### Batch Write

```cpp
coro::task<void> write_multiple_segments(int fd) {
    const char* data1 = "Hello";
    const char* data2 = "World";
    const char* data3 = "!";

    std::array<batch_write_segment, 3> segments;
    segments[0] = {0, data1, 5};     // Write at offset 0
    segments[1] = {10, data2, 5};    // Write at offset 10
    segments[2] = {20, data3, 1};    // Write at offset 20

    auto results = co_await batch_write(fd, segments);
}
```

### Design

- **Single syscall submission**: All segments are prepared in the SQ ring and submitted together
- **Per-segment completion tracking**: Each segment gets its own result (bytes read/written, or negative errno)
- **Tagged user_data**: The completion queue encodes both the batch state pointer and segment index, allowing out-of-order CQEs to be matched correctly
- **Epoll fallback**: When io_uring is unavailable, falls back to sequential synchronous `pread`/`pwrite`

### Limitations

- Segment offsets must not overlap (undefined behavior if they do)
- Maximum segments per batch is limited by the io_uring SQ ring depth
- If the SQ ring is full mid-batch, remaining segments get `-EAGAIN`

## File Helpers

High-level coroutine functions for common file operations:

### Reading a File

```cpp
auto content = co_await read_file("/path/to/file.txt");
if (content) {
    std::cout << *content << std::endl;
} else {
    std::cerr << "Failed to read file" << std::endl;
}
```

### Writing a File

```cpp
bool ok = co_await write_file("/path/to/file.txt", "Hello, World!");
```

### Appending to a File

```cpp
bool ok = co_await append_file("/path/to/log.txt", "New log entry\n");
```

### File Metadata

```cpp
// Check if file exists
if (file_exists("/path/to/file.txt")) {
    std::cout << "File exists" << std::endl;
}

// Get file size
if (auto size = file_size("/path/to/file.txt")) {
    std::cout << "Size: " << *size << " bytes" << std::endl;
}
```

### Reading Directory

```cpp
auto entries = read_dir("/path/to/directory");
if (entries) {
    for (const auto& entry : *entries) {
        if (entry.is_dir) {
            std::cout << "[DIR]  " << entry.name << std::endl;
        } else if (entry.is_file) {
            std::cout << "[FILE] " << entry.name << std::endl;
        } else if (entry.is_symlink) {
            std::cout << "[LINK] " << entry.name << std::endl;
        }
    }
}
```

### Chunking Strategy

Large files are read/written in **1MB chunks** to avoid excessive memory allocation in a single io_uring request. The chunking is transparent — the caller sees the entire file content.
