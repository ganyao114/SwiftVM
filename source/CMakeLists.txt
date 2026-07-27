cmake_minimum_required(VERSION 3.21)
project(SwiftRuntime)
enable_language(ASM)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include_directories(${CMAKE_CURRENT_SOURCE_DIR})

# Add the module directory to the list of paths
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

# Arch detection
include(DetectArchitecture)
if (NOT DEFINED ARCHITECTURE)
    message(FATAL_ERROR "Unsupported architecture encountered. Ending CMake generation.")
endif()
message(STATUS "Target architecture: ${ARCHITECTURE}")

add_subdirectory(base)
add_subdirectory(externals)
add_subdirectory(runtime)
add_subdirectory(compiler)
add_subdirectory(translator)
# AOT pre-compiler (docs/aot-design.md). After translator: it uses the
# elfio::elfio alias that translator/linux creates.
add_subdirectory(aot)
add_subdirectory(tests)