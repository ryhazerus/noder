//
// Created by zahyrseferina on 10/9/26.
//

#include "store.h"

iron::store::store() : store_() {
}

void iron::store::add_record(const std::string &key, const std::string &value) {
    this->store_[key] = value;
}

void iron::store::delete_record(const std::string &key) {
    this->store_.erase(key);
}

iron::store::~store() {
}
