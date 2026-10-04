cmake_minimum_required(VERSION 3.16)
project(netsplit CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(CURL REQUIRED)

add_executable(netsplit src/main.cpp src/http_client.cpp src/probe.cpp src/planner.cpp)
target_include_directories(netsplit PRIVATE include)
target_link_libraries(netsplit PRIVATE CURL::libcurl)