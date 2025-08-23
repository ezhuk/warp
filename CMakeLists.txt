cmake_minimum_required(VERSION 3.22)

project(warp VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(BUILD_TESTS "Build tests" OFF)

find_package(fmt CONFIG REQUIRED)

add_executable(warp src/main.cpp)

target_link_libraries(warp PRIVATE fmt::fmt)

if(BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
