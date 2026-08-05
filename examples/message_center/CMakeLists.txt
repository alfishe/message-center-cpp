cmake_minimum_required(VERSION 3.14)
project(message_center_example CXX)

set(CMAKE_CXX_STANDARD 11)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Threads REQUIRED)

add_executable(message_center_example
    main.cpp
    ../../src/messagecenter.cpp
    ../../src/eventqueue.cpp
)

target_include_directories(message_center_example PRIVATE ../../src ../../include)
target_link_libraries(message_center_example PRIVATE Threads::Threads)
