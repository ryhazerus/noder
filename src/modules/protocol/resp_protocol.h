#pragma once

#include <string>
#include <string_view>

namespace iron {
    class store;
}

/**
 * A subset of the Redis protocol (RESP2), so redis-cli, redis-benchmark and Redis client
 * libraries can talk to the server.
 *
 * Supported: GET, SET (without options), DEL, EXISTS, MGET, MSET, PING, ECHO, DBSIZE,
 * FLUSHALL/FLUSHDB, plus enough of COMMAND, CONFIG GET, CLIENT, SELECT and QUIT
 * for clients and benchmark tools to connect.
 */
namespace iron::resp_protocol {
    // Handles every complete command in `input` and appends the replies to `out`.
    // Returns the number of bytes consumed, or event_server::kClose to drop the connection.
    size_t process(std::string_view input, std::string &out, store &kv);
}
