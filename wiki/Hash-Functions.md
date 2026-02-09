# Hash Functions

Elio provides a set of hash and checksum functions for data integrity verification and cryptographic hashing. All implementations are header-only with no external dependencies.

## Features

- **CRC32**: Fast checksum for data integrity verification
- **SHA-1**: Cryptographic hash (legacy, use SHA-256 for security-sensitive applications)
- **SHA-256**: Strong cryptographic hash from the SHA-2 family
- **Incremental hashing**: All hash functions support streaming/incremental computation
- **Zero dependencies**: Pure C++ implementations, no OpenSSL required

## Quick Start

```cpp
#include <elio/hash/hash.hpp>

using namespace elio::hash;

// CRC32 checksum
uint32_t checksum = crc32("Hello, World!", 13);

// SHA-1 hash
std::string sha1_result = sha1_hex("Hello, World!");
// Result: "0a0a9f2a6772942557ab5355d76af442f8f65e01"

// SHA-256 hash
std::string sha256_result = sha256_hex("Hello, World!");
// Result: "dffd6021bb2bd5b0af676290809ec3a53191dd81c7f70a4b28688a362182986f"
```

## CRC32

CRC32 uses the IEEE polynomial (0xEDB88320) for computing checksums. It's fast and suitable for data integrity verification but not for cryptographic purposes.

### Basic Usage

```cpp
#include <elio/hash/crc32.hpp>

using namespace elio::hash;

// Compute CRC32 of a buffer
const char* data = "123456789";
uint32_t checksum = crc32(data, 9);
// Result: 0xCBF43926

// Compute CRC32 of a span
std::vector<uint8_t> bytes = {1, 2, 3, 4, 5};
uint32_t checksum2 = crc32(std::span<const uint8_t>(bytes));
```

### Scatter-Gather I/O

```cpp
// CRC32 over multiple iovec buffers
const char* part1 = "Hello";
const char* part2 = "World";

struct iovec iov[2];
iov[0].iov_base = const_cast<char*>(part1);
iov[0].iov_len = 5;
iov[1].iov_base = const_cast<char*>(part2);
iov[1].iov_len = 5;

uint32_t checksum = crc32_iovec(iov, 2);
```

### Incremental Computation

```cpp
// Stream-based CRC computation
uint32_t crc = 0xFFFFFFFF;  // Initial state
crc = crc32_update(data1, len1, crc);
crc = crc32_update(data2, len2, crc);
crc = crc32_update(data3, len3, crc);
uint32_t final_checksum = crc32_finalize(crc);
```

## SHA-1

SHA-1 produces a 160-bit (20-byte) hash. While SHA-1 is considered cryptographically weak for signatures and certificates, it remains useful for non-security applications like content addressing.

### Basic Usage

```cpp
#include <elio/hash/sha1.hpp>

using namespace elio::hash;

// Compute SHA-1 hash
sha1_digest digest = sha1("Hello, World!");

// Get hex string representation
std::string hex = sha1_hex("Hello, World!");

// Or compute directly to hex
std::string hex2 = sha1_hex(data, length);
```

### Incremental Hashing

```cpp
sha1_context ctx;

// Update with multiple chunks
ctx.update("Hello, ");
ctx.update("World!");

// Finalize to get digest
sha1_digest digest = ctx.finalize();

// Reset for reuse
ctx.reset();
ctx.update("New data");
sha1_digest digest2 = ctx.finalize();
```

## SHA-256

SHA-256 produces a 256-bit (32-byte) hash and is part of the SHA-2 family. It provides strong security and is recommended for cryptographic applications.

### Basic Usage

```cpp
#include <elio/hash/sha256.hpp>

using namespace elio::hash;

// Compute SHA-256 hash
sha256_digest digest = sha256("Hello, World!");

// Get hex string representation
std::string hex = sha256_hex("Hello, World!");

// From raw bytes
std::vector<uint8_t> data = {...};
sha256_digest digest2 = sha256(std::span<const uint8_t>(data));
```

### Incremental Hashing

```cpp
sha256_context ctx;

// Stream data in chunks
while (has_more_data()) {
    auto chunk = read_chunk();
    ctx.update(chunk.data(), chunk.size());
}

sha256_digest digest = ctx.finalize();
```

### File Hashing Example

```cpp
#include <elio/hash/sha256.hpp>
#include <fstream>

std::string hash_file(const std::string& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) return "";
    
    elio::hash::sha256_context ctx;
    char buffer[8192];
    
    while (file.read(buffer, sizeof(buffer)) || file.gcount() > 0) {
        ctx.update(buffer, file.gcount());
    }
    
    return elio::hash::sha256_hex(ctx.finalize());
}
```

## Utility Functions

### Hex Conversion

```cpp
#include <elio/hash/hash.hpp>

// Convert any digest array to hex string
std::array<uint8_t, 32> digest = {...};
std::string hex = elio::hash::to_hex(digest);

// Convert raw bytes to hex
std::string hex2 = elio::hash::to_hex(data, length);
```

## Constants

| Constant | Value | Description |
|----------|-------|-------------|
| `sha1_digest_size` | 20 | SHA-1 output size in bytes |
| `sha1_block_size` | 64 | SHA-1 internal block size |
| `sha256_digest_size` | 32 | SHA-256 output size in bytes |
| `sha256_block_size` | 64 | SHA-256 internal block size |

## Implementation Notes

**CRC32** uses hardware acceleration when available. On x86/x86-64, this means the PCLMULQDQ instruction and SSE4.2 CRC32 instruction; on ARM64, it uses the ACLE CRC intrinsics. When hardware support is not detected at runtime, the implementation falls back to a slicing-by-8 lookup table approach. In addition to the standard IEEE polynomial, the library also provides CRC32C (Castagnoli polynomial) via the `crc32c()` function. Hardware availability for CRC32C can be checked with `crc32c_hw_available()`.

**SHA-1 and SHA-256** are pure C++ implementations with no external dependencies. They do not rely on OpenSSL or any other cryptographic library. SHA-1 is included primarily for WebSocket handshakes (RFC 6455 requires it), while SHA-256 is suitable for data integrity verification and content addressing.

## Performance Considerations

- **CRC32**: Very fast, suitable for checksums on large data
- **SHA-1**: Faster than SHA-256, use when security isn't critical
- **SHA-256**: More secure but slower, recommended for security applications

All implementations are optimized for clarity and correctness. For maximum performance on large data, consider using hardware-accelerated implementations (e.g., OpenSSL).

## Thread Safety

- All standalone functions (`crc32`, `sha1`, `sha256`, etc.) are thread-safe
- Context objects (`sha1_context`, `sha256_context`) are NOT thread-safe; use separate instances per thread

## Integration with RPC

The hash module is used by the RPC framework for message integrity:

```cpp
#include <elio/rpc/rpc.hpp>

// CRC32 is available via the rpc namespace for convenience
using namespace elio::rpc;
uint32_t checksum = crc32(data, length);

// Enable checksums in RPC messages
auto [header, payload] = build_request(req_id, method_id, request, timeout, true);
```
