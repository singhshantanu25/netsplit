cmake_minimum_required(VERSION 3.16)
project(netsplit CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(CURL REQUIRED)

# nlohmann/json (header-only), downloaded once when CMake configures.
include(FetchContent)
if(POLICY CMP0135)
    cmake_policy(SET CMP0135 NEW)   # use download time for extracted files
endif()
FetchContent_Declare(json
    URL https://github.com/nlohmann/json/releases/download/v3.11.3/json.tar.xz)
FetchContent_MakeAvailable(json)

add_executable(netsplit src/main.cpp src/http_client.cpp src/probe.cpp src/planner.cpp src/storage.cpp src/hasher.cpp src/worker.cpp src/resume_state.cpp src/stop.cpp src/network.cpp)
target_include_directories(netsplit PRIVATE include)
target_link_libraries(netsplit PRIVATE CURL::libcurl nlohmann_json::nlohmann_json)
