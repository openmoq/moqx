cmake_minimum_required(VERSION 3.22)

project(moqx VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Default to RelWithDebInfo for reproducible dev builds.
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "" FORCE)
endif()

option(MOQX_BUILD_TESTS "Build tests" ON)
option(MOQX_BUILD_BENCHMARKS "Build benchmarks" OFF)
option(MOQX_ENABLE_SANITIZERS "Enable ASAN/UBSAN (non-Release)" OFF)
option(MOQX_ENABLE_TSAN "Enable TSan (non-Release)" OFF)

if(MOQX_ENABLE_SANITIZERS AND MOQX_ENABLE_TSAN)
  message(FATAL_ERROR "MOQX_ENABLE_SANITIZERS and MOQX_ENABLE_TSAN are mutually exclusive")
endif()

if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
  set(_moqx_bpf_default ON)
else()
  set(_moqx_bpf_default OFF)
endif()
option(MOQX_ENABLE_BPF_STEERING
  "Attach a classic BPF reuseport filter to steer QUIC packets to the correct worker (Linux only)"
  ${_moqx_bpf_default})

if(MOQX_ENABLE_SANITIZERS AND NOT CMAKE_BUILD_TYPE STREQUAL "Release")
  add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
  add_link_options(-fsanitize=address,undefined)
endif()

if(MOQX_ENABLE_TSAN AND NOT CMAKE_BUILD_TYPE STREQUAL "Release")
  add_compile_options(-fsanitize=thread -fno-omit-frame-pointer)
  add_link_options(-fsanitize=thread)
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
find_package(OpenSSL REQUIRED)

include(cmake/CPM.cmake)

# --- yaml-cpp (CPM) ---
CPMAddPackage(
        NAME yaml-cpp
        GITHUB_REPOSITORY jbeder/yaml-cpp
        GIT_TAG 0.8.0
        OPTIONS
        "YAML_CPP_BUILD_TESTS OFF"
        "YAML_CPP_BUILD_TOOLS OFF"
        "YAML_CPP_FORMAT_SOURCE OFF"
        "CMAKE_WARN_DEPRECATED OFF"
)

# --- reflect-cpp (FetchContent, needs yaml-cpp available) ---

CPMAddPackage(
        NAME reflectcpp
        GITHUB_REPOSITORY getml/reflect-cpp
        GIT_TAG v0.18.0
        OPTIONS "REFLECTCPP_YAML ON" "REFLECTCPP_BUILD_TESTS OFF"
)

# --- XLOG category rooting ---
# Rewrite __FILE__ for moqx sources so folly XLOG categories render as moqx.*
# (e.g. moqx.MoqxRelay) instead of src.* — sources live under src/, but the
# user-facing logging API expects "moqx" as the top-level category. The
# FOLLY_XLOG_STRIP_PREFIXES define handles paths the prefix-map doesn't touch
# (notably generated headers under the build dir). Placed here so subsequent
# moqx_* targets pick it up; yaml-cpp / reflectcpp above were declared before
# this point and are unaffected.
add_compile_options(-fmacro-prefix-map=${CMAKE_CURRENT_SOURCE_DIR}/src=moqx)
add_compile_definitions(
  "FOLLY_XLOG_STRIP_PREFIXES=\"${CMAKE_SOURCE_DIR}:${CMAKE_BINARY_DIR}\""
)

# --- catapult (CAT/CWT/MOQT auth implementation) ---

set(ENABLE_LOGGING OFF CACHE BOOL "" FORCE)
set(CATAPULT_ENABLE_JSON OFF CACHE BOOL "" FORCE)
add_subdirectory(deps/catapult EXCLUDE_FROM_ALL)

# --- Cache library (forked from moxygen) ---

add_library(moqx_cache STATIC
  src/MoqxCache.cpp
)

target_include_directories(moqx_cache
  PUBLIC
    ${PROJECT_SOURCE_DIR}/src
)

target_link_libraries(moqx_cache PUBLIC
  moxygen::moxygen_moq
  moxygen::moxygen_moq_consumers
  moxygen::moxygen_moq_framer
  moxygen::moxygen_util_location_interval_set
  Folly::folly_container_f14_hash
  Folly::folly_coro_baton
  Folly::folly_coro_task
  Folly::folly_logging_logging
)

target_compile_options(moqx_cache PRIVATE -Wall -Wextra -Wpedantic)

# --- HMAC key derivation (shared by the relay's verifier and the issuer) ---

add_library(moqx_hmac_key STATIC
  src/auth/HmacKey.cpp
)

target_include_directories(moqx_hmac_key
  PUBLIC
    ${PROJECT_SOURCE_DIR}/src
)

target_link_libraries(moqx_hmac_key PUBLIC OpenSSL::Crypto)

target_compile_options(moqx_hmac_key PRIVATE -Wall -Wextra -Wpedantic)

# --- Core library ---

add_library(moqx_core STATIC
  src/auth/Auth.cpp
  src/LoggingMultiFlag.cpp
  src/NamespaceTree.cpp
  src/SubscriptionRegistry.cpp
  src/MoqxRelay.cpp
  src/MoqxRelayContext.cpp
  src/MoqxRelayServer.cpp
  src/MoqxPicoRelayServer.cpp
  src/MoqxQmuxRelayServer.cpp
  src/ServiceMatcher.cpp
  src/admin/AdminServer.cpp
  src/admin/BuiltinRoutes.cpp
  src/admin/CachePurgeHandler.cpp
  src/admin/ConfigHandler.cpp
  src/admin/MetricsHandler.cpp
  src/admin/StateHandler.cpp
  src/stats/StatsRegistry.cpp
  src/stats/MoQStatsCollector.cpp
  src/stats/PicoQuicStatsCollector.cpp
  src/stats/QuicStatsCollector.cpp
  src/stats/EventBaseStatsCollector.cpp
  src/UpstreamProvider.cpp
  src/relay/AuthFilters.cpp
  src/relay/TopNFilter.cpp
  src/relay/PropertyRanking.cpp
  src/relay/CrossExecFilter.cpp
  src/relay/CrossExecForwarderCallback.cpp
  src/relay/WeakRelayForwarderCallback.cpp
  src/relay/PublisherCrossExecFilter.cpp
  src/relay/SubscriberCrossExecFilter.cpp
  src/logging/LogSetup.cpp
)

target_include_directories(moqx_core
  PUBLIC
    ${PROJECT_SOURCE_DIR}/src
)

target_link_libraries(moqx_core PUBLIC
  moqx_cache
  moqx_hmac_key
  moxygen::moxygen_relay_moq_forwarder
  moxygen::moxygen_moq_relay_session
  moxygen::moxygen_moq_server
  moxygen::moxygen_moq_qmux_server
  moxygen::moxygen_qmux_utils
  moxygen::openmoq_pico_evb_server_transport
  proxygen::proxygenhttpserver
  # Workaround: wangle::wangle_acceptor_acceptor_core uses AsyncFdSocket from
  # FizzAcceptorHandshakeHelper but omits this dep from its cmake config.
  Folly::folly_io_async_fdsock_async_fd_socket
  moxygen::moxygen_moqclient
  OpenSSL::Crypto
  catapult
  moxygen::moxygen_mlog_file_mlogger
  moxygen::moxygen_mlog_mlogger_factory
  moxygen::moxygen_mlog_sampling_mlogger_factory
  mvfst::mvfst_logging_file_qlogger
)

target_compile_options(moqx_core PRIVATE -Wall -Wextra -Wpedantic)
# Version reported by /info (and the relay dashboard badge). CI passes
# `git describe --tags` here: the bare tag on release commits (v0.2.1),
# tag-distance-sha on snapshots (v0.2.1-14-g2ed29c8). Local builds fall
# back to the static project version.
if(NOT MOQX_VERSION_STRING)
  set(MOQX_VERSION_STRING "${PROJECT_VERSION}")
endif()
target_compile_definitions(moqx_core PRIVATE MOQX_VERSION="${MOQX_VERSION_STRING}")

# --- Config types (resolved Config structs, header-only, no rfl dependency) ---
# Library consumers that only need the Config API link this.

add_library(moqx_config INTERFACE)

target_include_directories(moqx_config
  INTERFACE
    ${PROJECT_SOURCE_DIR}/src
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
  src/config/Loader.cpp
  src/config/ConfigResolver.cpp
  src/config/ConfigInit.cpp
  src/config/Pkcs12.cpp
)

target_include_directories(moqx_config_loader
  PUBLIC
    ${PROJECT_SOURCE_DIR}/src
)

target_link_libraries(moqx_config_loader PUBLIC
  moqx_config
  reflectcpp
  yaml-cpp
  Folly::folly_file_util
)

# PKCS#12 transcode (Pkcs12.cpp) + OPENSSL_cleanse in ConfigResolver.cpp;
# folly cert utils supplies OpenSSLCertUtils::pemEncode.
target_link_libraries(moqx_config_loader PRIVATE
  OpenSSL::Crypto
  Folly::folly_ssl_openssl_cert_utils
)

target_compile_options(moqx_config_loader PRIVATE -Wall -Wextra -Wpedantic)

# --- Main executable ---

add_executable(moqx
  src/main.cpp
)

if(MOQX_ENABLE_BPF_STEERING)
  # QuicReuseportSteering.cpp defines mvfst_hook_on_socket_create, overriding
  # the weak symbol in mvfst. It must live in the executable (not a static lib)
  # so the linker always includes it without a --whole-archive trick.
  target_sources(moqx PRIVATE src/bpf/QuicReuseportSteering.cpp)
  target_compile_definitions(moqx PRIVATE MOQX_ENABLE_BPF_STEERING)
endif()

target_link_libraries(moqx PRIVATE moqx_core moqx_config_loader)

# --- CAT4MOQ token issuer (signing) library, shared by the CLI and tests ---

add_library(moqx_issuer_lib STATIC
  src/auth/AuthTokenIssuer.cpp
)

target_include_directories(moqx_issuer_lib
  PUBLIC
    ${PROJECT_SOURCE_DIR}/src
)

target_link_libraries(moqx_issuer_lib PUBLIC
  moqx_hmac_key
  moqx_config
  catapult
  moxygen::moxygen_moq_types
)

target_compile_options(moqx_issuer_lib PRIVATE -Wall -Wextra -Wpedantic)

# --- Standalone moqx-issuer CLI ---

add_executable(moqx-issuer
  src/auth/IssuerMain.cpp
)

target_link_libraries(moqx-issuer PRIVATE
  moqx_issuer_lib
  moqx_config_loader
  Folly::folly_init_init
  Folly::folly_base64
  Folly::folly_string
  gflags
)

target_compile_options(moqx-issuer PRIVATE -Wall -Wextra -Wpedantic)

# --- Tests ---

if(MOQX_BUILD_TESTS)
  enable_testing()
  add_subdirectory(test)
endif()

if(MOQX_BUILD_BENCHMARKS)
  add_subdirectory(benchmark)
endif()

include(${PROJECT_SOURCE_DIR}/cmake/Lint.cmake)

install(TARGETS moqx moqx-issuer moqx_core moqx_config_loader)
