cmake_minimum_required(VERSION 3.14)
project(message_center_examples CXX)

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Threads REQUIRED)

add_subdirectory(message_center)
add_subdirectory(fast_queue)
add_subdirectory(batch_queue)
add_subdirectory(emulator_queue)
add_subdirectory(turbo_benchmark)
