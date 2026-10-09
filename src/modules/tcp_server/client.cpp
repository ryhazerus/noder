#include <cstdio>
#include <cstring>
#include <cerrno>
#include <unistd.h>
#include <stdexcept>
#include <sys/socket.h>
#include <iostream>

#include "client.h"
#include "common.h"

using namespace iron;

Client::Client(int fileDescriptor) {
    _sockfd.set(fileDescriptor);
    setConnected(false);
}

bool Client::operator==(const Client & other) const {
    if ((this->_sockfd.get() == other._sockfd.get()) &&
        (this->_id == other._id) ) {
        return true;
    }
    return false;
}

void Client::startListen() {
    setConnected(true);
    _receiveThread = new std::thread(&Client::receiveTask, this);
}

void Client::send(const char *msg, size_t msgSize) const {
    // ::send may write only part of the buffer when the socket's send buffer is full,
    // so keep going until everything is out
    size_t totalSent = 0;
    while (totalSent < msgSize) {
        // MSG_NOSIGNAL: a client that already hung up gives EPIPE instead of a SIGPIPE that kills the server
        const ssize_t numBytesSent = ::send(_sockfd.get(), msg + totalSent, msgSize - totalSent, MSG_NOSIGNAL);

        if (numBytesSent < 0) {
            if (errno == EINTR) continue;
            throw std::runtime_error(strerror(errno));
        }
        totalSent += static_cast<size_t>(numBytesSent);
    }
}

/*
 * Receive client packets, and notify user
 */
void Client::receiveTask() {
    while(isConnected()) {
        const fd_wait::Result waitResult = fd_wait::waitFor(_sockfd);

        if (waitResult == fd_wait::Result::FAILURE) {
            throw std::runtime_error(strerror(errno));
        } else if (waitResult == fd_wait::Result::TIMEOUT) {
            continue;
        }

        char receivedMessage[MAX_PACKET_SIZE];
        const ssize_t numOfBytesReceived = recv(_sockfd.get(), receivedMessage, MAX_PACKET_SIZE, 0);

        if(numOfBytesReceived < 1) {
            const bool clientClosedConnection = (numOfBytesReceived == 0);
            std::string disconnectionMessage;
            if (clientClosedConnection) {
                disconnectionMessage = "Client closed connection";
            } else {
                disconnectionMessage = strerror(errno);
            }
            setConnected(false);
            publishEvent(ClientEvent::DISCONNECTED, disconnectionMessage);
            return;
        } else {
            publishEvent(ClientEvent::INCOMING_MSG, std::string(receivedMessage, numOfBytesReceived));
        }
    }
}

void Client::publishEvent(ClientEvent clientEvent, const std::string &msg) {
    _eventHandlerCallback(*this, clientEvent, msg);
}

void Client::print() const {
    const std::string connected = isConnected() ? "True" : "False";
    std::cout << "-----------------\n" <<
              "IP address: " << getIp() << std::endl <<
              "Connected?: " << connected << std::endl <<
              "Socket FD: " << _sockfd.get() << std::endl;
}

void Client::terminateReceiveThread() {
    setConnected(false);
    if (_receiveThread) {
        _receiveThread->join();
        delete _receiveThread;
        _receiveThread = nullptr;
    }
}

void Client::close() {
    terminateReceiveThread();

    const bool closeFailed = (::close(_sockfd.get()) == -1);
    if (closeFailed) {
        throw std::runtime_error(strerror(errno));
    }
}