//
// Created by zahyrseferina on 10/9/26.
//

#include "store.h"

iron::store::store() : kvStore_() {
}

void iron::store::add_record(const std::string &key, const std::string &value) {
    this->kvStore_[key] = value;
}

void iron::store::delete_record(const std::string &key) {
    this->kvStore_.erase(key);
}

std::optional<std::string> iron::store::get_record(const std::string &key) const {
    const auto it = this->kvStore_.find(key);
    if (it == this->kvStore_.end()) {
        return std::nullopt;
    }
    return it->second;
}

iron::store::~store() {
}
