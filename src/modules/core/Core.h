//
// Created by zahyrseferina on 10/9/26.
//

#ifndef NODE_CONNECTOR_CORE_H
#define NODE_CONNECTOR_CORE_H

#include "../store/store.h"
#include "../event_loop/event_server.h"
#include "../models/request.h"

#include <string>
#include <string_view>

class Core {
private:
    int _port;
    unsigned _threads;
    // Actual Store
    // Lel this is going to be fun
    iron::store _store{};
    iron::event_server server;

    // one request line in, reply (if any) appended to `out`
    void handleLine(std::string_view line, std::string &out);

    void onIncomingUpdateMsg(const Request &req);

    void onIncomingDeleteMsg(const Request &req);

    void onIncomingGetMsg(const Request &req, std::string &out);

public:
    Core(int port, unsigned threads);

    int start();

    void stop();

    ~Core();
};


#endif //NODE_CONNECTOR_CORE_H
