# CMake entry point
cmake_minimum_required (VERSION 3.13)
project (message-center)

# Specify the C++ standard
set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Root folder
get_filename_component(ROOT_DIR ../ ABSOLUTE BASE_DIR ${CMAKE_CURRENT_SOURCE_DIR})

# Include CMake helper functions file
include(${ROOT_DIR}/cmake/func.cmake)
include(GNUInstallDirs)

# Header-only INTERFACE target for lock-free queues
add_library(message_center_headers INTERFACE)
add_library(message_center::headers ALIAS message_center_headers)
target_include_directories(message_center_headers INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
    $<BUILD_INTERFACE:${ROOT_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/message-center>
)
if (UNIX)
    target_link_libraries(message_center_headers INTERFACE pthread)
endif (UNIX)

# Static library target for classic EventQueue / MessageCenter + headers
add_library(message_center_lib STATIC
    ${CMAKE_CURRENT_SOURCE_DIR}/eventqueue.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/messagecenter.cpp
)
set_target_properties(message_center_lib PROPERTIES OUTPUT_NAME message_center)
add_library(message_center::message_center ALIAS message_center_lib)
target_include_directories(message_center_lib PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
    $<BUILD_INTERFACE:${ROOT_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}/message-center>
)
if (UNIX)
    target_link_libraries(message_center_lib PUBLIC pthread)
endif (UNIX)

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
if (UNIX)
    target_link_libraries(${PROJECT_NAME} pthread)
endif (UNIX)

if (MSVC)
    # Match runtime library with default gtest settings. See: https://stackoverflow.com/questions/14172856/compile-with-mt-instead-of-md-using-cmake
    set(CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE} /MT")
    set(CMAKE_CXX_FLAGS_DEBUG "${CMAKE_CXX_FLAGS_DEBUG} /MTd")
endif (MSVC)

# Install rules for library and public headers
file(GLOB MC_PUBLIC_HEADERS "${CMAKE_CURRENT_SOURCE_DIR}/*.h")
install(TARGETS message_center_lib message_center_headers
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
)
install(FILES ${MC_PUBLIC_HEADERS}
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}/message-center
)