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
     * TCP server built on epoll. It only moves bytes; the handler decides what they mean.
     *
     * Runs one worker thread per requested core. Each worker owns its own listening socket
     * (SO_REUSEPORT lets the kernel spread new connections over them), its own epoll instance
     * and all connections it accepted, so workers never share connection state or take locks.
     */
    class event_server {
    public:
        // Gets everything received so far that hasn't been consumed yet, appends replies to `out`
        // and returns how many bytes it consumed (an incomplete request at the end stays for next time),
        // or kClose to drop the connection after sending what's in `out`.
        // `state` belongs to the connection and starts at 0; the handler can keep anything in it.
        using data_handler_t = std::function<size_t(std::string_view input, std::string &out, uint32_t &state)>;

        static constexpr size_t kClose = static_cast<size_t>(-1);

        explicit event_server(data_handler_t handler);
        ~event_server();

        event_server(const event_server &) = delete;
        event_server &operator=(const event_server &) = delete;

        // throws std::runtime_error when a socket can't be set up
        void start(int port, unsigned numThreads);
        void stop();

    private:
        struct Connection {
            int fd = -1;
            std::string in;          // received bytes the handler hasn't consumed yet
            std::string out;         // replies not yet written to the socket
            size_t outOffset = 0;    // how much of `out` has already been sent
            uint32_t events = 0;     // epoll events currently registered
            uint32_t state = 0;      // owned by the handler (e.g. which protocol this client speaks)
        };

        struct Worker {
            int epollFd = -1;
            int listenFd = -1;
            int wakeFd = -1;         // eventfd, written by stop() to wake up epoll_wait
            std::thread thread;
            std::unordered_map<int, std::unique_ptr<Connection>> connections;
        };

        data_handler_t handler_;
        std::vector<std::unique_ptr<Worker>> workers_;
        std::atomic<bool> running_{false};

        void setupWorker(Worker &worker, int port);
        void run(Worker &worker);
        void acceptAll(Worker &worker);
        bool onReadable(Worker &worker, Connection &conn);
        bool flush(Worker &worker, Connection &conn);
        bool updateEvents(Worker &worker, Connection &conn);
        void closeConnection(Worker &worker, int fd);
    };
}
