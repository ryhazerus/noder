#include "event_server.h"

#include <cerrno>
#include <cstring>
#include <iostream>
#include <stdexcept>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/epoll.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <unistd.h>

namespace {
    constexpr size_t kReadChunk = 64 * 1024;
    constexpr size_t kMaxLineSize = 1 << 20;        // a client that never sends '\n' gets disconnected
    constexpr size_t kMaxPendingOutput = 8 << 20;   // stop reading from a client that doesn't read its replies
    constexpr int kMaxEvents = 256;

    [[noreturn]] void throwErrno(const char *what) {
        throw std::runtime_error(std::string(what) + ": " + strerror(errno));
    }
}

namespace iron {
    event_server::event_server(line_handler_t handler) : handler_(std::move(handler)) {
    }

    event_server::~event_server() {
        stop();
    }

    void event_server::start(int port, unsigned numThreads) {
        if (numThreads == 0) numThreads = 1;

        running_ = true;
        for (unsigned i = 0; i < numThreads; ++i) {
            auto worker = std::make_unique<Worker>();
            try {
                setupWorker(*worker, port);
            } catch (...) {
                workers_.push_back(std::move(worker));   // so stop() closes whatever was opened
                stop();
                throw;
            }
            workers_.push_back(std::move(worker));
        }

        // start threads only after every socket is set up, so a failed bind leaves nothing running
        for (auto &worker : workers_) {
            worker->thread = std::thread(&event_server::run, this, std::ref(*worker));
        }
    }

    void event_server::setupWorker(Worker &worker, int port) {
        worker.listenFd = socket(AF_INET, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
        if (worker.listenFd == -1) throwErrno("socket");

        const int on = 1;
        setsockopt(worker.listenFd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
        // every worker binds the same port; the kernel spreads incoming connections over them
        if (setsockopt(worker.listenFd, SOL_SOCKET, SO_REUSEPORT, &on, sizeof(on)) == -1) throwErrno("SO_REUSEPORT");

        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_ANY);
        address.sin_port = htons(port);
        if (bind(worker.listenFd, reinterpret_cast<sockaddr *>(&address), sizeof(address)) == -1) throwErrno("bind");
        if (listen(worker.listenFd, SOMAXCONN) == -1) throwErrno("listen");

        worker.epollFd = epoll_create1(EPOLL_CLOEXEC);
        if (worker.epollFd == -1) throwErrno("epoll_create1");

        worker.wakeFd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
        if (worker.wakeFd == -1) throwErrno("eventfd");

        for (const int fd : {worker.listenFd, worker.wakeFd}) {
            epoll_event event{};
            event.events = EPOLLIN;
            event.data.fd = fd;
            if (epoll_ctl(worker.epollFd, EPOLL_CTL_ADD, fd, &event) == -1) throwErrno("epoll_ctl");
        }
    }

    void event_server::stop() {
        if (!running_.exchange(false)) return;

        for (auto &worker : workers_) {
            if (worker->wakeFd != -1) {
                const uint64_t one = 1;
                (void) !write(worker->wakeFd, &one, sizeof(one));
            }
        }
        for (auto &worker : workers_) {
            if (worker->thread.joinable()) worker->thread.join();

            for (auto &[fd, conn] : worker->connections) ::close(fd);
            worker->connections.clear();
            for (const int fd : {worker->listenFd, worker->wakeFd, worker->epollFd}) {
                if (fd != -1) ::close(fd);
            }
        }
        workers_.clear();
    }

    void event_server::run(Worker &worker) {
        epoll_event events[kMaxEvents];

        while (running_) {
            const int count = epoll_wait(worker.epollFd, events, kMaxEvents, -1);
            if (count == -1) {
                if (errno == EINTR) continue;
                std::cerr << "epoll_wait failed: " << strerror(errno) << "\n";
                return;
            }

            for (int i = 0; i < count; ++i) {
                const int fd = events[i].data.fd;

                if (fd == worker.wakeFd) continue;   // stop() was called; the while condition handles it
                if (fd == worker.listenFd) {
                    acceptAll(worker);
                    continue;
                }

                const auto it = worker.connections.find(fd);
                if (it == worker.connections.end()) continue;   // closed earlier in this batch
                Connection &conn = *it->second;

                bool alive = true;
                // EPOLLHUP/EPOLLERR: recv reports what happened (0 or an error), so handle them as readable
                if (events[i].events & (EPOLLIN | EPOLLHUP | EPOLLERR)) alive = onReadable(worker, conn);
                if (alive && (events[i].events & EPOLLOUT)) alive = flush(worker, conn);
                if (!alive) closeConnection(worker, fd);
            }
        }
    }

    void event_server::acceptAll(Worker &worker) {
        // the listening socket is non-blocking: keep accepting until the queue is empty
        while (true) {
            const int fd = accept4(worker.listenFd, nullptr, nullptr, SOCK_NONBLOCK | SOCK_CLOEXEC);
            if (fd == -1) {
                if (errno == EAGAIN || errno == EWOULDBLOCK) return;
                if (errno == EINTR || errno == ECONNABORTED) continue;
                // EMFILE etc: out of file descriptors. Leave the rest queued and try on the next wakeup
                std::cerr << "accept failed: " << strerror(errno) << "\n";
                return;
            }

            // replies are small; don't let Nagle hold them back waiting for an ACK
            const int on = 1;
            setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &on, sizeof(on));

            auto conn = std::make_unique<Connection>();
            conn->fd = fd;
            conn->events = EPOLLIN;

            epoll_event event{};
            event.events = conn->events;
            event.data.fd = fd;
            if (epoll_ctl(worker.epollFd, EPOLL_CTL_ADD, fd, &event) == -1) {
                std::cerr << "epoll_ctl add failed: " << strerror(errno) << "\n";
                ::close(fd);
                continue;
            }
            worker.connections.emplace(fd, std::move(conn));
        }
    }

    // returns false when the connection should be closed
    bool event_server::onReadable(Worker &worker, Connection &conn) {
        // read everything available, then handle all complete lines and send the replies in one go
        while (true) {
            // recv straight into the buffer. resize_and_overwrite (C++23) skips the zero-fill
            // that resize() would do on 64KB for every read
            const size_t oldSize = conn.in.size();
            ssize_t n = 0;
            conn.in.resize_and_overwrite(oldSize + kReadChunk, [&](char *data, size_t) {
                n = recv(conn.fd, data + oldSize, kReadChunk, 0);
                return oldSize + (n > 0 ? static_cast<size_t>(n) : 0);
            });

            if (n > 0) {
                if (static_cast<size_t>(n) < kReadChunk) break;   // socket drained
                continue;
            }
            if (n == 0) return false;   // client closed the connection
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            return false;               // ECONNRESET etc
        }

        processLines(conn);

        if (conn.in.size() > kMaxLineSize) {
            std::cerr << "line too long, closing connection\n";
            return false;
        }
        return flush(worker, conn);
    }

    void event_server::processLines(Connection &conn) {
        // walk the buffer with an offset and erase the consumed part once at the end;
        // erasing after every line would move the rest of the buffer each time (O(n^2) for pipelined requests)
        size_t start = 0;
        size_t newline;
        while ((newline = conn.in.find('\n', start)) != std::string::npos) {
            std::string_view line(conn.in.data() + start, newline - start);
            if (!line.empty() && line.back() == '\r') line.remove_suffix(1);   // tolerate \r\n
            if (!line.empty()) handler_(line, conn.out);
            start = newline + 1;
        }
        conn.in.erase(0, start);
    }

    // send as much pending output as the socket takes; returns false when the connection should be closed
    bool event_server::flush(Worker &worker, Connection &conn) {
        while (conn.outOffset < conn.out.size()) {
            const ssize_t n = send(conn.fd, conn.out.data() + conn.outOffset, conn.out.size() - conn.outOffset,
                                   MSG_NOSIGNAL);
            if (n > 0) {
                conn.outOffset += n;
                continue;
            }
            if (n == -1 && errno == EINTR) continue;
            if (n == -1 && (errno == EAGAIN || errno == EWOULDBLOCK)) break;   // socket buffer full
            return false;
        }

        if (conn.outOffset == conn.out.size()) {
            conn.out.clear();   // keeps its capacity, so the next replies don't allocate
            conn.outOffset = 0;
        }
        return updateEvents(worker, conn);
    }

    // only ask epoll for what we need: EPOLLOUT while output is pending,
    // and no EPOLLIN while too much output is pending (backpressure for clients that don't read)
    bool event_server::updateEvents(Worker &worker, Connection &conn) {
        const size_t pending = conn.out.size() - conn.outOffset;
        uint32_t wanted = 0;
        if (pending < kMaxPendingOutput) wanted |= EPOLLIN;
        if (pending > 0) wanted |= EPOLLOUT;

        if (wanted == conn.events) return true;

        epoll_event event{};
        event.events = wanted;
        event.data.fd = conn.fd;
        if (epoll_ctl(worker.epollFd, EPOLL_CTL_MOD, conn.fd, &event) == -1) return false;
        conn.events = wanted;
        return true;
    }

    void event_server::closeConnection(Worker &worker, int fd) {
        // closing the fd also removes it from the epoll set
        ::close(fd);
        worker.connections.erase(fd);
    }
}
