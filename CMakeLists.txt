cmake_minimum_required(VERSION 4.3)
project(node_connector)

set(CMAKE_CXX_STANDARD 26)

# default to an optimized build; an unoptimized build runs at about half the speed
if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release)
endif()

include(FetchContent)
FetchContent_Declare(json
        URL https://github.com/nlohmann/json/releases/download/v3.12.0/json.tar.xz
        SYSTEM)
FetchContent_MakeAvailable(json)


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
        src/modules/core/Core.cpp
        src/modules/core/Core.h
        src/modules/models/request.cpp
        src/modules/models/request.h)

target_link_libraries(node_connector PRIVATE nlohmann_json::nlohmann_json)
