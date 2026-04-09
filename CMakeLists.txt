cmake_minimum_required(VERSION 3.25)

project(moqx VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Default to RelWithDebInfo for reproducible dev builds.
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "" FORCE)
endif()

option(MOQX_BUILD_TESTS "Build tests" ON)
option(MOQX_ENABLE_SANITIZERS "Enable ASAN/UBSAN (non-Release)" OFF)

if(MOQX_ENABLE_SANITIZERS AND NOT CMAKE_BUILD_TYPE STREQUAL "Release")
  add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
  add_link_options(-fsanitize=address,undefined)
endif()

# Prefer static libraries for all transitive deps (from-release mode).
# For from-source builds, build.sh overrides this to ON (system gflags is shared-only).
set(GFLAGS_SHARED OFF CACHE BOOL "")

list(APPEND CMAKE_MODULE_PATH
  "${PROJECT_SOURCE_DIR}/deps/moxygen/build/fbcode_builder/CMake"
)

# Folly's config calls FindBoost which was removed in CMake 3.30 (CMP0167).
# Folly itself works fine with Boost's own config-mode package; silence the
# policy warning until upstream fixes their config files.
if(POLICY CMP0167)
  cmake_policy(SET CMP0167 OLD)
endif()

find_package(moxygen REQUIRED)

# --- yaml-cpp (FetchContent) ---

include(FetchContent)

set(YAML_CPP_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(YAML_CPP_BUILD_TOOLS OFF CACHE BOOL "" FORCE)
set(YAML_CPP_FORMAT_SOURCE OFF CACHE BOOL "" FORCE)

# yaml-cpp 0.8.0 uses cmake_minimum_required(VERSION 3.0.2) which triggers a
# deprecation warning on CMake ≥ 3.27. Suppress it for this dependency only.
set(_moqx_save_warn_deprecated "${CMAKE_WARN_DEPRECATED}")
set(CMAKE_WARN_DEPRECATED OFF CACHE BOOL "" FORCE)

FetchContent_Declare(yaml-cpp
  GIT_REPOSITORY https://github.com/jbeder/yaml-cpp.git
  GIT_TAG 0.8.0
  SYSTEM
)
FetchContent_MakeAvailable(yaml-cpp)

set(CMAKE_WARN_DEPRECATED "${_moqx_save_warn_deprecated}" CACHE BOOL "" FORCE)

# --- reflect-cpp (FetchContent, needs yaml-cpp available) ---

set(REFLECTCPP_YAML ON CACHE BOOL "" FORCE)
set(REFLECTCPP_BUILD_TESTS OFF CACHE BOOL "" FORCE)

FetchContent_Declare(reflectcpp
  GIT_REPOSITORY https://github.com/getml/reflect-cpp.git
  GIT_TAG v0.18.0
  SYSTEM
)
FetchContent_MakeAvailable(reflectcpp)

# --- Core library ---

add_library(moqx_core STATIC
  src/MoqxRelay.cpp
  src/MoqxRelayServer.cpp
  src/ServiceMatcher.cpp
  src/admin/AdminServer.cpp
  src/admin/BuiltinRoutes.cpp
  src/admin/MetricsHandler.cpp
  src/stats/StatsRegistry.cpp
  src/stats/MoQStatsCollector.cpp
  src/stats/QuicStatsCollector.cpp
  src/UpstreamProvider.cpp
  src/relay/TopNFilter.cpp
  src/relay/PropertyRanking.cpp
)

target_include_directories(moqx_core
  PUBLIC
    ${PROJECT_SOURCE_DIR}/include
)

target_link_libraries(moqx_core PUBLIC
  moxygen::moxygen_relay_moq_forwarder
  moxygen::moxygen_relay_moq_cache
  moxygen::moxygen_moq_relay_session
  moxygen::moxygen_moq_server
  proxygen::proxygenhttpserver
  # Workaround: wangle::wangle_acceptor_acceptor_core uses AsyncFdSocket from
  # FizzAcceptorHandshakeHelper but omits this dep from its cmake config.
  Folly::folly_io_async_fdsock_async_fd_socket
  moxygen::moxygen_moqclient
)

target_compile_options(moqx_core PRIVATE -Wall -Wextra -Wpedantic)
target_compile_definitions(moqx_core PRIVATE MOQX_VERSION="${PROJECT_VERSION}")

# --- Config types (resolved Config structs, header-only, no rfl dependency) ---
# Library consumers that only need the Config API link this.

add_library(moqx_config INTERFACE)

target_include_directories(moqx_config
  INTERFACE
    ${PROJECT_SOURCE_DIR}/include
)

target_link_libraries(moqx_config INTERFACE
  Folly::folly_network_address
  Folly::folly_container_detail_f14_hash_detail
)

# --- Config loader (YAML parsing + validation + resolution + init) ---
# Application-level: ParsedConfig rfl structs, loadConfig(), resolveConfig(),
# handleConfigSubcommand(). Links rfl/yaml-cpp; consumers that only need
# resolved Config types can depend on moqx_config alone.

add_library(moqx_config_loader STATIC
  src/config/loader.cpp
  src/config/config_resolver.cpp
  src/config/config_init.cpp
)

target_include_directories(moqx_config_loader
  PUBLIC
    ${PROJECT_SOURCE_DIR}/include
)

target_link_libraries(moqx_config_loader PUBLIC
  moqx_config
  reflectcpp
  yaml-cpp
  Folly::folly_file_util
)

target_compile_options(moqx_config_loader PRIVATE -Wall -Wextra -Wpedantic)

# --- Main executable ---

add_executable(moqx
  src/main.cpp
)

target_link_libraries(moqx PRIVATE moqx_core moqx_config_loader)

# --- Tests ---

if(MOQX_BUILD_TESTS)
  enable_testing()
  find_package(GTest REQUIRED CONFIG)

  add_library(moqx_test_utils STATIC
    tests/TestUtils.cpp
  )
  target_include_directories(moqx_test_utils PUBLIC
    ${PROJECT_SOURCE_DIR}/tests
  )
  target_link_libraries(moqx_test_utils PUBLIC
    Folly::folly_io_iobuf
    Folly::folly_random
  )

  add_library(moqx_test_main STATIC
    tests/TestMain.cpp
  )
  target_link_libraries(moqx_test_main PUBLIC
    Folly::folly_init_init
    GTest::gtest
  )

  add_executable(moqx_relay_test
    tests/MoqxRelayTest.cpp
  )
  target_link_libraries(moqx_relay_test PRIVATE
    moqx_core
    moqx_test_utils
    moqx_test_main
    GTest::gmock
    moxygen::moxygen_events_moq_folly_executor_impl
  )
  include(GoogleTest)
  gtest_discover_tests(moqx_relay_test)

  add_executable(moqx_track_filter_test
    tests/MoqxTrackFilterTest.cpp
  )
  target_link_libraries(moqx_track_filter_test PRIVATE
    moqx_core
    moqx_test_utils
    moqx_test_main
    GTest::gmock
    moxygen::moxygen_events_moq_folly_executor_impl
  )
  gtest_discover_tests(moqx_track_filter_test)

  add_executable(moqx_config_test
    tests/config/loader_test.cpp
  )
  target_link_libraries(moqx_config_test PRIVATE
    moqx_config_loader
    GTest::gtest_main
    GTest::gmock
  )
  target_compile_definitions(moqx_config_test PRIVATE
    CONFIG_EXAMPLE_PATH="${PROJECT_SOURCE_DIR}/config.example.yaml"
  )
  # When using static gflags (from-release), gflags_nothreads_static also
  # appears as a plain transitive dep from folly — link it as WHOLE_ARCHIVE
  # and override the feature to avoid mixed link strategy warnings.
  # LINK_LIBRARY:WHOLE_ARCHIVE requires CMake >= 3.24 (cmake_minimum_required is 3.25).
  if(NOT GFLAGS_SHARED)
    target_link_libraries(moqx_config_test PRIVATE
      "$<LINK_LIBRARY:WHOLE_ARCHIVE,gflags_nothreads_static>"
    )
    set_property(TARGET moqx_config_test PROPERTY
      LINK_LIBRARY_OVERRIDE "WHOLE_ARCHIVE,gflags_nothreads_static"
    )
  endif()
  gtest_discover_tests(moqx_config_test)

  add_executable(moqx_config_resolver_test
    tests/config/config_resolver_test.cpp
  )
  target_link_libraries(moqx_config_resolver_test PRIVATE
    moqx_config_loader
    GTest::gtest_main
    GTest::gmock
  )
  if(NOT GFLAGS_SHARED)
    target_link_libraries(moqx_config_resolver_test PRIVATE
      "$<LINK_LIBRARY:WHOLE_ARCHIVE,gflags_nothreads_static>"
    )
    set_property(TARGET moqx_config_resolver_test PROPERTY
      LINK_LIBRARY_OVERRIDE "WHOLE_ARCHIVE,gflags_nothreads_static"
    )
  endif()
  gtest_discover_tests(moqx_config_resolver_test)

  add_executable(moqx_bounded_histogram_test
    tests/stats/BoundedHistogramTest.cpp
  )
  target_include_directories(moqx_bounded_histogram_test PRIVATE
    ${PROJECT_SOURCE_DIR}/include
  )
  target_link_libraries(moqx_bounded_histogram_test PRIVATE
    GTest::gtest_main
    GTest::gmock
  )
  gtest_discover_tests(moqx_bounded_histogram_test)

  add_executable(moqx_service_matcher_test
    tests/ServiceMatcherTest.cpp
  )
  target_link_libraries(moqx_service_matcher_test PRIVATE
    moqx_core
    moqx_config
    GTest::gtest_main
    GTest::gmock
  )
  gtest_discover_tests(moqx_service_matcher_test)

  add_test(
    NAME relay_chain
    COMMAND bash ${PROJECT_SOURCE_DIR}/tests/test_relay_chain.sh $<TARGET_FILE:moqx>
  )
  set_tests_properties(relay_chain PROPERTIES
    TIMEOUT 120
    ENVIRONMENT "MOQBIN=${PROJECT_SOURCE_DIR}/.scratch/moxygen-install/bin"
  )

  add_test(
    NAME admin_info_endpoint
    COMMAND bash ${PROJECT_SOURCE_DIR}/tests/test_admin_info.sh $<TARGET_FILE:moqx>
  )
  add_test(
    NAME admin_tls_endpoint
    COMMAND bash ${PROJECT_SOURCE_DIR}/tests/test_admin_tls.sh $<TARGET_FILE:moqx>
  )
  add_test(
    NAME admin_metrics_endpoint
    COMMAND bash ${PROJECT_SOURCE_DIR}/tests/test_admin_metrics.sh $<TARGET_FILE:moqx>
  )

  add_executable(moqx_upstream_provider_test
    tests/UpstreamProviderTest.cpp
  )
  target_link_libraries(moqx_upstream_provider_test PRIVATE
    moqx_core
    moqx_test_utils
    moqx_test_main
    moxygen::moqtest_utils
    moxygen::moxygen_events_moq_folly_executor_impl
    moxygen::moxygen_moqclient
    GTest::gmock
  )
  gtest_discover_tests(moqx_upstream_provider_test)

  # Webinar blackbox test - connects to a real relay to test TRACK_FILTER
  add_executable(webinar_blackbox_test
    tests/WebinarBlackboxTest.cpp
  )
  target_link_libraries(webinar_blackbox_test PRIVATE
    moqx_core
    Folly::folly_init_init
    moxygen::moxygen_events_moq_folly_executor_impl
    moxygen::moxygen_moqclient
    moxygen::moxygen_moq_webtransport_client
    moxygen::moxygen_relay_moq_relay_client
    moxygen::moxygen_relay_moq_forwarder
    moxygen::moxygen_moq_relay_session
    moxygen::moxygen_util_insecure_verifier_dangerous_do_not_use_in_production
  )
  # Not auto-discovered as a test since it requires a running relay

  # Track Filter Load Test - large-scale scalability test
  add_executable(track_filter_load_test
    tests/TrackFilterLoadTest.cpp
  )
  target_link_libraries(track_filter_load_test PRIVATE
    moqx_core
    Folly::folly_init_init
    moxygen::moxygen_events_moq_folly_executor_impl
    moxygen::moxygen_moqclient
    moxygen::moxygen_moq_webtransport_client
    moxygen::moxygen_relay_moq_relay_client
    moxygen::moxygen_relay_moq_forwarder
    moxygen::moxygen_moq_relay_session
    moxygen::moxygen_util_insecure_verifier_dangerous_do_not_use_in_production
  )
  # Not auto-discovered as a test since it requires a running relay
endif()

include(${PROJECT_SOURCE_DIR}/cmake/Lint.cmake)

install(TARGETS moqx moqx_core moqx_config_loader)
