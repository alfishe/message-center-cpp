cmake_minimum_required(VERSION 3.14)
project(turbo_benchmark CXX)

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Threads REQUIRED)

add_executable(turbo_benchmark main.cpp)
target_link_libraries(turbo_benchmark PRIVATE Threads::Threads)
