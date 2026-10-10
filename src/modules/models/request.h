#pragma once

#include <nlohmann/json.hpp>
#include <cstdint>
#include <optional>
#include <string>

enum class Command {
    Invalid,   // must be first: nlohmann uses the first entry for unknown strings
    Add,
    Get,
    Update,
    Delete
};

NLOHMANN_JSON_SERIALIZE_ENUM(Command, {
    {Command::Invalid, nullptr},
    {Command::Add,     "ADD"},
    {Command::Get,     "GET"},
    {Command::Update,  "UPDATE"},
    {Command::Delete,  "DELETE"},
})

class Request {
private:
    Command     command_ = Command::Invalid;
    std::string key_;
    std::string value_;
    std::optional<int64_t> ttl_;   // seconds, ADD/UPDATE only

public:
    Request() = default;

    Command            command() const { return command_; }
    const std::string& key()     const { return key_; }
    const std::string& value()   const { return value_; }
    std::optional<int64_t> ttl() const { return ttl_; }

    // Friend so it can fill in the private members
    friend void from_json(const nlohmann::json& j, Request& r);
};

// Declared at namespace scope too, so it is visible to nlohmann everywhere.
// Only the JSON protocol's slow path uses this, for requests its fast parser doesn't handle.
void from_json(const nlohmann::json& j, Request& r);
