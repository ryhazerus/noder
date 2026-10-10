#include "request.h"

#include <stdexcept>

#include "../store/store.h"

// Deserialize: JSON -> Request (rejects bad commands)
void from_json(const nlohmann::json& j, Request& r) {
    j.at("command").get_to(r.command_);
    if (r.command_ == Command::Invalid)
        throw std::invalid_argument("Unknown command; expected ADD, GET, UPDATE or DELETE");
    j.at("key").get_to(r.key_);
    r.value_ = j.value("value", "");   // optional for GET / DELETE

    // optional: seconds until the key expires
    if (const auto ttl = j.find("ttl"); ttl != j.end()) {
        const bool validNumber = ttl->is_number_integer() && !(ttl->is_number_unsigned() &&
                                 ttl->get<uint64_t>() > static_cast<uint64_t>(INT64_MAX));
        const int64_t seconds = validNumber ? ttl->get<int64_t>() : 0;
        if (seconds <= 0 || seconds > iron::store::kMaxTtlMs / 1000)
            throw std::invalid_argument("ttl must be a whole number of seconds, greater than 0");
        r.ttl_ = seconds;
    }
}