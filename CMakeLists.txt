cmake_minimum_required(VERSION 3.16)
project(netsplit CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(CURL REQUIRED)

add_executable(netsplit main.cpp)
target_link_libraries(netsplit PRIVATE CURL::libcurl)
