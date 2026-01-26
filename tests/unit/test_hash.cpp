#include <catch2/catch_test_macros.hpp>
#include <elio/hash/hash.hpp>

using namespace elio::hash;

// ============================================================================
// CRC32 tests
// ============================================================================

TEST_CASE("crc32 basic", "[hash][crc32]") {
    SECTION("empty buffer") {
        uint32_t crc = crc32(nullptr, 0);
        REQUIRE(crc == 0);
    }
    
    SECTION("simple data - standard test vector") {
        // Standard CRC32 test: "123456789" should produce 0xCBF43926
        const char* data = "123456789";
        uint32_t crc = crc32(data, 9);
        REQUIRE(crc == 0xCBF43926);
    }
    
    SECTION("span overload") {
        std::vector<uint8_t> data = {'1', '2', '3', '4', '5', '6', '7', '8', '9'};
        uint32_t crc = crc32(std::span<const uint8_t>(data));
        REQUIRE(crc == 0xCBF43926);
    }
    
    SECTION("single byte") {
        uint8_t byte = 'a';
        uint32_t crc = crc32(&byte, 1);
        REQUIRE(crc == 0xE8B7BE43);
    }
}

TEST_CASE("crc32_iovec", "[hash][crc32]") {
    // Split "123456789" across multiple iovecs
    const char* part1 = "12345";
    const char* part2 = "6789";
    
    struct iovec iov[2];
    iov[0].iov_base = const_cast<char*>(part1);
    iov[0].iov_len = 5;
    iov[1].iov_base = const_cast<char*>(part2);
    iov[1].iov_len = 4;
    
    uint32_t crc = crc32_iovec(iov, 2);
    REQUIRE(crc == 0xCBF43926);
}

TEST_CASE("crc32 incremental", "[hash][crc32]") {
    // Test incremental CRC computation
    const char* data = "123456789";
    
    uint32_t crc = 0xFFFFFFFF;
    crc = crc32_update(data, 5, crc);      // "12345"
    crc = crc32_update(data + 5, 4, crc);  // "6789"
    uint32_t result = crc32_finalize(crc);
    
    REQUIRE(result == 0xCBF43926);
}

// ============================================================================
// SHA-1 tests
// ============================================================================

TEST_CASE("sha1 basic", "[hash][sha1]") {
    SECTION("empty string") {
        // SHA1("") = da39a3ee5e6b4b0d3255bfef95601890afd80709
        auto digest = sha1("", 0);
        std::string hex = sha1_hex(digest);
        REQUIRE(hex == "da39a3ee5e6b4b0d3255bfef95601890afd80709");
    }
    
    SECTION("simple string") {
        // SHA1("abc") = a9993e364706816aba3e25717850c26c9cd0d89d
        auto digest = sha1("abc");
        std::string hex = sha1_hex(digest);
        REQUIRE(hex == "a9993e364706816aba3e25717850c26c9cd0d89d");
    }
    
    SECTION("longer string") {
        // SHA1("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq")
        // = 84983e441c3bd26ebaae4aa1f95129e5e54670f1
        const char* input = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
        auto digest = sha1(input);
        std::string hex = sha1_hex(digest);
        REQUIRE(hex == "84983e441c3bd26ebaae4aa1f95129e5e54670f1");
    }
    
    SECTION("convenience function") {
        std::string hex = sha1_hex("abc");
        REQUIRE(hex == "a9993e364706816aba3e25717850c26c9cd0d89d");
    }
}

TEST_CASE("sha1 incremental", "[hash][sha1]") {
    sha1_context ctx;
    ctx.update("abc");
    auto digest = ctx.finalize();
    
    std::string hex = sha1_hex(digest);
    REQUIRE(hex == "a9993e364706816aba3e25717850c26c9cd0d89d");
}

TEST_CASE("sha1 multi-block", "[hash][sha1]") {
    // Test with data spanning multiple blocks (block size = 64 bytes)
    std::string data(1000, 'a');  // 1000 'a' characters
    
    // SHA1 of 1000 'a's
    auto digest = sha1(data);
    std::string hex = sha1_hex(digest);
    REQUIRE(hex == "291e9a6c66994949b57ba5e650361e98fc36b1ba");
}

TEST_CASE("sha1 incremental multi-update", "[hash][sha1]") {
    sha1_context ctx;
    
    // Split "abc" into multiple updates
    ctx.update("a");
    ctx.update("b");
    ctx.update("c");
    
    auto digest = ctx.finalize();
    std::string hex = sha1_hex(digest);
    REQUIRE(hex == "a9993e364706816aba3e25717850c26c9cd0d89d");
}

// ============================================================================
// SHA-256 tests
// ============================================================================

TEST_CASE("sha256 basic", "[hash][sha256]") {
    SECTION("empty string") {
        // SHA256("") = e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855
        auto digest = sha256("", 0);
        std::string hex = sha256_hex(digest);
        REQUIRE(hex == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");
    }
    
    SECTION("simple string") {
        // SHA256("abc") = ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad
        auto digest = sha256("abc");
        std::string hex = sha256_hex(digest);
        REQUIRE(hex == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    }
    
    SECTION("longer string") {
        // SHA256("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq")
        // = 248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1
        const char* input = "abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq";
        auto digest = sha256(input);
        std::string hex = sha256_hex(digest);
        REQUIRE(hex == "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1");
    }
    
    SECTION("convenience function") {
        std::string hex = sha256_hex("abc");
        REQUIRE(hex == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
    }
}

TEST_CASE("sha256 incremental", "[hash][sha256]") {
    sha256_context ctx;
    ctx.update("abc");
    auto digest = ctx.finalize();
    
    std::string hex = sha256_hex(digest);
    REQUIRE(hex == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
}

TEST_CASE("sha256 multi-block", "[hash][sha256]") {
    // Test with data spanning multiple blocks (block size = 64 bytes)
    std::string data(1000, 'a');  // 1000 'a' characters
    
    // SHA256 of 1000 'a's
    auto digest = sha256(data);
    std::string hex = sha256_hex(digest);
    REQUIRE(hex == "41edece42d63e8d9bf515a9ba6932e1c20cbc9f5a5d134645adb5db1b9737ea3");
}

TEST_CASE("sha256 incremental multi-update", "[hash][sha256]") {
    sha256_context ctx;
    
    // Split "abc" into multiple updates
    ctx.update("a");
    ctx.update("b");
    ctx.update("c");
    
    auto digest = ctx.finalize();
    std::string hex = sha256_hex(digest);
    REQUIRE(hex == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad");
}

TEST_CASE("sha256 reset", "[hash][sha256]") {
    sha256_context ctx;
    
    // First hash
    ctx.update("abc");
    auto digest1 = ctx.finalize();
    
    // Reset and hash again
    ctx.reset();
    ctx.update("abc");
    auto digest2 = ctx.finalize();
    
    REQUIRE(digest1 == digest2);
}

// ============================================================================
// Utility function tests
// ============================================================================

TEST_CASE("to_hex utility", "[hash][util]") {
    SECTION("array to hex") {
        std::array<uint8_t, 4> data = {0xDE, 0xAD, 0xBE, 0xEF};
        std::string hex = to_hex(data);
        REQUIRE(hex == "deadbeef");
    }
    
    SECTION("raw bytes to hex") {
        uint8_t data[] = {0x01, 0x23, 0x45, 0x67, 0x89, 0xAB, 0xCD, 0xEF};
        std::string hex = to_hex(data, 8);
        REQUIRE(hex == "0123456789abcdef");
    }
    
    SECTION("empty data") {
        std::string hex = to_hex(nullptr, 0);
        REQUIRE(hex.empty());
    }
}

// ============================================================================
// Digest size tests
// ============================================================================

TEST_CASE("digest sizes", "[hash]") {
    REQUIRE(sha1_digest_size == 20);
    REQUIRE(sha256_digest_size == 32);
    REQUIRE(sha1_block_size == 64);
    REQUIRE(sha256_block_size == 64);
}
