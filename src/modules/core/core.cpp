//
// Created by zahyrseferina on 10/9/26.
//

#include "core.h"

#include <iostream>

core::core(const int &port) : _port(port) {
}


int core::start() {
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
    observer1.wantedIP = "127.0.0.1";
    server.subscribe(observer1);

    // configure and register observer2
    observer2.incomingPacketHandler = [this](const std::string &clientIP, const char *msg, size_t size) {
        onIncomingMsg2(clientIP, msg, size);
    };
    observer2.disconnectionHandler = nullptr;
    // nullptr or not setting this means we don't care about disconnection event
    observer2.wantedIP = "10.88.0.11"; // use empty string instead to receive messages from any IP address
    server.subscribe(observer2);

    this->acceptClient();

    return 0;
}


// accept a single client.
// if we wish to accept multiple clients, call this function in a loop
// (you might want to use a thread to accept clients without blocking)
void core::acceptClient() {
    try {
        std::cout << "waiting for incoming client...\n";
        std::string clientIP = server.acceptClient(0);
        std::cout << "accepted new client with IP: " << clientIP << "\n" <<
                "== updated list of accepted clients ==" << "\n";
        server.printClients();
    } catch (const std::runtime_error &error) {
        std::cout << "Accepting client failed: " << error.what() << "\n";
    }
}

// observer callback. will be called for every new message received by clients
// with the requested IP address
void core::onIncomingMsg1(const std::string &clientIP, const char * msg, size_t size) {
    std::string msgStr = msg;
    // print client message
    std::cout << "Observer1 got client msg: " << msgStr << "\n";
}

// observer callback. will be called for every new message received by clients
// with the requested IP address
void core::onIncomingMsg2(const std::string &clientIP, const char * msg, size_t size) {
    std::string msgStr = msg;
    // print client message
    std::cout << "Observer2 got client msg: " << msgStr << "\n";
}

// observer callback. will be called when client disconnects
void core::onClientDisconnected(const std::string &ip, const std::string &msg) {
    std::cout << "Client: " << ip << " disconnected. Reason: " << msg << "\n";
}

core::~core() {
    server.close();
}
