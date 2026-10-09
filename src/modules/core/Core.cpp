//
// Created by zahyrseferina on 10/9/26.
//

#include "Core.h"

#include <iostream>

#include <nlohmann/json.hpp>

Core::Core(const int &port) : _port(port) {
}


int Core::start() {
    // start server on given port
    auto start_result = server.start(this->_port);

    if (start_result.isSuccessful()) {
        std::cout << "Server started successfully " << std::endl;
    } else {
        std::cout << "Server started failed " << std::endl;
        return EXIT_FAILURE;
    }

    // single observer: parse each packet once, then dispatch per request.
    // (multiple observers with wantedIP "" would all receive the same bytes
    // and append them to the same per-client buffer more than once)
    observer.incomingPacketHandler = [this](const std::string &clientIP, const char *msg, size_t size) {
        for (const Request &req : parseRequests(clientIP, msg, size)) {
            onIncomingRequest(clientIP, req);
        }
    };
    observer.disconnectionHandler = [this](const std::string &ip, const std::string &msg) {
        onClientDisconnected(ip, msg);
    };
    observer.wantedIP = ""; // empty string receives messages from any IP address
    server.subscribe(observer);

    _running = true;
    _acceptThread = std::thread(&Core::acceptLoop, this);
    return 0;
}


void Core::acceptClient() {
    try {
        // 1s timeout so the loop wakes up regularly to check _running
        std::string clientIP = server.acceptClient(1);
        // don't print the full client list here: with n clients that is O(n) output per accept,
        // which made accepting thousands of connections take minutes
        std::cout << "accepted new client: " << clientIP << "\n";
    } catch (const std::runtime_error &error) {
        if (std::string(error.what()) != "Timeout waiting for client") {
            std::cout << "Accepting client failed: " << error.what() << "\n";
        }
    }
}

std::vector<Request> Core::parseRequests(const std::string &clientIP, const char *msg, size_t size) {
    std::vector<Request> out;

    // under the lock: only move the complete lines out of the buffer.
    // JSON parsing happens after, so other clients aren't kept waiting on it
    std::string complete;
    {
        std::lock_guard<std::mutex> lock(buffersMutex_);
        std::string &buf = buffers_[clientIP];
        buf.append(msg, size);

        // Guard against a client that never sends a newline
        constexpr size_t kMaxBuffer = 1 << 20;   // 1 MB
        if (buf.size() > kMaxBuffer) {
            std::cerr << "Buffer overflow from " << clientIP << ", dropping data\n";
            buf.clear();
            return out;
        }

        const size_t lastNewline = buf.rfind('\n');
        if (lastNewline == std::string::npos) return out;
        complete = buf.substr(0, lastNewline + 1);
        buf.erase(0, lastNewline + 1);
    }

    size_t pos;
    while ((pos = complete.find('\n')) != std::string::npos) {
        std::string line = complete.substr(0, pos);
        complete.erase(0, pos + 1);

        if (line.empty()) continue;
        if (line.back() == '\r') line.pop_back();   // tolerate \r\n

        try {
            out.push_back(nlohmann::json::parse(line).get<Request>());
        }
        catch (const nlohmann::json::exception &e) {
            std::cerr << "Bad request from " << clientIP << ": " << e.what() << "\n";
        }
        catch (const std::invalid_argument &e) {
            std::cerr << "Invalid command from " << clientIP << ": " << e.what() << "\n";
        }
    }
    return out;   // anything after the last '\n' stayed in buffers_ for next time
}

void Core::onIncomingRequest(const std::string &clientIP, const Request &req) {
    switch (req.command()) {
        case Command::Add:
        case Command::Update:
            onIncomingUpdateMsg(req);
            break;
        case Command::Delete:
            onIncomingDeleteMsg(req);
            break;
        case Command::Get:
            onIncomingGetMsg(clientIP, req);
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

void Core::onIncomingGetMsg(const std::string &clientIP, const Request &req) {
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

    // newline-delimited, same framing as incoming requests
    const std::string payload = response.dump() + "\n";

    auto sendResult = server.sendToClient(clientIP, payload.data(), payload.size());
    if (!sendResult.isSuccessful()) {
        std::cerr << "Failed to send GET response to " << clientIP << ": " << sendResult.message() << "\n";
    }
}

// observer callback. will be called when client disconnects
void Core::onClientDisconnected(const std::string &ip, const std::string &msg) {
    std::cout << "Client: " << ip << " disconnected. Reason: " << msg << "\n";
    std::lock_guard<std::mutex> lock(buffersMutex_);
    buffers_.erase(ip);
}

void Core::acceptLoop() {
    std::cout << "waiting for incoming clients...\n";
    while (_running) {
        acceptClient();
    }
}

void Core::stop() {
    if (!_running.exchange(false)) {
        return; // already stopped, or never started
    }
    if (_acceptThread.joinable()) {
        _acceptThread.join();
    }
    server.close();
}

Core::~Core() {
    stop();
}
