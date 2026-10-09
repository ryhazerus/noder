cmake_minimum_required(VERSION 4.3)
project(node_connector)

set(CMAKE_CXX_STANDARD 26)


add_executable(node_connector src/main.cpp
        src/modules/store/store.cpp
        src/modules/store/store.h
        src/modules/tcp_server/tcp_server.cpp
        src/modules/tcp_server/tcp_server.h
        src/modules/tcp_server/pipe_ret_t.cpp
        src/modules/tcp_server/pipe_ret_t.h
        src/modules/tcp_server/server_observer.h
        src/modules/tcp_server/client.cpp
        src/modules/tcp_server/client.h
        src/modules/tcp_server/file_descriptor.h
        src/modules/tcp_server/common.cpp
        src/modules/tcp_server/common.h
        src/modules/tcp_server/client_event.h
        src/modules/tcp_server/tcp_client.cpp
        src/modules/tcp_server/tcp_client.h
        src/modules/tcp_server/client_observer.h
        src/modules/core/core.cpp
        src/modules/core/core.h)


