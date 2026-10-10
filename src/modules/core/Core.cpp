//
// Created by zahyrseferina on 10/9/26.
//

#include "Core.h"

#include <iostream>
#include <stdexcept>

#include "../protocol/json_protocol.h"
#include "../protocol/resp_protocol.h"

namespace {
    // per-connection protocol, kept in the connection's state by the event server
    enum Protocol : uint32_t {
        Unknown = 0,
        Json,
        Resp,
    };
}

Core::Core(int port, unsigned threads)
    : _port(port), _threads(threads),
      _server([this](std::string_view input, std::string &out, uint32_t &protocol) {
          return onData(input, out, protocol);
      }) {
}

int Core::start() {
    try {
        _server.start(_port, _threads);
    } catch (const std::runtime_error &error) {
        std::cout << "Server start failed: " << error.what() << std::endl;
        return EXIT_FAILURE;
    }
    std::cout << "Server started on port " << _port << " with " << _threads << " worker threads" << std::endl;
    return 0;
}

// runs on a worker thread; several workers call this at the same time
size_t Core::onData(std::string_view input, std::string &out, uint32_t &protocol) {
    if (protocol == Unknown) {
        // the first real byte tells the protocols apart: JSON requests start with '{',
        // RESP with '*' (or a plain-text inline command like "PING")
        const size_t first = input.find_first_not_of(" \t\r\n");
        if (first == std::string_view::npos) {
            return input.size();
        }
        protocol = input[first] == '{' ? Json : Resp;
    }

    if (protocol == Json) {
        return iron::json_protocol::process(input, out, _store);
    }
    return iron::resp_protocol::process(input, out, _store);
}

void Core::stop() {
    _server.stop();
}

Core::~Core() {
    stop();
}
