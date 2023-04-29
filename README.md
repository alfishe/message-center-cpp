iOS / macOS style Message Center / Queue


Allows to subscribe following observer types:
- Static functions
- Class instance methods
- Lambdas (or any other code blocks compatible with std::function)


Pre-requisites:
- CMake v3.13 or newer


Submodules:
- Google Test
- Google Benchmark


How to start:

    git clone --recurse-submodules https://github.com/alfishe/message-center-cpp.git

or

    git clone https://github.com/alfishe/message-center-cpp.git
    git submodule init
    git submodule update


Updates:

    git pull --recurse-submodules
    
# Build

## Linux / macOS

    mkdir build
    cd build
    cmake ..
    cmake --build . --parallel 12

/src /tests /benchmarks can be built separately same way

## Windows

CMake/MSBuild chain under windows behaves weirdly: it generates MSVC projects and always uses Debug configuration totally ignoring CMAKE_BUILD_TYPE value. So the only way to have control over build type - to use cmake --config parameter.
 
    --config Debug
    --config Release

The rest is similar to *nix:

    mkdir build
    cd build
    cmake ..
    cmake --build . --config Release

/src /tests /benchmarks can be built separately same way
