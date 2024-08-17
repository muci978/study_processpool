cmake_minimum_required(VERSION 3.22.1)
project(processpool)
set(CMAKE_CXX_STANDARD 11)

file(GLOB SRC ${CMAKE_CURRENT_SOURCE_DIR}/*.cpp)
include_directories(${CMAKE_CURRENT_SOURCE_DIR})
set(EXECUTABLE_OUTPUT_PATH ${CMAKE_CURRENT_SOURCE_DIR}/bin)

add_executable(cgi_server ${SRC})