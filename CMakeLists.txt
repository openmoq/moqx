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

# --- yaml-cpp (FetchContent) ---

include(FetchContent)

set(YAML_CPP_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(YAML_CPP_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
set(YAML_CPP_FORMAT_SOURCE OFF CACHE BOOL "" FORCE)

FetchContent_Declare(yaml-cpp
  GIT_REPOSITORY https://github.com/jbeder/yaml-cpp.git
  GIT_TAG 0.8.0
)
FetchContent_MakeAvailable(yaml-cpp)

# --- reflect-cpp (FetchContent, needs yaml-cpp available) ---

set(REFLECTCPP_YAML ON CACHE BOOL "" FORCE)
set(REFLECTCPP_BUILD_TESTS OFF CACHE BOOL "" FORCE)

FetchContent_Declare(reflectcpp
  GIT_REPOSITORY https://github.com/getml/reflect-cpp.git
  GIT_TAG v0.18.0
)
FetchContent_MakeAvailable(reflectcpp)

# --- Core library ---

add_library(o_rly_core STATIC
  src/ORelay.cpp
  src/ORelayServer.cpp
)

target_include_directories(o_rly_core
  PUBLIC
    ${PROJECT_SOURCE_DIR}/include
)

target_link_libraries(o_rly_core PUBLIC
  moxygen::moxygen_relay_moq_forwarder
  moxygen::moxygen_relay_moq_cache
  moxygen::moxygen_moq_relay_session
  moxygen::moxygen_moq_server
)

target_compile_options(o_rly_core PRIVATE -Wall -Wextra -Wpedantic)

# --- Config library ---

add_library(o_rly_config STATIC
  src/config/loader.cpp
)

target_include_directories(o_rly_config
  PUBLIC
    ${PROJECT_SOURCE_DIR}/include
)

target_link_libraries(o_rly_config PUBLIC
  reflectcpp
  yaml-cpp
)

target_compile_options(o_rly_config PRIVATE -Wall -Wextra -Wpedantic)

# --- Main executable ---

add_executable(o_rly
  src/main.cpp
)

target_link_libraries(o_rly PRIVATE o_rly_core o_rly_config)

# --- Tests ---

if(ORLY_BUILD_TESTS)
  enable_testing()
  find_package(GTest REQUIRED)

  add_executable(o_rly_relay_test
    tests/ORelayTest.cpp
    ${PROJECT_SOURCE_DIR}/deps/moxygen/moxygen/test/TestUtils.cpp
  )
  target_include_directories(o_rly_relay_test PRIVATE
    ${PROJECT_SOURCE_DIR}/deps/moxygen
  )
  target_link_libraries(o_rly_relay_test PRIVATE
    o_rly_core
    moxygen::moqtest_utils
    moxygen::moxygen_events_moq_folly_executor_impl
    GTest::gtest_main
    GTest::gmock
  )
  include(GoogleTest)
  gtest_discover_tests(o_rly_relay_test)

  add_executable(o_rly_config_test
    tests/config/loader_test.cpp
  )
  target_link_libraries(o_rly_config_test PRIVATE
    o_rly_config
    GTest::gtest_main
  )
  target_compile_definitions(o_rly_config_test PRIVATE
    CONFIG_EXAMPLE_PATH="${PROJECT_SOURCE_DIR}/config.example.yaml"
  )
  gtest_discover_tests(o_rly_config_test)
endif()

include(${PROJECT_SOURCE_DIR}/cmake/Lint.cmake)

install(TARGETS o_rly o_rly_core o_rly_config)
