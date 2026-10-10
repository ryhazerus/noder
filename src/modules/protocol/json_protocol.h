#pragma once

#include <string>
#include <string_view>

namespace iron {
    class store;
}

/**
 * The original protocol: one JSON object per line.
 *   {"command":"ADD|UPDATE|DELETE|GET","key":"...","value":"...","ttl":60}
 * "ttl" is optional, in seconds, for ADD/UPDATE. Without it the key never expires.
 * Only GET replies: {"command":"GET","found":true,"key":"...","value":"..."}
 */
namespace iron::json_protocol {
    // Handles every complete line in `input` and appends the replies to `out`.
    // Returns the number of bytes consumed, or event_server::kClose to drop the connection.
    size_t process(std::string_view input, std::string &out, store &kv);

    // appends `s` as a JSON string (with quotes), escaped exactly like nlohmann::json::dump()
    void append_string(std::string &out, std::string_view s);
}
