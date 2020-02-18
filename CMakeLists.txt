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