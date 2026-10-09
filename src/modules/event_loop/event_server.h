#pragma once

#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>

namespace iron {
    /**
     * Newline-delimited TCP server built on epoll.
     *
     * Runs one worker thread per requested core. Each worker owns its own listening socket
     * (SO_REUSEPORT lets the kernel spread new connections over them), its own epoll instance
     * and all connections it accepted, so workers never share connection state or take locks.
     */
    class event_server {
    public:
        // called once per complete line (without the '\n'); append any reply to `out`
        using line_handler_t = std::function<void(std::string_view line, std::string &out)>;

        explicit event_server(line_handler_t handler);
        ~event_server();

        event_server(const event_server &) = delete;
        event_server &operator=(const event_server &) = delete;

        // throws std::runtime_error when a socket can't be set up
        void start(int port, unsigned numThreads);
        void stop();

    private:
        struct Connection {
            int fd = -1;
            std::string in;          // received bytes not yet forming a complete line
            std::string out;         // replies not yet written to the socket
            size_t outOffset = 0;    // how much of `out` has already been sent
            uint32_t events = 0;     // epoll events currently registered
        };

        struct Worker {
            int epollFd = -1;
            int listenFd = -1;
            int wakeFd = -1;         // eventfd, written by stop() to wake up epoll_wait
            std::thread thread;
            std::unordered_map<int, std::unique_ptr<Connection>> connections;
        };

        line_handler_t handler_;
        std::vector<std::unique_ptr<Worker>> workers_;
        std::atomic<bool> running_{false};

        void setupWorker(Worker &worker, int port);
        void run(Worker &worker);
        void acceptAll(Worker &worker);
        bool onReadable(Worker &worker, Connection &conn);
        void processLines(Connection &conn);
        bool flush(Worker &worker, Connection &conn);
        bool updateEvents(Worker &worker, Connection &conn);
        void closeConnection(Worker &worker, int fd);
    };
}
