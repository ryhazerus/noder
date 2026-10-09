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
            onIncomingRequest(req);
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
        std::cout << "accepted new client with IP: " << clientIP << "\n" <<
                "== updated list of accepted clients ==" << "\n";
        server.printClients();
    } catch (const std::runtime_error &error) {
        if (std::string(error.what()) != "Timeout waiting for client") {
            std::cout << "Accepting client failed: " << error.what() << "\n";
        }
    }
}

std::vector<Request> Core::parseRequests(const std::string &clientIP, const char *msg, size_t size) {
    std::vector<Request> out;

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

    size_t pos;
    while ((pos = buf.find('\n')) != std::string::npos) {
        std::string line = buf.substr(0, pos);
        buf.erase(0, pos + 1);

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
    return out;   // anything after the last '\n' stays in buf for next time
}

void Core::onIncomingRequest(const Request &req) {
    switch (req.command()) {
        case Command::Add:
        case Command::Update:
            onIncomingUpdateMsg(req);
            break;
        case Command::Delete:
            onIncomingDeleteMsg(req);
            break;
        default:
            std::cerr << "Unhandled command for key: " << req.key() << "\n";
            break;
    }
}

void Core::onIncomingUpdateMsg(const Request &req) {
    std::cout << "Update Query: " << req.key() << " = " << req.value() << "\n";
    _store.add_record(req.key(), req.value());
}

void Core::onIncomingDeleteMsg(const Request &req) {
    std::cout << "Delete Query: " << req.key() << "\n";
    _store.delete_record(req.key());
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
