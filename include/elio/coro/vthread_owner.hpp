#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <new>
#include <vector>

namespace elio::coro {

/// Segmented owner storage for coroutine frames within one vthread domain.
///
/// This allocator prioritizes pointer stability and simple O(1) bump allocation
/// in the active segment. Individual frame deallocation is intentionally a no-op;
/// memory is reclaimed when the owner is destroyed.
class vthread_owner {
public:
    static constexpr size_t INITIAL_SEGMENT_SIZE = 4096;
    static constexpr uint64_t ALLOCATION_MAGIC = 0x564F574E4552464DULL;

    struct allocation_info {
        vthread_owner* owner = nullptr;
        bool is_root = false;
        bool found = false;
    };

    vthread_owner() {
        add_segment(INITIAL_SEGMENT_SIZE);
    }

    ~vthread_owner() {
        segment* seg = head_;
        while (seg) {
            segment* next = seg->next;
            unregister_segment(seg->data, seg->capacity);
            ::operator delete(seg->data);
            delete seg;
            seg = next;
        }
    }

    vthread_owner(const vthread_owner&) = delete;
    vthread_owner& operator=(const vthread_owner&) = delete;
    vthread_owner(vthread_owner&&) = delete;
    vthread_owner& operator=(vthread_owner&&) = delete;

    [[nodiscard]] void* allocate(size_t size,
                                 size_t alignment = alignof(std::max_align_t),
                                 bool is_root = false) {
        if (size == 0) return nullptr;
        if (alignment == 0) alignment = alignof(std::max_align_t);
        if (!current_) return nullptr;

        void* ptr = try_allocate_in_segment(current_, size, alignment, this, is_root);
        if (ptr) return ptr;

        const size_t required = size + header_size(alignment) + alignment;
        const size_t next_size = std::max(required, current_->capacity * 2);
        add_segment(next_size);
        return try_allocate_in_segment(current_, size, alignment, this, is_root);
    }

    static void mark_root_allocation(void* ptr, bool is_root) noexcept {
        auto* header = header_from_user(ptr);
        if (!header || header->magic != ALLOCATION_MAGIC) return;
        header->is_root = is_root;
    }

    [[nodiscard]] static allocation_info inspect_allocation(const void* ptr) noexcept {
        if (!is_in_registered_segment(ptr)) {
            return {};
        }

        auto* header = header_from_user(ptr);
        if (!header || header->magic != ALLOCATION_MAGIC) {
            return {};
        }

        return allocation_info{
            .owner = header->owner,
            .is_root = header->is_root,
            .found = true,
        };
    }

    [[nodiscard]] bool owns_address(const void* ptr) const noexcept {
        auto addr = reinterpret_cast<uintptr_t>(ptr);
        segment* seg = head_;
        while (seg) {
            auto begin = reinterpret_cast<uintptr_t>(seg->data);
            auto end = begin + seg->capacity;
            if (addr >= begin && addr < end) {
                return true;
            }
            seg = seg->next;
        }
        return false;
    }

private:
    struct segment_range {
        uintptr_t begin;
        uintptr_t end;
    };

    struct allocation_header {
        uint64_t magic;
        vthread_owner* owner;
        bool is_root;
    };

    struct segment {
        char* data;
        size_t capacity;
        size_t used;
        segment* next;
    };

    static size_t align_up(size_t value, size_t alignment) noexcept {
        const size_t mask = alignment - 1;
        return (value + mask) & ~mask;
    }

    static size_t header_size(size_t alignment) noexcept {
        return align_up(sizeof(allocation_header), alignment);
    }

    static allocation_header* header_from_user(const void* ptr) noexcept {
        if (!ptr) return nullptr;
        auto* bytes = static_cast<const char*>(ptr);
        auto* header = reinterpret_cast<const allocation_header*>(bytes - sizeof(allocation_header));
        return const_cast<allocation_header*>(header);
    }

    static bool is_in_registered_segment(const void* ptr) {
        if (!ptr) return false;
        const auto addr = reinterpret_cast<uintptr_t>(ptr);
        std::lock_guard<std::mutex> lock(segment_registry_mutex_);
        for (const auto& range : segment_registry_) {
            if (addr >= range.begin && addr < range.end) {
                return true;
            }
        }
        return false;
    }

    static void register_segment(const char* data, size_t capacity) {
        std::lock_guard<std::mutex> lock(segment_registry_mutex_);
        segment_registry_.push_back(segment_range{
            .begin = reinterpret_cast<uintptr_t>(data),
            .end = reinterpret_cast<uintptr_t>(data) + capacity,
        });
    }

    static void unregister_segment(const char* data, size_t capacity) {
        const auto begin = reinterpret_cast<uintptr_t>(data);
        const auto end = begin + capacity;
        std::lock_guard<std::mutex> lock(segment_registry_mutex_);
        auto it = std::remove_if(segment_registry_.begin(), segment_registry_.end(),
                                 [&](const segment_range& range) {
                                     return range.begin == begin && range.end == end;
                                 });
        segment_registry_.erase(it, segment_registry_.end());
    }

    static void* try_allocate_in_segment(segment* seg,
                                         size_t size,
                                         size_t alignment,
                                         vthread_owner* owner,
                                         bool is_root) noexcept {
        const size_t header = header_size(alignment);
        const size_t offset = align_up(seg->used + header, alignment);
        if (offset + size > seg->capacity) {
            return nullptr;
        }

        auto* allocation = reinterpret_cast<allocation_header*>(seg->data + offset - sizeof(allocation_header));
        allocation->magic = ALLOCATION_MAGIC;
        allocation->owner = owner;
        allocation->is_root = is_root;

        void* ptr = seg->data + offset;
        seg->used = offset + size;
        return ptr;
    }

    void add_segment(size_t capacity) {
        auto* seg = new segment{
            .data = static_cast<char*>(::operator new(capacity)),
            .capacity = capacity,
            .used = 0,
            .next = nullptr,
        };
        register_segment(seg->data, seg->capacity);

        if (!head_) {
            head_ = seg;
            current_ = seg;
            return;
        }

        current_->next = seg;
        current_ = seg;
    }

    segment* head_ = nullptr;
    segment* current_ = nullptr;

    static inline std::mutex segment_registry_mutex_{};
    static inline std::vector<segment_range> segment_registry_{};
};

} // namespace elio::coro
