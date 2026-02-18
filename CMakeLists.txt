cmake_minimum_required(VERSION 3.20)

project(o_rly VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Default to RelWithDebInfo for reproducible dev builds.
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "" FORCE)
endif()

option(ORLY_BUILD_TESTS "Build tests" ON)
option(ORLY_ENABLE_SANITIZERS "Enable ASAN/UBSAN (non-Release)" OFF)

if(ORLY_ENABLE_SANITIZERS AND NOT CMAKE_BUILD_TYPE STREQUAL "Release")
  add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
  add_link_options(-fsanitize=address,undefined)
endif()

# Prefer static libraries for all transitive deps
set(GFLAGS_SHARED OFF CACHE BOOL "" FORCE)

list(APPEND CMAKE_MODULE_PATH
  "${PROJECT_SOURCE_DIR}/deps/moxygen/build/fbcode_builder/CMake"
)

find_package(moxygen REQUIRED)

add_library(o_rly_core STATIC
  src/relay.cpp
)

target_include_directories(o_rly_core
  PUBLIC
    ${PROJECT_SOURCE_DIR}/include
)

target_link_libraries(o_rly_core PUBLIC
  moxygen::moxygen_relay_moq_relay
  moxygen::moxygen_moq_relay_session
  moxygen::moxygen_moq_server
)

target_compile_options(o_rly_core PRIVATE -Wall -Wextra -Wpedantic)

add_executable(o_rly
  src/main.cpp
)

target_link_libraries(o_rly PRIVATE o_rly_core)

if(ORLY_BUILD_TESTS)
  enable_testing()
  add_executable(o_rly_smoke tests/smoke_test.cpp)
  target_link_libraries(o_rly_smoke PRIVATE o_rly_core)
  add_test(NAME o_rly_smoke COMMAND o_rly_smoke)
endif()

include(${PROJECT_SOURCE_DIR}/cmake/Lint.cmake)

install(TARGETS o_rly o_rly_core)
