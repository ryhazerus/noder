//
// Created by zahyrseferina on 10/9/26.
//

#include "store.h"

#include <chrono>

namespace {
    // Active expiry, same idea as Redis: every 100ms sample 20 random keys that have a TTL,
    // delete the expired ones, and sample again while more than 25% of them were expired.
    constexpr auto kExpireInterval = std::chrono::milliseconds(100);
    constexpr auto kExpireTimeBudget = std::chrono::milliseconds(10);   // max work per round
    constexpr size_t kExpireSampleSize = 20;
}

iron::Ttl iron::Ttl::in_ms(int64_t ms) {
    return {Mode::ExpireAt, store::now_ms() + ms};
}

iron::store::store() {
    for (Shard &shard : shards_) {
        shard.map.reserve(100000 / kShards);
    }
    expirer_ = std::jthread([this](std::stop_token stop) { run_expirer(stop); });
}

iron::store::~store() {
    // std::jthread's destructor would do this too, but the shards must still exist while it stops
    expirer_.request_stop();
    if (expirer_.joinable()) {
        expirer_.join();
    }
}

int64_t iron::store::now_ms() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

iron::store::Shard &iron::store::shard_for(std::string_view key) {
    const size_t hash = std::hash<std::string_view>{}(key);
    // take the shard from the high bits; the map inside the shard picks its bucket from the
    // same hash (hash % bucket_count), and this keeps those two choices independent
    return shards_[(hash >> 32) & (kShards - 1)];
}

const iron::store::Shard &iron::store::shard_for(std::string_view key) const {
    return const_cast<store *>(this)->shard_for(key);
}

// ---- keeping Shard::expiring in sync (callers hold the shard's write lock) ----

void iron::store::track(Shard &shard, node_t &node) {
    if (node.second.expiringIndex != kNotExpiring) return;
    node.second.expiringIndex = static_cast<uint32_t>(shard.expiring.size());
    shard.expiring.push_back(&node);
}

void iron::store::untrack(Shard &shard, node_t &node) {
    const uint32_t index = node.second.expiringIndex;
    if (index == kNotExpiring) return;
    // O(1) removal: move the last element into the hole
    node_t *last = shard.expiring.back();
    shard.expiring[index] = last;
    last->second.expiringIndex = index;
    shard.expiring.pop_back();
    node.second.expiringIndex = kNotExpiring;
}

void iron::store::erase(Shard &shard, map_t::iterator it) {
    untrack(shard, *it);
    shard.map.erase(it);
}

void iron::store::apply_ttl(Shard &shard, node_t &node, Ttl ttl) {
    switch (ttl.mode) {
        case Ttl::Mode::Keep:
            break;
        case Ttl::Mode::Clear:
            untrack(shard, node);
            node.second.expiresAtMs = 0;
            break;
        case Ttl::Mode::ExpireAt:
            node.second.expiresAtMs = ttl.expiresAtMs;
            track(shard, node);
            break;
    }
}

// ---- operations ----

void iron::store::add_record(std::string_view key, std::string_view value, Ttl ttl) {
    Shard &shard = shard_for(key);
    std::unique_lock lock(shard.mutex);
    auto it = shard.map.find(key);
    if (it != shard.map.end()) {
        // an expired key doesn't exist anymore, so there's no TTL to keep
        if (ttl.mode == Ttl::Mode::Keep && is_expired(it->second)) ttl = Ttl::clear();
        it->second.value.assign(value);   // reuses the existing string's memory when it fits
    } else {
        it = shard.map.emplace(std::string(key), Entry{std::string(value)}).first;
    }
    apply_ttl(shard, *it, ttl);
}

bool iron::store::delete_record(std::string_view key) {
    Shard &shard = shard_for(key);
    std::unique_lock lock(shard.mutex);
    const auto it = shard.map.find(key);
    if (it == shard.map.end()) {
        return false;
    }
    const bool existed = !is_expired(it->second);
    erase(shard, it);
    return existed;
}

bool iron::store::contains(std::string_view key) const {
    return read_record(key, [](std::string_view) {});
}

std::optional<std::string> iron::store::get_record(std::string_view key) const {
    std::optional<std::string> result;
    read_record(key, [&](std::string_view value) { result.emplace(value); });
    return result;
}

bool iron::store::expire_in(std::string_view key, int64_t ms) {
    Shard &shard = shard_for(key);
    std::unique_lock lock(shard.mutex);
    const auto it = shard.map.find(key);
    if (it == shard.map.end()) {
        return false;
    }
    if (is_expired(it->second)) {
        erase(shard, it);
        return false;
    }
    if (ms <= 0) {
        erase(shard, it);   // like Redis: a TTL in the past deletes the key
        return true;
    }
    apply_ttl(shard, *it, Ttl::in_ms(ms));
    return true;
}

bool iron::store::persist(std::string_view key) {
    Shard &shard = shard_for(key);
    std::unique_lock lock(shard.mutex);
    const auto it = shard.map.find(key);
    if (it == shard.map.end()) {
        return false;
    }
    if (is_expired(it->second)) {
        erase(shard, it);
        return false;
    }
    if (it->second.expiresAtMs == 0) {
        return false;
    }
    apply_ttl(shard, *it, Ttl::clear());
    return true;
}

int64_t iron::store::ttl_ms(std::string_view key) const {
    const Shard &shard = shard_for(key);
    std::shared_lock lock(shard.mutex);
    const auto it = shard.map.find(key);
    if (it == shard.map.end()) {
        return -2;
    }
    if (it->second.expiresAtMs == 0) {
        return -1;
    }
    const int64_t remaining = it->second.expiresAtMs - now_ms();
    return remaining > 0 ? remaining : -2;
}

size_t iron::store::size() const {
    size_t total = 0;
    for (const Shard &shard : shards_) {
        std::shared_lock lock(shard.mutex);
        total += shard.map.size();
    }
    return total;
}

void iron::store::clear() {
    for (Shard &shard : shards_) {
        std::unique_lock lock(shard.mutex);
        shard.expiring.clear();
        shard.map.clear();
    }
}

// ---- background expiry ----

void iron::store::run_expirer(std::stop_token stop) {
    std::mt19937_64 random(std::random_device{}());
    std::unique_lock lock(expirerMutex_);
    while (!stop.stop_requested()) {
        // sleeps for the interval, but wakes up right away when the store is destroyed
        expirerWakeup_.wait_for(lock, stop, kExpireInterval, [] { return false; });
        if (stop.stop_requested()) break;
        expire_cycle(random);
    }
}

void iron::store::expire_cycle(std::mt19937_64 &random) {
    const auto deadline = std::chrono::steady_clock::now() + kExpireTimeBudget;

    // continue where the previous round ran out of time, so every shard gets its turn
    for (size_t visited = 0; visited < kShards; ++visited) {
        Shard &shard = shards_[expirerNextShard_];
        expirerNextShard_ = (expirerNextShard_ + 1) % kShards;

        while (true) {
            size_t sampled = 0;
            size_t expired = 0;
            {
                // the lock is held for one small batch only, so workers wait microseconds at most
                std::unique_lock lock(shard.mutex);
                const int64_t now = now_ms();
                for (; sampled < kExpireSampleSize && !shard.expiring.empty(); ++sampled) {
                    node_t *node = shard.expiring[random() % shard.expiring.size()];
                    if (node->second.expiresAtMs <= now) {
                        erase(shard, shard.map.find(node->first));
                        ++expired;
                    }
                }
            }

            if (std::chrono::steady_clock::now() >= deadline) return;
            // few expired keys in the sample: probably not many more, move on to the next shard
            if (sampled < kExpireSampleSize || expired * 4 <= sampled) break;
        }
    }
}
