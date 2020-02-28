# CMake entry point
cmake_minimum_required (VERSION 3.13)
project (message-center-benchmark)

# Specify the C++ standard
set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Default to Release mode
if (NOT CMAKE_BUILD_TYPE)
    set (CMAKE_BUILD_TYPE Release)
endif (NOT CMAKE_BUILD_TYPE)

message(STATUS "CMAKE_BUILD_TYPE: ${CMAKE_BUILD_TYPE}")

# Library root
get_filename_component(ROOT_DIR ../ ABSOLUTE BASE_DIR ${CMAKE_CURRENT_SOURCE_DIR})

# Allow CMake to find modules in /cmake
set(CMAKE_MODULE_PATH "${ROOT_DIR}/cmake" ${CMAKE_MODULE_PATH})

# Include CMake helper functions file
include(${ROOT_DIR}/cmake/func.cmake)


 # Google test (explicit sources added to prevent conflicts when gtest and benchmark targets built simultaneously from top level CMakeLists.txt)
get_filename_component(GOOGLETEST_DIR ${ROOT_DIR}/lib/googletest ABSOLUTE BASE_DIR ${CMAKE_CURRENT_SOURCE_DIR})
set(GOOGLETEST_PATH ${GOOGLETEST_DIR} CACHE PATH "")                            # Hint for Google Benchmark where to find googletest sources
set (GOOGLETEST_FILES
    ${GOOGLETEST_DIR}/googletest/src/gtest.cc
    ${GOOGLETEST_DIR}/googletest/src/gtest-death-test.cc
    ${GOOGLETEST_DIR}/googletest/src/gtest-filepath.cc
    ${GOOGLETEST_DIR}/googletest/src/gtest-matchers.cc
    ${GOOGLETEST_DIR}/googletest/src/gtest-port.cc
    ${GOOGLETEST_DIR}/googletest/src/gtest-printers.cc
    ${GOOGLETEST_DIR}/googletest/src/gtest-test-part.cc
    ${GOOGLETEST_DIR}/googletest/src/gtest-typed-test.cc
    ${GOOGLETEST_DIR}/googletest/src/gtest-internal-inl.h
)

# Google benchmark
get_filename_component(GOOGLEBENCHMARK_DIR ${ROOT_DIR}/lib/benchmark ABSOLUTE BASE_DIR ${CMAKE_CURRENT_SOURCE_DIR})
if (UNIX)
    add_subdirectory(${GOOGLEBENCHMARK_DIR} ${CMAKE_CURRENT_BINARY_DIR}/googlebenchmark)
endif (UNIX)

if (MSVC)
	# Recommendation from: https://cmake.org/pipermail/cmake/2016-April/063337.html
    if (NOT CMAKE_RUNTIME_OUTPUT_DIRECTORY)
        set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/bin")
    endif ()
    if (NOT CMAKE_LIBRARY_OUTPUT_DIRECTORY)
        set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/lib")
    endif()
    if (NOT CMAKE_ARCHIVE_OUTPUT_DIRECTORY)
        set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${PROJECT_BINARY_DIR}/lib")
    endif()

    file (GLOB_RECURSE BENCHMARK_SOURCES ${GOOGLEBENCHMARK_DIR}/src/*.cc)

	#add_subdirectory(${GOOGLEBENCHMARK_DIR} ${CMAKE_CURRENT_BINARY_DIR}/googlebenchmark)

endif (WIN32)
set(BENCHMARK_ENABLE_TESTING OFF)   # Disable Google Benchmark self-testing


# Compiler include folders
include_directories(${ROOT_DIR}/include)
include_directories(${ROOT_DIR}/src)
include_directories(${GOOGLETEST_DIR}/googletest)                               # To serve src/gtest-internal-inl.h include
include_directories(${GOOGLETEST_DIR}/googletest/include)
include_directories(${GOOGLEBENCHMARK_DIR}/include)

# Store executable into /bin subfolder
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# All sources in /benchmarks and subfolders will be compiled
file (GLOB_RECURSE SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/*.cpp)
EXCLUDE_FILES_FROM_DIR_IN_LIST("${SOURCES}" "/build/" FALSE OUTVAR SOURCES)     # Exclude /benchmarks/build folder

# All library sources
file (GLOB_RECURSE ROOT_SRC ${ROOT_DIR}/src/*.cpp)
EXCLUDE_FILES_FROM_DIR_IN_LIST("${ROOT_SRC}" "/build/" FALSE OUTVAR ROOT_SRC)   # Exclude /src/build folder
EXCLUDE_FILES_FROM_DIR_IN_LIST("${ROOT_SRC}" "/main.cpp" FALSE OUTVAR ROOT_SRC) # Exclude main.cpp file if exists
set(SOURCES "${SOURCES};${ROOT_SRC};${GOOGLETEST_FILES};${BENCHMARK_SOURCES}")

# Make '#define _CODE_UNDER_TEST 1' for the target code
add_compile_definitions(_CODE_UNDER_TEST=1)

# Compile example into executable
add_executable(${PROJECT_NAME} ${SOURCES})

# Link with following libraries
if (UNIX)
    target_link_libraries(${PROJECT_NAME} pthread benchmark benchmark_main)
endif (UNIX)

if (MSVC)
    # Match runtime library with default google benchmark settings. See: https://stackoverflow.com/questions/14172856/compile-with-mt-instead-of-md-using-cmake
    set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} /MT")
    set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} /MTd")

   	target_link_libraries(${PROJECT_NAME} shlwapi)
endif (MSVC)