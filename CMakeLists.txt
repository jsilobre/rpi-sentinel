cmake_minimum_required(VERSION 3.28)
project(rpi-sentinel LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

include(cmake/CompilerOptions.cmake)
include(cmake/Dependencies.cmake)

option(BUILD_TESTING "Build unit tests" ON)

add_subdirectory(src)

if(BUILD_TESTING)
    enable_testing()
    add_subdirectory(tests)
endif()
