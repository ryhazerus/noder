//
// Created by zahyrseferina on 10/9/26.
//

#include "Core.h"

#include <iostream>

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

    // configure and register observer1
    observer1.incomingPacketHandler = [this](const std::string &clientIP, const char *msg, size_t size) {
        onIncomingMsg1(clientIP, msg, size);
    };
    observer1.disconnectionHandler = [this](const std::string &ip, const std::string &msg) {
        onClientDisconnected(ip, msg);
    };
    observer1.wantedIP = "";
    server.subscribe(observer1);

    // configure and register observer2
    observer2.incomingPacketHandler = [this](const std::string &clientIP, const char *msg, size_t size) {
        onIncomingMsg2(clientIP, msg, size);
    };
    observer2.disconnectionHandler = nullptr;
    // nullptr or not setting this means we don't care about disconnection event
    observer2.wantedIP = ""; // use empty string instead to receive messages from any IP address
    server.subscribe(observer2);

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

// observer callback. will be called for every new message received by clients
// with the requested IP address
void Core::onIncomingMsg1(const std::string &clientIP, const char * msg, size_t size) {
    std::string msgStr = msg;
    // print client message
    std::cout << "Observer1 got client msg: " << msgStr << "\n";
}

// observer callback. will be called for every new message received by clients
// with the requested IP address
void Core::onIncomingMsg2(const std::string &clientIP, const char * msg, size_t size) {
    std::string msgStr = msg;
    // print client message
    std::cout << "Observer2 got client msg: " << msgStr << "\n";
}

// observer callback. will be called when client disconnects
void Core::onClientDisconnected(const std::string &ip, const std::string &msg) {
    std::cout << "Client: " << ip << " disconnected. Reason: " << msg << "\n";
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