cmake_minimum_required(VERSION 3.25)
project(test_package LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(embedview CONFIG REQUIRED)

add_executable(test_package test_package.cpp)
if(MSVC)
    target_compile_options(test_package PRIVATE /utf-8)
endif()
target_link_libraries(test_package PRIVATE embedview::native)
