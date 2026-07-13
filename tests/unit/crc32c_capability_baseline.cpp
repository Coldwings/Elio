#include <elio/hash/crc32.hpp>

using crc32c_fn = uint32_t (*)(const void*, size_t, uint32_t) noexcept;
using crc32c_capability_fn = bool (*)() noexcept;

extern "C" crc32c_fn elio_test_crc32c_baseline_fn() noexcept {
    return static_cast<crc32c_fn>(&elio::hash::crc32c);
}

extern "C" crc32c_capability_fn
elio_test_crc32c_baseline_capability_fn() noexcept {
    return &elio::hash::crc32c_hw_available;
}
