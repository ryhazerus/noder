//
// Created by zahyrseferina on 10/9/26.
//

#ifndef NODE_CONNECTOR_CORE_H
#define NODE_CONNECTOR_CORE_H

#include "../tcp_server/tcp_server.h"

class Core {
private:

    // TCP server configurations
    int _port;
    iron::tcp_server server{};
    server_observer_t observer1, observer2;


public:
    Core(const int &port);

    int start();
    void acceptClient();
    void onIncomingMsg2(const std::string &clientIP, const char * msg, size_t size);
    void onIncomingMsg1(const std::string &clientIP, const char * msg, size_t size);
    void onClientDisconnected(const std::string &ip, const std::string &msg);

    ~Core();
};


#endif //NODE_CONNECTOR_CORE_H
