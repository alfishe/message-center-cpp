# CMake entry point
cmake_minimum_required (VERSION 3.13)
project (message-center)

if (CMAKE_CURRENT_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    set(MC_IS_TOP_LEVEL ON)
else ()
    set(MC_IS_TOP_LEVEL OFF)
endif ()

# Build options (default ON for top-level builds, OFF when embedded via add_subdirectory/FetchContent)
option(BUILD_TESTS "Build unit tests" ${MC_IS_TOP_LEVEL})
option(BENCHMARK "Execute benchmarks" ${MC_IS_TOP_LEVEL})
option(BUILD_BENCHMARKS "Alias for BENCHMARK" ${BENCHMARK})
option(BUILD_EXAMPLES "Build examples" OFF)

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
if (BENCHMARK AND BUILD_BENCHMARKS)
    add_subdirectory(benchmarks)
endif ()

# Examples
if (BUILD_EXAMPLES)
    add_subdirectory(examples)
endif (BUILD_EXAMPLES)