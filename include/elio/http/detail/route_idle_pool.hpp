#pragma once

#include <elio/http/detail/route_plan.hpp>

#include <array>
#include <chrono>
#include <deque>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <utility>
#include <vector>

namespace elio::http::detail {

// Legacy opt-out retention for private route streams. Live admission remains
// unbounded here; finite admission uses bounded_pool and its owned permits.
template<typename Stream>
class route_idle_pool {
    using pool_map = std::unordered_map<connection_key, std::deque<Stream>, connection_key_hash>;
    struct shard {
        std::mutex mutex;
        pool_map entries;
    };
public:
    static constexpr size_t shard_count = 16;
    using detached = std::array<pool_map, shard_count>;

    route_idle_pool(size_t max_idle_per_route, std::chrono::seconds idle_timeout)
        : max_idle_per_route_(max_idle_per_route), idle_timeout_(idle_timeout) {}

    std::optional<Stream> take(const connection_key& key) {
        std::vector<Stream> retired;
        std::optional<Stream> result;
        Stream candidate;
        auto& selected = shard_for(key);
        {
            std::lock_guard lock(selected.mutex);
            auto found = selected.entries.find(key);
            if (found == selected.entries.end()) return {};
            while (!found->second.empty()) {
                candidate = std::move(found->second.front());
                found->second.pop_front();
                if (std::chrono::steady_clock::now() - candidate.last_use() < idle_timeout_) {
                    candidate.touch();
                    result = std::move(candidate);
                    break;
                }
                retired.push_back(std::move(candidate));
            }
            if (found->second.empty()) selected.entries.erase(found);
        }
        return result;
    }

    bool retain(const connection_key& key, Stream& stream) {
        if (max_idle_per_route_ == 0) return false;
        auto& selected = shard_for(key);
        std::lock_guard lock(selected.mutex);
        auto& entries = selected.entries[key];
        if (entries.size() >= max_idle_per_route_) return false;
        stream.touch();
        entries.push_back(std::move(stream));
        return true;
    }

    detached detach() {
        detached result;
        for (size_t i = 0; i < shard_count; ++i) {
            std::lock_guard lock(shards_[i].mutex);
            result[i].swap(shards_[i].entries);
        }
        return result;
    }

private:
    shard& shard_for(const connection_key& key) noexcept {
        return shards_[connection_key_hash{}(key) % shard_count];
    }
    size_t max_idle_per_route_;
    std::chrono::seconds idle_timeout_;
    std::array<shard, shard_count> shards_;
};

} // namespace elio::http::detail
