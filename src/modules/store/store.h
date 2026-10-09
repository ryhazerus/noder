//
// Created by zahyrseferina on 10/9/26.
//

#ifndef NODE_CONNECTOR_STORE_H
#define NODE_CONNECTOR_STORE_H
#include <optional>
#include <string>
#include <unordered_map>

namespace iron {
    class store {
    private:
        std::unordered_map<std::string, std::string> kvStore_;

    public:
        store();
        std::optional<std::string> get_record(const std::string &key) const;
        void add_record(const std::string &key, const std::string &value);

        void delete_record(const std::string &key);

        ~store();
    };
}


#endif //NODE_CONNECTOR_STORE_H
