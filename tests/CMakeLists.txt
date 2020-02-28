# CMake entry point
cmake_minimum_required (VERSION 3.13)
project (message-center-tests)

# Specify the C++ standard
set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Library root
get_filename_component(ROOT_DIR ../ ABSOLUTE BASE_DIR ${CMAKE_CURRENT_SOURCE_DIR})

# Include CMake helper functions file
include(${ROOT_DIR}/cmake/func.cmake)

# Google test
get_filename_component(GOOGLETEST_DIR ${ROOT_DIR}/lib/googletest/ ABSOLUTE BASE_DIR ${CMAKE_CURRENT_SOURCE_DIR})
add_subdirectory(${GOOGLETEST_DIR} ${CMAKE_CURRENT_BINARY_DIR}/googletest)

# Compiler include folders
include_directories(${ROOT_DIR}/include)
include_directories(${ROOT_DIR}/src)
include_directories(${GOOGLETEST_DIR}/googletest/include)

# Store executable into /bin subfolder
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# All sources in /tests and subfolders will be compiled
file (GLOB_RECURSE SOURCES ${CMAKE_CURRENT_SOURCE_DIR}/*.cpp)
EXCLUDE_FILES_FROM_DIR_IN_LIST("${SOURCES}" "/build/" FALSE OUTVAR SOURCES) # Exclude /tests/build folder

# All library sources
file (GLOB_RECURSE ROOT_SRC ${ROOT_DIR}/src/*.cpp)
EXCLUDE_FILES_FROM_DIR_IN_LIST("${ROOT_SRC}" "/build/" FALSE OUTVAR ROOT_SRC)   # Exclude /src/build folder
EXCLUDE_FILES_FROM_DIR_IN_LIST("${ROOT_SRC}" "/main.cpp" FALSE OUTVAR ROOT_SRC) # Exclude main.cpp file if exists
set(SOURCES "${SOURCES};${ROOT_SRC}")

# Compile tests into executable
add_executable(${PROJECT_NAME} ${SOURCES})

# Declare _DEBUG symbol for compiler
if (CMAKE_BUILD_TYPE MATCHES Debug)
    message(STATUS "CMAKE_BUILD_TYPE:${CMAKE_BUILD_TYPE} - defining '_DEBUG'")
    target_compile_definitions(${PROJECT_NAME} PRIVATE _DEBUG)
endif (CMAKE_BUILD_TYPE MATCHES Debug)

# Enable test wrappers in code
target_compile_definitions(${PROJECT_NAME} PRIVATE _CODE_UNDER_TEST)

# Link with following libraries
if (UNIX)
    target_link_libraries(${PROJECT_NAME} pthread gtest gtest_main)
endif (UNIX)

if (WIN32)
    # Match runtime library with default gtest settings. See: https://stackoverflow.com/questions/14172856/compile-with-mt-instead-of-md-using-cmake
    set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} /MT")
    set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} /MTd")

    target_link_libraries(${PROJECT_NAME} gtest gtest_main)
endif (WIN32)
