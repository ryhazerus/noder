#include <cstdint>
#include "file_descriptor.h"
#include "common.h"

#include <cerrno>
#include <poll.h>

namespace fd_wait {
    /**
     * monitor file descriptor and wait for I/O operation
     */
    Result waitFor(const FileDescriptor &fileDescriptor, uint32_t timeoutSeconds) {
        // poll instead of select: select's fd_set only holds fds below 1024 (FD_SETSIZE),
        // so it fails once the server has ~1000 connections open
        pollfd pfd{};
        pfd.fd = fileDescriptor.get();
        pfd.events = POLLIN;
        const int pollRet = poll(&pfd, 1, static_cast<int>(timeoutSeconds * 1000));

        if (pollRet == -1) {
            // interrupted by a signal: nothing is wrong, the caller just waits again
            return errno == EINTR ? Result::TIMEOUT : Result::FAILURE;
        } else if (pollRet == 0) {
            return Result::TIMEOUT;
        }
        // POLLIN, or POLLHUP/POLLERR: either way the following recv/accept reports what happened
        return Result::SUCCESS;
    }
}