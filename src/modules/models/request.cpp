#include "request.h"

#include <stdexcept>

// Serialize: Request -> JSON
void to_json(nlohmann::json& j, const Request& r) {
    j = nlohmann::json{
        {"command", r.command_},
        {"key",     r.key_},
        {"value",   r.value_}
    };
}

// Deserialize: JSON -> Request (rejects bad commands)
void from_json(const nlohmann::json& j, Request& r) {
    j.at("command").get_to(r.command_);
    if (r.command_ == Command::Invalid)
        throw std::invalid_argument("Unknown command; expected ADD, UPDATE or DELETE");
    j.at("key").get_to(r.key_);
    j.at("value").get_to(r.value_);
}