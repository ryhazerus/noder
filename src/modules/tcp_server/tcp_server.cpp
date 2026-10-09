
#include <functional>
#include <thread>
#include <algorithm>
#include <netinet/tcp.h>

#include "tcp_server.h"
#include "common.h"

using namespace  iron;

tcp_server::tcp_server() {
    _subscribers.reserve(10);
    _clients.reserve(10);
    _stopRemoveClientsTask = false;
}

tcp_server::~tcp_server() {
    close();
}

void tcp_server::subscribe(const server_observer_t & observer) {
    std::lock_guard<std::mutex> lock(_subscribersMtx);
    _subscribers.push_back(observer);
}

void tcp_server::printClients() {
    std::lock_guard<std::mutex> lock(_clientsMtx);
    if (_clients.empty()) {
        std::cout << "no connected clients\n";
    }
    for (const Client *client : _clients) {
        client->print();
    }
}

/**
 * Remove dead clients (disconnected) from clients vector periodically
 */
void tcp_server::removeDeadClients() {
    std::vector<Client*>::const_iterator clientToRemove;
    while (!_stopRemoveClientsTask) {
        {
            std::lock_guard<std::mutex> lock(_clientsMtx);
            do {
                clientToRemove = std::find_if(_clients.begin(), _clients.end(),
                                              [](Client *client) { return !client->isConnected(); });

                if (clientToRemove != _clients.end()) {
                    (*clientToRemove)->close();
                    delete *clientToRemove;
                    _clients.erase(clientToRemove);
                }
            } while (clientToRemove != _clients.end());
        }

        sleep(2);
    }
}

void tcp_server::terminateDeadClientsRemover() {
    if (_clientsRemoverThread) {
        _stopRemoveClientsTask = true;
        _clientsRemoverThread->join();
        delete _clientsRemoverThread;
        _clientsRemoverThread = nullptr;
    }
}

/**
 * Handle different client events. Subscriber callbacks should be short and fast, and must not
 * call other server functions to avoid deadlock
 */
void tcp_server::clientEventHandler(const Client &client, ClientEvent event, const std::string &msg) {
    switch (event) {
        case ClientEvent::DISCONNECTED: {
            publishClientDisconnected(client, msg);
            break;
        }
        case ClientEvent::INCOMING_MSG: {
            publishClientMsg(client, msg.c_str(), msg.size());
            break;
        }
    }
}

/*
 * Publish incomingPacketHandler client message to observer.
 * Observers get only messages that originated
 * from clients with IP address identical to
 * the specific observer requested IP
 */
void tcp_server::publishClientMsg(const Client & client, const char * msg, size_t msgSize) {
    // copy the list so handlers run without the lock, otherwise every client thread
    // waits on this mutex and requests are handled one at a time
    std::vector<server_observer_t> subscribers;
    {
        std::lock_guard<std::mutex> lock(_subscribersMtx);
        subscribers = _subscribers;
    }

    for (const server_observer_t& subscriber : subscribers) {
        if (subscriber.wantedIP == client.getIp() || subscriber.wantedIP.empty()) {
            if (subscriber.incomingPacketHandler) {
                subscriber.incomingPacketHandler(client.getId(), msg, msgSize);
            }
        }
    }
}

/*
 * Publish client disconnection to observer.
 * Observers get only notify about clients
 * with IP address identical to the specific
 * observer requested IP
 */
void tcp_server::publishClientDisconnected(const Client &client, const std::string &clientMsg) {
    std::vector<server_observer_t> subscribers;
    {
        std::lock_guard<std::mutex> lock(_subscribersMtx);
        subscribers = _subscribers;
    }

    for (const server_observer_t& subscriber : subscribers) {
        if (subscriber.wantedIP == client.getIp() || subscriber.wantedIP.empty()) {
            if (subscriber.disconnectionHandler) {
                subscriber.disconnectionHandler(client.getId(), clientMsg);
            }
        }
    }
}

/*
 * Bind port and start listening
 * Return tcp_ret_t
 */
pipe_ret_t tcp_server::start(int port, int maxNumOfClients, bool removeDeadClientsAutomatically) {
    if (removeDeadClientsAutomatically) {
        _clientsRemoverThread = new std::thread(&tcp_server::removeDeadClients, this);
    }
    try {
        initializeSocket();
        bindAddress(port);
        listenToClients(maxNumOfClients);
    } catch (const std::runtime_error &error) {
        return pipe_ret_t::failure(error.what());
    }
    return pipe_ret_t::success();
}

void tcp_server::initializeSocket() {
    _sockfd.set(socket(AF_INET, SOCK_STREAM, 0));
    const bool socketFailed = (_sockfd.get() == -1);
    if (socketFailed) {
        throw std::runtime_error(strerror(errno));
    }

    // set socket for reuse (otherwise might have to wait 4 minutes every time socket is closed)
    const int option = 1;
    setsockopt(_sockfd.get(), SOL_SOCKET, SO_REUSEADDR, &option, sizeof(option));
}

void tcp_server::bindAddress(int port) {
    memset(&_serverAddress, 0, sizeof(_serverAddress));
    _serverAddress.sin_family = AF_INET;
    _serverAddress.sin_addr.s_addr = htonl(INADDR_ANY);
    _serverAddress.sin_port = htons(port);

    const int bindResult = bind(_sockfd.get(), (struct sockaddr *)&_serverAddress, sizeof(_serverAddress));
    const bool bindFailed = (bindResult == -1);
    if (bindFailed) {
        throw std::runtime_error(strerror(errno));
    }
}

void tcp_server::listenToClients(int maxNumOfClients) {
    const int clientsQueueSize = maxNumOfClients;
    const bool listenFailed = (listen(_sockfd.get(), clientsQueueSize) == -1);
    if (listenFailed) {
        throw std::runtime_error(strerror(errno));
    }
}

/*
 * Accept and handle new client socket. To handle multiple clients, user must
 * call this function in a loop to enable the acceptance of more than one.
 * If timeout argument equal 0, this function is executed in blocking mode.
 * If timeout argument is > 0 then this function is executed in non-blocking
 * mode (async) and will quit after timeout seconds if no client tried to connect.
 * Return accepted client IP, or throw error if failed
 */
std::string tcp_server::acceptClient(uint timeout) {
    const pipe_ret_t waitingForClient = waitForClient(timeout);
    if (!waitingForClient.isSuccessful()) {
        throw std::runtime_error(waitingForClient.message());
    }

    socklen_t socketSize  = sizeof(_clientAddress);
    const int fileDescriptor = accept(_sockfd.get(), (struct sockaddr*)&_clientAddress, &socketSize);

    const bool acceptFailed = (fileDescriptor == -1);
    if (acceptFailed) {
        throw std::runtime_error(strerror(errno));
    }

    // disable Nagle's algorithm: otherwise a small reply sent while a previous one is still
    // unacknowledged waits for the client's delayed ACK (~40ms)
    const int noDelay = 1;
    setsockopt(fileDescriptor, IPPROTO_TCP, TCP_NODELAY, &noDelay, sizeof(noDelay));

    auto newClient = new Client(fileDescriptor);
    newClient->setIp(inet_ntoa(_clientAddress.sin_addr));
    // several connections can share an IP (e.g. all local clients are 127.0.0.1),
    // so the source port is what tells them apart
    newClient->setId(newClient->getIp() + ":" + std::to_string(ntohs(_clientAddress.sin_port)));
    using namespace std::placeholders;
    newClient->setEventsHandler(std::bind(&tcp_server::clientEventHandler, this, _1, _2, _3));
    newClient->startListen();

    std::lock_guard<std::mutex> lock(_clientsMtx);
    _clients.push_back(newClient);

    return newClient->getId();
}

pipe_ret_t tcp_server::waitForClient(uint32_t timeout) {
    if (timeout > 0) {
        const fd_wait::Result waitResult = fd_wait::waitFor(_sockfd, timeout);

        if (waitResult == fd_wait::Result::FAILURE) {
            return pipe_ret_t::failure(strerror(errno));
        } else if (waitResult == fd_wait::Result::TIMEOUT) {
            return pipe_ret_t::failure("Timeout waiting for client");
        }
    }

    return pipe_ret_t::success();
}

/*
 * Send message to all connected clients.
 * Return true if message was sent successfully to all clients
 */
pipe_ret_t tcp_server::sendToAllClients(const char * msg, size_t size) {
    std::lock_guard<std::mutex> lock(_clientsMtx);

    for (const Client *client : _clients) {
        pipe_ret_t sendingResult = sendToClient(*client, msg, size);
        if (!sendingResult.isSuccessful()) {
            return sendingResult;
        }
    }

    return pipe_ret_t::success();
}

/*
 * Send message to specific client (determined by client id, "ip:port").
 * Return true if message was sent successfully
 */
pipe_ret_t tcp_server::sendToClient(const Client & client, const char * msg, size_t size){
    try{
        client.send(msg, size);
    } catch (const std::runtime_error &error) {
        return pipe_ret_t::failure(error.what());
    }

    return pipe_ret_t::success();
}

pipe_ret_t tcp_server::sendToClient(const std::string & clientId, const char * msg, size_t size) {
    std::lock_guard<std::mutex> lock(_clientsMtx);

    const auto clientIter = std::find_if(_clients.begin(), _clients.end(),
         [&clientId](Client *client) { return client->getId() == clientId; });

    if (clientIter == _clients.end()) {
        return pipe_ret_t::failure("client not found");
    }

    const Client &client = *(*clientIter);
    return sendToClient(client, msg, size);
}

/*
 * Close server and clients resources.
 * Return true is successFlag, false otherwise
 */
pipe_ret_t tcp_server::close() {
    terminateDeadClientsRemover();
    { // close clients
        std::lock_guard<std::mutex> lock(_clientsMtx);

        for (Client * client : _clients) {
            try {
                client->close();
            } catch (const std::runtime_error& error) {
                return pipe_ret_t::failure(error.what());
            }
        }
        _clients.clear();
    }

    { // close server
        const int closeServerResult = ::close(_sockfd.get());
        const bool closeServerFailed = (closeServerResult == -1);
        if (closeServerFailed) {
            return pipe_ret_t::failure(strerror(errno));
        }
    }

    return pipe_ret_t::success();
}