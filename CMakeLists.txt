cmake_minimum_required(VERSION 3.22)

project(warp VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(BUILD_TESTS "Build tests" ON)
option(BUILD_SHARED_LIBS "Build shared libraries" OFF)

find_package(fmt CONFIG REQUIRED)
find_package(folly CONFIG REQUIRED)
find_package(wangle CONFIG REQUIRED)

add_library(warp)

target_include_directories(warp
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>
  PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_sources(warp
  PRIVATE
    src/warp/warp.cpp
    src/warp/mqtt/codec.cpp
    src/warp/mqtt/message.cpp
    src/warp/mqtt/server.cpp
)

target_link_libraries(warp
  PUBLIC
    fmt::fmt
    Folly::folly
  PRIVATE
    wangle::wangle
)

add_library(warp::warp ALIAS warp)

add_executable(app)

target_sources(app
  PRIVATE
    src/main.cpp
)

target_link_libraries(app
  PRIVATE
    fmt::fmt
    warp::warp
)

set_target_properties(app PROPERTIES
  OUTPUT_NAME "warp"
)

if(BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()

include(GNUInstallDirs)

install(DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/include/
  DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)

install(TARGETS warp app
  EXPORT warp-targets
  INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
  ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
  LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
  RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
)

install(EXPORT warp-targets
  FILE warp-targets.cmake
  NAMESPACE warp::
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/warp
)

include(CMakePackageConfigHelpers)

configure_package_config_file(
  ${CMAKE_CURRENT_SOURCE_DIR}/cmake/warp-config.cmake.in
  ${CMAKE_CURRENT_BINARY_DIR}/warp-config.cmake
  INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/warp
)

write_basic_package_version_file(
  ${CMAKE_CURRENT_BINARY_DIR}/warp-config-version.cmake
  VERSION ${PROJECT_VERSION}
  COMPATIBILITY SameMajorVersion
)

install(
  FILES
    ${CMAKE_CURRENT_BINARY_DIR}/warp-config.cmake
    ${CMAKE_CURRENT_BINARY_DIR}/warp-config-version.cmake
  DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/warp
)
