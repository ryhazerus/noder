#include "resp_protocol.h"

#include <algorithm>
#include <charconv>
#include <cstdint>
#include <vector>

#include "../event_loop/event_server.h"
#include "../store/store.h"

namespace iron::resp_protocol {
    namespace {
        constexpr int64_t kMaxBulkLength = 512 * 1024 * 1024;   // same as Redis' default proto-max-bulk-len
        constexpr int64_t kMaxArgs = 1024 * 1024;
        constexpr size_t kMaxInlineSize = 64 * 1024;
        constexpr size_t kMaxNumberLength = 32;

        enum class Parse { Complete, Incomplete, Error };

        // ---- writing replies ----

        void append_number(std::string &out, int64_t value) {
            char digits[24];
            const auto result = std::to_chars(digits, digits + sizeof(digits), value);
            out.append(digits, result.ptr);
        }

        void append_simple(std::string &out, std::string_view text) {
            out += '+';
            out += text;
            out += "\r\n";
        }

        void append_error(std::string &out, std::string_view message) {
            out += '-';
            out += message;
            out += "\r\n";
        }

        void append_integer(std::string &out, int64_t value) {
            out += ':';
            append_number(out, value);
            out += "\r\n";
        }

        void append_bulk(std::string &out, std::string_view data) {
            out += '$';
            append_number(out, static_cast<int64_t>(data.size()));
            out += "\r\n";
            out += data;
            out += "\r\n";
        }

        void append_null(std::string &out) {
            out += "$-1\r\n";
        }

        void append_array_header(std::string &out, size_t count) {
            out += '*';
            append_number(out, static_cast<int64_t>(count));
            out += "\r\n";
        }

        void append_wrong_args(std::string &out, std::string_view command) {
            out += "-ERR wrong number of arguments for '";
            out += command;
            out += "' command\r\n";
        }

        // ---- parsing requests ----

        // reads "<number>\r\n" at pos; on success pos points past the \n
        Parse read_number_line(std::string_view in, size_t &pos, int64_t &value) {
            // only look a few bytes ahead, a number line is never long
            const std::string_view window = in.substr(pos, kMaxNumberLength + 2);
            const size_t cr = window.find('\r');
            if (cr == std::string_view::npos) {
                return window.size() > kMaxNumberLength ? Parse::Error : Parse::Incomplete;
            }
            if (cr + 1 >= window.size()) return Parse::Incomplete;
            if (window[cr + 1] != '\n') return Parse::Error;

            const auto result = std::from_chars(window.data(), window.data() + cr, value);
            if (result.ec != std::errc() || result.ptr != window.data() + cr) return Parse::Error;
            pos += cr + 2;
            return Parse::Complete;
        }

        // "*<count>\r\n" followed by count times "$<length>\r\n<bytes>\r\n"
        // The args are views into `in`: no copies, valid until the caller consumes those bytes.
        Parse parse_multibulk(std::string_view in, size_t &pos, std::vector<std::string_view> &args,
                              const char *&error) {
            size_t p = pos + 1;   // skip '*'
            int64_t count;
            Parse result = read_number_line(in, p, count);
            if (result != Parse::Complete) {
                error = "invalid multibulk length";
                return result;
            }
            if (count > kMaxArgs) {
                error = "invalid multibulk length";
                return Parse::Error;
            }

            args.clear();
            for (int64_t i = 0; i < count; ++i) {
                if (p >= in.size()) return Parse::Incomplete;
                if (in[p] != '$') {
                    error = "expected '$'";
                    return Parse::Error;
                }
                ++p;

                int64_t length;
                result = read_number_line(in, p, length);
                if (result != Parse::Complete) {
                    error = "invalid bulk length";
                    return result;
                }
                if (length < 0 || length > kMaxBulkLength) {
                    error = "invalid bulk length";
                    return Parse::Error;
                }

                const auto size = static_cast<size_t>(length);
                if (in.size() - p < size + 2) return Parse::Incomplete;   // the data hasn't fully arrived yet
                if (in[p + size] != '\r' || in[p + size + 1] != '\n') {
                    error = "expected CRLF after bulk data";
                    return Parse::Error;
                }
                args.emplace_back(in.data() + p, size);
                p += size + 2;
            }

            pos = p;
            return Parse::Complete;
        }

        // inline commands are plain text lines like "PING" or "GET foo", what you type into telnet/nc
        void split_inline(std::string_view line, std::vector<std::string_view> &args) {
            args.clear();
            size_t i = 0;
            while (i < line.size()) {
                while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) ++i;
                const size_t start = i;
                while (i < line.size() && line[i] != ' ' && line[i] != '\t') ++i;
                if (i > start) args.push_back(line.substr(start, i - start));
            }
        }

        // ---- commands ----

        // compares against a lowercase name, ignoring the case of `text`
        bool is(std::string_view text, std::string_view lowercaseName) {
            if (text.size() != lowercaseName.size()) return false;
            for (size_t i = 0; i < text.size(); ++i) {
                char c = text[i];
                if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + ('a' - 'A'));
                if (c != lowercaseName[i]) return false;
            }
            return true;
        }

        void append_unknown_command(std::string &out, std::string_view name) {
            out += "-ERR unknown command '";
            // the name goes into a single-line reply: drop anything that could break the framing
            for (const char c : name.substr(0, 128)) {
                out += (static_cast<unsigned char>(c) < 0x20 || c == 0x7F) ? ' ' : c;
            }
            out += "'\r\n";
        }

        // a whole argument as a 64-bit integer, like Redis' string2ll
        bool parse_int64(std::string_view text, int64_t &value) {
            const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
            return result.ec == std::errc() && result.ptr == text.data() + text.size();
        }

        // SET key value [EX seconds | PX milliseconds | KEEPTTL]
        void execute_set(const std::vector<std::string_view> &args, std::string &out, store &kv) {
            const size_t argc = args.size();
            if (argc < 3) return append_wrong_args(out, "set");

            Ttl ttl = Ttl::clear();
            bool ttlGiven = false;   // EX, PX and KEEPTTL exclude each other
            for (size_t i = 3; i < argc; ++i) {
                const std::string_view option = args[i];
                const bool seconds = is(option, "ex");
                if (seconds || is(option, "px")) {
                    if (ttlGiven || i + 1 >= argc) return append_error(out, "ERR syntax error");
                    int64_t amount;
                    if (!parse_int64(args[++i], amount)) {
                        return append_error(out, "ERR value is not an integer or out of range");
                    }
                    const int64_t limit = seconds ? store::kMaxTtlMs / 1000 : store::kMaxTtlMs;
                    if (amount <= 0 || amount > limit) {
                        return append_error(out, "ERR invalid expire time in 'set' command");
                    }
                    ttl = Ttl::in_ms(seconds ? amount * 1000 : amount);
                    ttlGiven = true;
                } else if (is(option, "keepttl")) {
                    if (ttlGiven) return append_error(out, "ERR syntax error");
                    ttl = Ttl::keep();
                    ttlGiven = true;
                } else {
                    return append_error(out, "ERR syntax error");   // NX, XX, GET, ... aren't supported
                }
            }

            kv.add_record(args[1], args[2], ttl);
            append_simple(out, "OK");
        }

        // EXPIRE key seconds / PEXPIRE key milliseconds
        void execute_expire(const std::vector<std::string_view> &args, std::string &out, store &kv, bool seconds) {
            const std::string_view name = seconds ? "expire" : "pexpire";
            if (args.size() != 3) return append_wrong_args(out, name);
            int64_t amount;
            if (!parse_int64(args[2], amount)) return append_error(out, "ERR value is not an integer or out of range");
            if (amount > (seconds ? store::kMaxTtlMs / 1000 : store::kMaxTtlMs)) {
                out += "-ERR invalid expire time in '";
                out += name;
                out += "' command\r\n";
                return;
            }
            // zero or negative deletes the key, like Redis
            const int64_t ms = amount <= 0 ? 0 : (seconds ? amount * 1000 : amount);
            append_integer(out, kv.expire_in(args[1], ms) ? 1 : 0);
        }

        void execute(const std::vector<std::string_view> &args, std::string &out, store &kv) {
            const std::string_view command = args[0];
            const size_t argc = args.size();

            // most frequent commands first
            if (is(command, "get")) {
                if (argc != 2) return append_wrong_args(out, "get");
                const bool found = kv.read_record(args[1], [&](std::string_view value) { append_bulk(out, value); });
                if (!found) append_null(out);
            } else if (is(command, "set")) {
                execute_set(args, out, kv);
            } else if (is(command, "del")) {
                if (argc < 2) return append_wrong_args(out, "del");
                int64_t deleted = 0;
                for (size_t i = 1; i < argc; ++i) deleted += kv.delete_record(args[i]);
                append_integer(out, deleted);
            } else if (is(command, "exists")) {
                if (argc < 2) return append_wrong_args(out, "exists");
                int64_t existing = 0;
                for (size_t i = 1; i < argc; ++i) existing += kv.contains(args[i]);
                append_integer(out, existing);
            } else if (is(command, "mget")) {
                if (argc < 2) return append_wrong_args(out, "mget");
                append_array_header(out, argc - 1);
                for (size_t i = 1; i < argc; ++i) {
                    if (!kv.read_record(args[i], [&](std::string_view value) { append_bulk(out, value); })) {
                        append_null(out);
                    }
                }
            } else if (is(command, "mset")) {
                if (argc < 3 || argc % 2 == 0) return append_wrong_args(out, "mset");
                for (size_t i = 1; i < argc; i += 2) kv.add_record(args[i], args[i + 1]);
                append_simple(out, "OK");
            } else if (is(command, "expire")) {
                execute_expire(args, out, kv, true);
            } else if (is(command, "pexpire")) {
                execute_expire(args, out, kv, false);
            } else if (is(command, "ttl") || is(command, "pttl")) {
                const bool seconds = is(command, "ttl");
                if (argc != 2) return append_wrong_args(out, seconds ? "ttl" : "pttl");
                const int64_t ms = kv.ttl_ms(args[1]);
                // -2 (missing) and -1 (no TTL) are passed through; Redis rounds TTL to the nearest second
                append_integer(out, ms < 0 || !seconds ? ms : (ms + 500) / 1000);
            } else if (is(command, "persist")) {
                if (argc != 2) return append_wrong_args(out, "persist");
                append_integer(out, kv.persist(args[1]) ? 1 : 0);
            } else if (is(command, "ping")) {
                if (argc == 1) append_simple(out, "PONG");
                else if (argc == 2) append_bulk(out, args[1]);
                else append_wrong_args(out, "ping");
            } else if (is(command, "echo")) {
                if (argc != 2) return append_wrong_args(out, "echo");
                append_bulk(out, args[1]);
            } else if (is(command, "dbsize")) {
                append_integer(out, static_cast<int64_t>(kv.size()));
            } else if (is(command, "flushall") || is(command, "flushdb")) {
                kv.clear();
                append_simple(out, "OK");
            } else if (is(command, "config")) {
                // redis-benchmark asks for these two on startup; answer like a Redis without persistence
                if (argc < 3 || !is(args[1], "get")) return append_error(out, "ERR only CONFIG GET is supported");
                if (is(args[2], "save")) {
                    append_array_header(out, 2);
                    append_bulk(out, "save");
                    append_bulk(out, "");
                } else if (is(args[2], "appendonly")) {
                    append_array_header(out, 2);
                    append_bulk(out, "appendonly");
                    append_bulk(out, "no");
                } else {
                    append_array_header(out, 0);
                }
            } else if (is(command, "command")) {
                append_array_header(out, 0);   // redis-cli asks for command docs on startup
            } else if (is(command, "client") || is(command, "quit")) {
                append_simple(out, "OK");      // client libraries send CLIENT SETNAME / SETINFO when connecting
            } else if (is(command, "select")) {
                if (argc != 2) return append_wrong_args(out, "select");
                if (args[1] != "0") return append_error(out, "ERR DB index is out of range");
                append_simple(out, "OK");
            } else {
                append_unknown_command(out, command);
            }
        }
    }

    size_t process(std::string_view input, std::string &out, store &kv) {
        // reused for every command on this thread, so parsing doesn't allocate
        thread_local std::vector<std::string_view> args;

        size_t pos = 0;
        while (pos < input.size()) {
            if (input[pos] == '*') {
                const char *error = nullptr;
                size_t next = pos;
                const Parse result = parse_multibulk(input, next, args, error);
                if (result == Parse::Incomplete) break;   // wait for more data
                if (result == Parse::Error) {
                    // like Redis: report the problem, then close, because we can't find the next command
                    out += "-ERR Protocol error: ";
                    out += error;
                    out += "\r\n";
                    return event_server::kClose;
                }
                pos = next;
            } else {
                const size_t newline = input.find('\n', pos);
                if (newline == std::string_view::npos) {
                    if (input.size() - pos > kMaxInlineSize) {
                        append_error(out, "ERR Protocol error: too big inline request");
                        return event_server::kClose;
                    }
                    break;
                }
                std::string_view line = input.substr(pos, newline - pos);
                if (!line.empty() && line.back() == '\r') line.remove_suffix(1);
                split_inline(line, args);
                pos = newline + 1;
            }

            if (!args.empty()) execute(args, out, kv);
        }
        return pos;
    }
}
