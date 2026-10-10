#pragma once

#include <nlohmann/json.hpp>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>

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
    Request(Command c, std::string key, std::string value = "")
        : command_(c), key_(std::move(key)), value_(std::move(value)) {}

    Command            command() const { return command_; }
    const std::string& key()     const { return key_; }
    const std::string& value()   const { return value_; }
    std::optional<int64_t> ttl() const { return ttl_; }

    // Friends so the JSON functions can access the private members
    friend void to_json(nlohmann::json& j, const Request& r);
    friend void from_json(const nlohmann::json& j, Request& r);
};

// Declared at namespace scope too, so they are visible to nlohmann everywhere
void to_json(nlohmann::json& j, const Request& r);
void from_json(const nlohmann::json& j, Request& r);