//
// Created by zahyrseferina on 10/9/26.
//

#ifndef NODE_CONNECTOR_STORE_H
#define NODE_CONNECTOR_STORE_H
#include <array>
#include <condition_variable>
#include <cstdint>
#include <mutex>
#include <optional>
#include <random>
#include <shared_mutex>
#include <stop_token>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

namespace iron {
    // lets find/erase take a string_view without building a temporary std::string
    struct string_hash {
        using is_transparent = void;
        size_t operator()(std::string_view sv) const noexcept { return std::hash<std::string_view>{}(sv); }
    };

    // what a write does with the key's time-to-live
    struct Ttl {
        enum class Mode {
            Clear,      // key becomes permanent (the default, same as a Redis SET without options)
            Keep,       // keep whatever TTL the key already had (Redis' KEEPTTL)
            ExpireAt,   // expire at expiresAtMs
        };

        Mode mode = Mode::Clear;
        int64_t expiresAtMs = 0;

        static Ttl clear() { return {}; }
        static Ttl keep() { return {Mode::Keep, 0}; }
        static Ttl in_ms(int64_t ms);
    };

    /**
     * Thread-safe key-value store, split into shards that each have their own lock.
     * Worker threads touching different keys usually hit different shards, so they
     * don't wait on (or bounce the cache line of) one shared mutex.
     *
     * Keys can have a TTL. Expired keys are removed in two ways, like Redis does:
     *  - lazily: reads treat an expired key as missing straight away
     *  - actively: a background thread samples keys that have a TTL and deletes the expired ones,
     *    so keys that are never read again don't stay in memory forever
     */
    class store {
    public:
        static constexpr size_t kShards = 64;   // power of two, see shard_for
        static constexpr int64_t kMaxTtlMs = 100LL * 365 * 24 * 3600 * 1000;   // ~100 years, avoids overflow

        store();

        ~store();

        store(const store &) = delete;
        store &operator=(const store &) = delete;

        // milliseconds on a monotonic clock: unaffected by changes to the system time
        static int64_t now_ms();

        void add_record(std::string_view key, std::string_view value, Ttl ttl = Ttl::clear());

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
            if (it == shard.map.end() || is_expired(it->second)) {
                return false;
            }
            fn(std::string_view(it->second.value));
            return true;
        }

        // sets the key to expire `ms` from now (ms <= 0 deletes it); false if the key doesn't exist
        bool expire_in(std::string_view key, int64_t ms);

        // removes the key's TTL; false if the key doesn't exist or had no TTL
        bool persist(std::string_view key);

        // remaining time to live in ms, -1 if the key has no TTL, -2 if it doesn't exist (same as Redis' PTTL)
        int64_t ttl_ms(std::string_view key) const;

        // includes expired keys the background thread hasn't removed yet (Redis' DBSIZE does the same)
        size_t size() const;

        void clear();

    private:
        static constexpr uint32_t kNotExpiring = UINT32_MAX;

        struct Entry {
            std::string value;
            int64_t expiresAtMs = 0;                  // 0: no TTL
            uint32_t expiringIndex = kNotExpiring;    // position in Shard::expiring
        };

        using map_t = std::unordered_map<std::string, Entry, string_hash, std::equal_to<>>;
        using node_t = map_t::value_type;

        // alignas(64): each shard's mutex gets its own cache line, otherwise threads locking
        // neighbouring shards would still slow each other down (false sharing)
        struct alignas(64) Shard {
            mutable std::shared_mutex mutex;
            map_t map;
            // only the keys that have a TTL, so the expirer never looks at permanent keys.
            // Pointers to unordered_map elements stay valid when the map grows, unlike iterators.
            std::vector<node_t *> expiring;
        };

        std::array<Shard, kShards> shards_;

        // background expiry; declared last so the thread is stopped before the shards are destroyed
        std::mutex expirerMutex_;
        std::condition_variable_any expirerWakeup_;
        size_t expirerNextShard_ = 0;
        std::jthread expirer_;

        Shard &shard_for(std::string_view key);
        const Shard &shard_for(std::string_view key) const;

        static bool is_expired(const Entry &entry) {
            return entry.expiresAtMs != 0 && entry.expiresAtMs <= now_ms();
        }

        static void apply_ttl(Shard &shard, node_t &node, Ttl ttl);
        static void track(Shard &shard, node_t &node);
        static void untrack(Shard &shard, node_t &node);
        static void erase(Shard &shard, map_t::iterator it);

        void run_expirer(std::stop_token stop);
        void expire_cycle(std::mt19937_64 &random);
    };
}


#endif //NODE_CONNECTOR_STORE_H
