# CMake entry point
cmake_minimum_required (VERSION 3.5)
project (message-center)

# Specify the C++ standard
set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Root folder
get_filename_component(ROOT_DIR ../ ABSOLUTE BASE_DIR ${CMAKE_CURRENT_SOURCE_DIR})

# Include CMake helper functions file
include(${ROOT_DIR}/cmake/func.cmake)

# Compiler include folders
include_directories(${ROOT_DIR}/include)

# Store executable into /bin subfolder
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# Compile following .cpp sources
file (GLOB_RECURSE SOURCES *.cpp)
EXCLUDE_FILES_FROM_DIR_IN_LIST("${SOURCES}" "/build/" FALSE OUTVAR SOURCES) # Exclude /build folder


# Compile example into executable
add_executable(${PROJECT_NAME} ${SOURCES})

# Declare _DEBUG symbol for compiler
if (CMAKE_BUILD_TYPE MATCHES DEBUG)
    target_compile_definitions(${PROJECT_NAME} PRIVATE _DEBUG)
endif (CMAKE_BUILD_TYPE MATCHES DEBUG)

# Link with following libraries
target_link_libraries(${PROJECT_NAME} pthread)