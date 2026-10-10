//
// Created by zahyrseferina on 10/9/26.
//

#ifndef NODE_CONNECTOR_STORE_H
#define NODE_CONNECTOR_STORE_H
#include <array>
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <unordered_map>

namespace iron {
    // lets find/erase take a string_view without building a temporary std::string
    struct string_hash {
        using is_transparent = void;
        size_t operator()(std::string_view sv) const noexcept { return std::hash<std::string_view>{}(sv); }
    };

    /**
     * Thread-safe key-value store, split into shards that each have their own lock.
     * Worker threads touching different keys usually hit different shards, so they
     * don't wait on (or bounce the cache line of) one shared mutex.
     */
    class store {
    public:
        static constexpr size_t kShards = 64;   // power of two, see shard_for

        store();

        void add_record(std::string_view key, std::string_view value);

        // returns true when the key existed
        bool delete_record(std::string_view key);

        bool contains(std::string_view key) const;

        std::optional<std::string> get_record(std::string_view key) const;

        // Calls fn(std::string_view value) while the shard is read-locked, so the value can be
        // written straight into a reply without copying it first. Returns false if the key is missing.
        // Keep fn short: writers to this shard wait until it returns.
        template<typename Fn>
        bool read_record(std::string_view key, Fn &&fn) const {
            const Shard &shard = shard_for(key);
            std::shared_lock lock(shard.mutex);
            const auto it = shard.map.find(key);
            if (it == shard.map.end()) {
                return false;
            }
            fn(std::string_view(it->second));
            return true;
        }

        size_t size() const;

        void clear();

    private:
        using map_t = std::unordered_map<std::string, std::string, string_hash, std::equal_to<>>;

        // alignas(64): each shard's mutex gets its own cache line, otherwise threads locking
        // neighbouring shards would still slow each other down (false sharing)
        struct alignas(64) Shard {
            mutable std::shared_mutex mutex;
            map_t map;
        };

        std::array<Shard, kShards> shards_;

        Shard &shard_for(std::string_view key);
        const Shard &shard_for(std::string_view key) const;
    };
}


#endif //NODE_CONNECTOR_STORE_H
