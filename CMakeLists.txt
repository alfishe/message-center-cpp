# CMake entry point
cmake_minimum_required (VERSION 3.13)
project (message-center)

# Build options
option(BUILD_TESTS "Build unit tests" ON)
option(BENCHMARK "Execute benchmarks" ON)

# Default to Release mode
if (NOT CMAKE_BUILD_TYPE)
    set (CMAKE_BUILD_TYPE Release)
endif (NOT CMAKE_BUILD_TYPE)

message(STATUS "CMAKE_BUILD_TYPE: ${CMAKE_BUILD_TYPE}")

# Dealing with
# "cl : Command line warning D9025 : overriding '/MDd' with '/MTd'" and
# "cl : Command line warning D9025 : overriding '/MD' with '/MT'"
# warnings
if (MSVC)
    message("CMAKE_C_FLAGS_RELEASE: ${CMAKE_C_FLAGS_RELEASE}")
    message("CMAKE_C_FLAGS_DEBUG: ${CMAKE_C_FLAGS_DEBUG}")
    string(REPLACE "/MD"  "/MT"  CMAKE_C_FLAGS_RELEASE "${CMAKE_C_FLAGS_RELEASE}")
    string(REPLACE "/MDd" "/MTd" CMAKE_C_FLAGS_DEBUG   "${CMAKE_C_FLAGS_DEBUG}")
    message("CMAKE_C_FLAGS_RELEASE after: ${CMAKE_C_FLAGS_RELEASE}")
    message("CMAKE_C_FLAGS_DEBUG after: ${CMAKE_C_FLAGS_DEBUG}")
endif (MSVC)

# Main project
add_subdirectory(src)

# Unit tests
if (BUILD_TESTS)
    add_subdirectory(tests)
endif (BUILD_TESTS)

# Benchmarks
if (BENCHMARK)
    add_subdirectory(benchmarks)
endif (BENCHMARK)