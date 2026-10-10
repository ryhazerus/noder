//
// Created by zahyrseferina on 10/9/26.
//

#ifndef NODE_CONNECTOR_CORE_H
#define NODE_CONNECTOR_CORE_H

#include "../store/store.h"
#include "../event_loop/event_server.h"

#include <string>
#include <string_view>

class Core {
private:
    int _port;
    unsigned _threads;
    // Actual Store
    // Lel this is going to be fun
    iron::store _store{};
    iron::event_server _server;

    // bytes from one connection in, replies appended to `out`; see event_server::data_handler_t
    size_t onData(std::string_view input, std::string &out, uint32_t &protocol);

public:
    Core(int port, unsigned threads);

    int start();

    void stop();

    ~Core();
};


#endif //NODE_CONNECTOR_CORE_H
