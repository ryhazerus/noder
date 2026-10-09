//
// Created by zahyrseferina on 10/9/26.
//

#ifndef NODE_CONNECTOR_CORE_H
#define NODE_CONNECTOR_CORE_H

#include "../store/store.h"
#include "../tcp_server/tcp_server.h"
#include "../models/request.h"

#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

class Core {
private:
    // TCP server configurations
    int _port;
    iron::tcp_server server{};
    server_observer_t observer;
    std::atomic<bool> _running{false};
    std::thread _acceptThread;
    std::unordered_map<std::string, std::string> buffers_;   // partial data per client
    std::mutex buffersMutex_;
    // Actual Store
    // Lel this is going to be fun
    iron::store _store{};


    void acceptLoop();

    std::vector<Request> parseRequests(const std::string &clientIP, const char *msg, size_t size);
public:
    Core(const int &port);

    int start();

    void acceptClient();

    void onIncomingRequest(const std::string &clientIP, const Request &req);

     void onIncomingUpdateMsg(const Request &req);

     void onIncomingDeleteMsg(const Request &req);

    void onIncomingGetMsg(const std::string &clientIP, const Request &req);

    void onClientDisconnected(const std::string &ip, const std::string &msg);

    void stop();

    ~Core();
};


#endif //NODE_CONNECTOR_CORE_H
