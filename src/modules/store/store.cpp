//
// Created by zahyrseferina on 10/9/26.
//

#include "store.h"

iron::store::store() : kvStore_() {
    this->kvStore_.reserve(100000);
}

void iron::store::add_record(const std::string &key, const std::string &value) {
    std::unique_lock lock(this->mutex_);
    this->kvStore_[key] = value;
}

void iron::store::delete_record(std::string_view key) {
    std::unique_lock lock(this->mutex_);
    this->kvStore_.erase(key);
}

std::optional<std::string> iron::store::get_record(std::string_view key) const {
    std::shared_lock lock(this->mutex_);
    const auto it = this->kvStore_.find(key);
    if (it == this->kvStore_.end()) {
        return std::nullopt;
    }
    return it->second;
}

iron::store::~store() {
}
