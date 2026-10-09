#include <cstdlib>
#include <iostream>
#include <memory>
#include <signal.h>
#include <thread>

#include "modules/core/Core.h"

auto main(int argc, char *argv[]) -> int {

    // Block SIGINT/SIGTERM before any thread is created. Every thread inherits this mask,
    // so the signal stays pending until sigwait() below picks it up.
    sigset_t signals;
    sigemptyset(&signals);
    sigaddset(&signals, SIGINT);
    sigaddset(&signals, SIGTERM);
    pthread_sigmask(SIG_BLOCK, &signals, nullptr);

    // usage: node_connector [worker threads], defaults to one per CPU core
    unsigned threads = std::thread::hardware_concurrency();
    if (argc > 1) threads = static_cast<unsigned>(std::strtoul(argv[1], nullptr, 10));
    if (threads == 0) threads = 1;

    const auto core = std::make_shared<Core>(4321, threads);
    if (core->start() != 0) {
        return EXIT_FAILURE;
    }

    int sig = 0;
    sigwait(&signals, &sig); // main sleeps here until Ctrl+C or kill
    std::cout << "\nreceived signal " << sig << ", shutting down\n";

    core->stop();
    return 0;
}
