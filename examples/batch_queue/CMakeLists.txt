cmake_minimum_required(VERSION 3.14)
project(batch_queue_example CXX)

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Threads REQUIRED)

add_executable(batch_queue_example main.cpp)
target_link_libraries(batch_queue_example PRIVATE Threads::Threads)
