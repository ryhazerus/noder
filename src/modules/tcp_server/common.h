#pragma once

#include <cstdint>
#include <cstdio>
#include "file_descriptor.h"

#define MAX_PACKET_SIZE 4096

namespace fd_wait {
    enum Result {
        FAILURE,
        TIMEOUT,
        SUCCESS
    };

    Result waitFor(const FileDescriptor &fileDescriptor, uint32_t timeoutSeconds =1);
};