//
// Created by zahyrseferina on 10/9/26.
//

#include "Core.h"

#include <iostream>
#include <stdexcept>

#include <nlohmann/json.hpp>

Core::Core(int port, unsigned threads)
    : _port(port), _threads(threads),
      server([this](std::string_view line, std::string &out) { handleLine(line, out); }) {
}

int Core::start() {
    try {
        server.start(_port, _threads);
    } catch (const std::runtime_error &error) {
        std::cout << "Server start failed: " << error.what() << std::endl;
        return EXIT_FAILURE;
    }
    std::cout << "Server started on port " << _port << " with " << _threads << " worker threads" << std::endl;
    return 0;
}

// runs on a worker thread; several workers call this at the same time
void Core::handleLine(std::string_view line, std::string &out) {
    Request req;
    try {
        req = nlohmann::json::parse(line).get<Request>();
    }
    catch (const nlohmann::json::exception &e) {
        std::cerr << "Bad request: " << e.what() << "\n";
        return;
    }
    catch (const std::invalid_argument &e) {
        std::cerr << "Invalid command: " << e.what() << "\n";
        return;
    }

    switch (req.command()) {
        case Command::Add:
        case Command::Update:
            onIncomingUpdateMsg(req);
            break;
        case Command::Delete:
            onIncomingDeleteMsg(req);
            break;
        case Command::Get:
            onIncomingGetMsg(req, out);
            break;
        default:
            std::cerr << "Unhandled command for key: " << req.key() << "\n";
            break;
    }
}

void Core::onIncomingUpdateMsg(const Request &req) {
#ifndef NDEBUG   // per-request logging only in debug builds, it is costly at high request rates
    std::cout << "Update Query: " << req.key() << " = " << req.value() << "\n";
#endif
    _store.add_record(req.key(), req.value());
}

void Core::onIncomingDeleteMsg(const Request &req) {
#ifndef NDEBUG   // per-request logging only in debug builds, it is costly at high request rates
    std::cout << "Delete Query: " << req.key() << "\n";
#endif
    _store.delete_record(req.key());
}

void Core::onIncomingGetMsg(const Request &req, std::string &out) {
#ifndef NDEBUG   // per-request logging only in debug builds, it is costly at high request rates
    std::cout << "Get Query: " << req.key() << "\n";
#endif

    const std::optional<std::string> record = _store.get_record(req.key());

    nlohmann::json response{
        {"command", Command::Get},
        {"key",     req.key()},
        {"found",   record.has_value()},
        {"value",   record ? nlohmann::json(*record) : nlohmann::json(nullptr)}
    };

    // newline-delimited, same framing as incoming requests. The event server sends
    // everything appended to `out` in one go after the whole batch of requests is handled
    out += response.dump();
    out += '\n';
}

void Core::stop() {
    server.stop();
}

Core::~Core() {
    stop();
}
