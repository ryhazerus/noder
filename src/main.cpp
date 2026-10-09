#include <memory>
#include <signal.h>

#include "modules/core/Core.h"

auto main(int argc, char *argv[]) -> int {

    // Block SIGINT/SIGTERM before any thread is created. Every thread inherits this mask,
    // so the signal stays pending until sigwait() below picks it up.
    sigset_t signals;
    sigemptyset(&signals);
    sigaddset(&signals, SIGINT);
    sigaddset(&signals, SIGTERM);
    pthread_sigmask(SIG_BLOCK, &signals, nullptr);

    const auto core = std::make_shared<Core>(4321);
    if (core->start() != 0) {
        return EXIT_FAILURE;
    }

    int sig = 0;
    sigwait(&signals, &sig); // main sleeps here until Ctrl+C or kill
    std::cout << "\nreceived signal " << sig << ", shutting down\n";

    core->stop();
    return 0;
}
