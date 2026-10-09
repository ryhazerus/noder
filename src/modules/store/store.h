//
// Created by zahyrseferina on 10/9/26.
//

#ifndef NODE_CONNECTOR_STORE_H
#define NODE_CONNECTOR_STORE_H
#include <mutex>
#include <optional>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <unordered_map>

namespace iron {
    // lets find/erase take a string_view without building a temporary std::string
    struct string_hash {
        using is_transparent = void;
        size_t operator()(std::string_view sv) const noexcept { return std::hash<std::string_view>{}(sv); }
    };

    class store {
    private:
        std::unordered_map<std::string, std::string, string_hash, std::equal_to<>> kvStore_;
        // many readers at once, writers get exclusive access
        mutable std::shared_mutex mutex_;

    public:
        store();
        std::optional<std::string> get_record(std::string_view key) const;
        void add_record(const std::string &key, const std::string &value);

        void delete_record(std::string_view key);

        ~store();
    };
}


#endif //NODE_CONNECTOR_STORE_H
