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
        src/modules/event_loop/event_server.cpp
        src/modules/event_loop/event_server.h
        src/modules/core/Core.cpp
        src/modules/core/Core.h
        src/modules/models/request.cpp
        src/modules/models/request.h)

target_link_libraries(node_connector PRIVATE nlohmann_json::nlohmann_json)
