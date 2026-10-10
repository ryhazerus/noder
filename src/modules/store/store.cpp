//
// Created by zahyrseferina on 10/9/26.
//

#include "store.h"

iron::store::store() {
    for (Shard &shard : shards_) {
        shard.map.reserve(100000 / kShards);
    }
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

void iron::store::add_record(std::string_view key, std::string_view value) {
    Shard &shard = shard_for(key);
    std::unique_lock lock(shard.mutex);
    const auto it = shard.map.find(key);
    if (it != shard.map.end()) {
        it->second.assign(value);   // reuses the existing string's memory when it fits
    } else {
        shard.map.emplace(std::string(key), std::string(value));
    }
}

bool iron::store::delete_record(std::string_view key) {
    Shard &shard = shard_for(key);
    std::unique_lock lock(shard.mutex);
    const auto it = shard.map.find(key);
    if (it == shard.map.end()) {
        return false;
    }
    shard.map.erase(it);
    return true;
}

bool iron::store::contains(std::string_view key) const {
    const Shard &shard = shard_for(key);
    std::shared_lock lock(shard.mutex);
    return shard.map.contains(key);
}

std::optional<std::string> iron::store::get_record(std::string_view key) const {
    std::optional<std::string> result;
    read_record(key, [&](std::string_view value) { result.emplace(value); });
    return result;
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
        shard.map.clear();
    }
}
