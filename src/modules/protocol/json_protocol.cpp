#include "json_protocol.h"

#include <charconv>
#include <cstdint>
#include <cstdio>
#include <iostream>
#include <stdexcept>

#include <nlohmann/json.hpp>

#include "../event_loop/event_server.h"
#include "../models/request.h"
#include "../store/store.h"

namespace iron::json_protocol {
    namespace {
        constexpr size_t kMaxLineSize = 1 << 20;   // a client that never sends '\n' gets disconnected

        // Fast path: a tiny parser for the one shape of object we expect, with string values only
        // (and a plain integer for "ttl").
        // Strings without escapes become views straight into the receive buffer (no copy, no allocation).
        // Anything else (numbers, null, nesting, invalid JSON) is handed to nlohmann in handle_line,
        // so odd input behaves exactly like before and gets the same error messages.

        struct Fields {
            std::string_view command, key, value;
            int64_t ttl = 0;
            bool hasCommand = false, hasKey = false, hasValue = false, hasTtl = false;
        };

        // only used when a string contains escapes; reused between requests so they keep their capacity
        struct Scratch {
            std::string name, command, key, value, other;
        };

        void skip_whitespace(const char *&p, const char *end) {
            while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) ++p;
        }

        // nlohmann rejects invalid UTF-8 too, so the fast path must not accept it
        bool valid_utf8(std::string_view s) {
            const auto *bytes = reinterpret_cast<const unsigned char *>(s.data());
            size_t i = 0;
            while (i < s.size()) {
                const unsigned char c = bytes[i];
                if (c < 0x80) {
                    ++i;
                    continue;
                }

                size_t length;
                uint32_t codepoint;
                if ((c & 0xE0) == 0xC0) { length = 2; codepoint = c & 0x1F; }
                else if ((c & 0xF0) == 0xE0) { length = 3; codepoint = c & 0x0F; }
                else if ((c & 0xF8) == 0xF0) { length = 4; codepoint = c & 0x07; }
                else return false;

                if (i + length > s.size()) return false;
                for (size_t k = 1; k < length; ++k) {
                    if ((bytes[i + k] & 0xC0) != 0x80) return false;
                    codepoint = (codepoint << 6) | (bytes[i + k] & 0x3F);
                }
                const bool overlong = (length == 2 && codepoint < 0x80) || (length == 3 && codepoint < 0x800) ||
                                      (length == 4 && codepoint < 0x10000);
                const bool surrogate = codepoint >= 0xD800 && codepoint <= 0xDFFF;
                if (overlong || surrogate || codepoint > 0x10FFFF) return false;
                i += length;
            }
            return true;
        }

        void append_utf8(std::string &out, uint32_t codepoint) {
            if (codepoint < 0x80) {
                out += static_cast<char>(codepoint);
            } else if (codepoint < 0x800) {
                out += static_cast<char>(0xC0 | (codepoint >> 6));
                out += static_cast<char>(0x80 | (codepoint & 0x3F));
            } else if (codepoint < 0x10000) {
                out += static_cast<char>(0xE0 | (codepoint >> 12));
                out += static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F));
                out += static_cast<char>(0x80 | (codepoint & 0x3F));
            } else {
                out += static_cast<char>(0xF0 | (codepoint >> 18));
                out += static_cast<char>(0x80 | ((codepoint >> 12) & 0x3F));
                out += static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F));
                out += static_cast<char>(0x80 | (codepoint & 0x3F));
            }
        }

        bool parse_hex4(const char *&p, const char *end, uint32_t &value) {
            if (end - p < 4) return false;
            value = 0;
            for (int i = 0; i < 4; ++i) {
                const char c = *p++;
                value <<= 4;
                if (c >= '0' && c <= '9') value |= c - '0';
                else if (c >= 'a' && c <= 'f') value |= c - 'a' + 10;
                else if (c >= 'A' && c <= 'F') value |= c - 'A' + 10;
                else return false;
            }
            return true;
        }

        // p points at the opening quote; on success it points just past the closing quote
        bool parse_string(const char *&p, const char *end, std::string_view &result, std::string &scratch) {
            const char *start = ++p;

            // common case: no escapes, so the string can be used where it is
            while (p < end) {
                const auto c = static_cast<unsigned char>(*p);
                if (c == '"') {
                    result = std::string_view(start, p - start);
                    ++p;
                    return valid_utf8(result);
                }
                if (c == '\\') break;
                if (c < 0x20) return false;   // raw control characters aren't allowed in JSON strings
                ++p;
            }
            if (p >= end) return false;

            // escapes: decode into the scratch buffer
            scratch.assign(start, p);
            while (p < end) {
                const auto c = static_cast<unsigned char>(*p++);
                if (c == '"') {
                    result = scratch;
                    return valid_utf8(scratch);
                }
                if (c < 0x20) return false;
                if (c != '\\') {
                    scratch += static_cast<char>(c);
                    continue;
                }
                if (p >= end) return false;
                switch (*p++) {
                    case '"': scratch += '"'; break;
                    case '\\': scratch += '\\'; break;
                    case '/': scratch += '/'; break;
                    case 'b': scratch += '\b'; break;
                    case 'f': scratch += '\f'; break;
                    case 'n': scratch += '\n'; break;
                    case 'r': scratch += '\r'; break;
                    case 't': scratch += '\t'; break;
                    case 'u': {
                        uint32_t codepoint;
                        if (!parse_hex4(p, end, codepoint)) return false;
                        if (codepoint >= 0xD800 && codepoint <= 0xDBFF) {
                            // characters outside the BMP (emoji etc.) come as a surrogate pair: 😀
                            uint32_t low;
                            if (end - p < 6 || p[0] != '\\' || p[1] != 'u') return false;
                            p += 2;
                            if (!parse_hex4(p, end, low) || low < 0xDC00 || low > 0xDFFF) return false;
                            codepoint = 0x10000 + ((codepoint - 0xD800) << 10) + (low - 0xDC00);
                        } else if (codepoint >= 0xDC00 && codepoint <= 0xDFFF) {
                            return false;   // low surrogate without a high one
                        }
                        append_utf8(scratch, codepoint);
                        break;
                    }
                    default:
                        return false;
                }
            }
            return false;
        }

        // a plain integer like 60; anything else (1.5, 1e3, "60") goes to the slow path
        bool parse_integer(const char *&p, const char *end, int64_t &value) {
            const char *digits = (p < end && *p == '-') ? p + 1 : p;
            if (end - digits > 1 && digits[0] == '0' && digits[1] >= '0' && digits[1] <= '9') return false;   // 007 isn't JSON
            const auto result = std::from_chars(p, end, value);
            if (result.ec != std::errc()) return false;
            p = result.ptr;
            return p == end || *p == ',' || *p == '}' || *p == ' ' || *p == '\t' || *p == '\r' || *p == '\n';
        }

        bool parse_request(std::string_view line, Fields &fields, Scratch &scratch) {
            const char *p = line.data();
            const char *end = p + line.size();

            skip_whitespace(p, end);
            if (p == end || *p != '{') return false;
            ++p;
            skip_whitespace(p, end);

            if (p < end && *p == '}') {
                ++p;
            } else {
                while (true) {
                    if (p == end || *p != '"') return false;
                    std::string_view name;
                    if (!parse_string(p, end, name, scratch.name)) return false;

                    skip_whitespace(p, end);
                    if (p == end || *p != ':') return false;
                    ++p;
                    skip_whitespace(p, end);

                    // a repeated field overwrites the earlier one, same as nlohmann
                    bool ok;
                    if (name == "ttl") {
                        ok = parse_integer(p, end, fields.ttl);
                        fields.hasTtl = true;
                    } else if (p == end || *p != '"') {
                        return false;   // other non-string values: leave it to nlohmann
                    } else if (name == "command") {
                        ok = parse_string(p, end, fields.command, scratch.command);
                        fields.hasCommand = true;
                    } else if (name == "key") {
                        ok = parse_string(p, end, fields.key, scratch.key);
                        fields.hasKey = true;
                    } else if (name == "value") {
                        ok = parse_string(p, end, fields.value, scratch.value);
                        fields.hasValue = true;
                    } else {
                        std::string_view ignored;
                        ok = parse_string(p, end, ignored, scratch.other);
                    }
                    if (!ok) return false;

                    skip_whitespace(p, end);
                    if (p == end) return false;
                    if (*p == ',') {
                        ++p;
                        skip_whitespace(p, end);
                        continue;
                    }
                    if (*p == '}') {
                        ++p;
                        break;
                    }
                    return false;
                }
            }

            skip_whitespace(p, end);
            return p == end;
        }

        Command to_command(std::string_view name) {
            if (name == "GET") return Command::Get;
            if (name == "ADD") return Command::Add;
            if (name == "UPDATE") return Command::Update;
            if (name == "DELETE") return Command::Delete;
            return Command::Invalid;
        }

        void execute(Command command, std::string_view key, std::string_view value, std::optional<int64_t> ttlSeconds,
                     std::string &out, store &kv) {
            switch (command) {
                case Command::Add:
                case Command::Update:
                    // no ttl: the key becomes permanent, also when it had a TTL before (like Redis' SET)
                    kv.add_record(key, value, ttlSeconds ? Ttl::in_ms(*ttlSeconds * 1000) : Ttl::clear());
                    break;
                case Command::Delete:
                    kv.delete_record(key);
                    break;
                case Command::Get: {
                    // same bytes nlohmann produced: keys sorted, no spaces
                    out += R"({"command":"GET","found":)";
                    const bool found = kv.read_record(key, [&](std::string_view stored) {
                        out += R"(true,"key":)";
                        append_string(out, key);
                        out += R"(,"value":)";
                        append_string(out, stored);
                    });
                    if (!found) {
                        out += R"(false,"key":)";
                        append_string(out, key);
                        out += R"(,"value":null)";
                    }
                    out += "}\n";
                    break;
                }
                default:
                    std::cerr << "Unhandled command for key: " << key << "\n";
                    break;
            }
        }

        void handle_line(std::string_view line, std::string &out, store &kv, Scratch &scratch) {
            Fields fields;
            if (parse_request(line, fields, scratch) && fields.hasCommand && fields.hasKey) {
                const Command command = to_command(fields.command);
                // an out-of-range ttl goes to the slow path too, which reports the error
                const bool ttlValid = !fields.hasTtl || (fields.ttl > 0 && fields.ttl <= store::kMaxTtlMs / 1000);
                if (command != Command::Invalid && ttlValid) {
                    const std::optional<int64_t> ttl = fields.hasTtl ? std::optional(fields.ttl) : std::nullopt;
                    execute(command, fields.key, fields.hasValue ? fields.value : std::string_view{}, ttl, out, kv);
                    return;
                }
            }

            // slow path: whatever the fast parser doesn't handle, including all the error cases
            try {
                const Request req = nlohmann::json::parse(line).get<Request>();
                execute(req.command(), req.key(), req.value(), req.ttl(), out, kv);
            }
            catch (const nlohmann::json::exception &e) {
                std::cerr << "Bad request: " << e.what() << "\n";
            }
            catch (const std::invalid_argument &e) {
                std::cerr << "Invalid command: " << e.what() << "\n";
            }
        }
    }

    void append_string(std::string &out, std::string_view s) {
        out += '"';
        // copy runs of characters that need no escaping in one go
        size_t runStart = 0;
        for (size_t i = 0; i < s.size(); ++i) {
            const auto c = static_cast<unsigned char>(s[i]);
            if (c >= 0x20 && c != '"' && c != '\\') continue;

            out.append(s.data() + runStart, i - runStart);
            switch (c) {
                case '"': out += "\\\""; break;
                case '\\': out += "\\\\"; break;
                case '\b': out += "\\b"; break;
                case '\f': out += "\\f"; break;
                case '\n': out += "\\n"; break;
                case '\r': out += "\\r"; break;
                case '\t': out += "\\t"; break;
                default: {
                    char escaped[7];
                    std::snprintf(escaped, sizeof(escaped), "\\u%04x", c);
                    out += escaped;
                }
            }
            runStart = i + 1;
        }
        out.append(s.data() + runStart, s.size() - runStart);
        out += '"';
    }

    size_t process(std::string_view input, std::string &out, store &kv) {
        thread_local Scratch scratch;

        size_t start = 0;
        size_t newline;
        while ((newline = input.find('\n', start)) != std::string_view::npos) {
            std::string_view line = input.substr(start, newline - start);
            if (!line.empty() && line.back() == '\r') line.remove_suffix(1);   // tolerate \r\n
            if (!line.empty()) handle_line(line, out, kv, scratch);
            start = newline + 1;
        }

        if (input.size() - start > kMaxLineSize) {
            std::cerr << "line too long, closing connection\n";
            return event_server::kClose;
        }
        return start;
    }
}
