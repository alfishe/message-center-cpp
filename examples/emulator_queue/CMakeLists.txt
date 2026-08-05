cmake_minimum_required(VERSION 3.14)
project(emulator_queue_example CXX)

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_executable(emulator_queue_example main.cpp)
target_link_libraries(emulator_queue_example PRIVATE pthread)
