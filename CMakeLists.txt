cmake_minimum_required(VERSION 3.23)  # reflect-cpp requires >= 3.23

project(moqx VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# scripts/dev/lint.sh reads compile_commands.json from the build dir.
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Route the compiler through ccache when it is installed, without a toolchain edit.
if(NOT DEFINED CMAKE_CXX_COMPILER_LAUNCHER)
  find_program(CCACHE_PROGRAM ccache)
  if(CCACHE_PROGRAM)
    set(CMAKE_C_COMPILER_LAUNCHER "${CCACHE_PROGRAM}")
    set(CMAKE_CXX_COMPILER_LAUNCHER "${CCACHE_PROGRAM}")
    message(STATUS "ccache: enabled (${CCACHE_PROGRAM})")
  endif()
endif()

# Default to RelWithDebInfo for reproducible dev builds.
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "" FORCE)
endif()

# Defines MOQX_VERSION_STRING and the moqx_version interface target.
include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/MoqxVersion.cmake)

option(MOQX_BUILD_TESTS "Build tests" ON)
option(MOQX_BUILD_BENCHMARKS "Build benchmarks" OFF)
option(MOQX_ENABLE_SANITIZERS "Enable ASAN/UBSAN (non-Release)" OFF)
option(MOQX_ENABLE_TSAN "Enable TSan (non-Release)" OFF)
option(MOQX_MOXYGEN_PREBUILT
  "Auto-download a prebuilt moxygen install when none is found on CMAKE_PREFIX_PATH"
  ON)

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

include(cmake/SanitizerFlags.cmake)

if(MOQX_ENABLE_SANITIZERS AND NOT CMAKE_BUILD_TYPE STREQUAL "Release")
  add_compile_options(${MOQX_ASAN_FLAGS})
  add_link_options(${MOQX_ASAN_FLAGS})
endif()

if(MOQX_ENABLE_TSAN AND NOT CMAKE_BUILD_TYPE STREQUAL "Release")
  add_compile_options(${MOQX_TSAN_FLAGS})
  add_link_options(${MOQX_TSAN_FLAGS})
endif()

include(cmake/CheckSystemDeps.cmake)

# GFLAGS_SHARED=OFF makes moxygen's FindGflags steer glog at a second, static
# libgflags.a. Both copies load and gflags' startup self-check aborts on the
# duplicate registration.
set(GFLAGS_SHARED ON CACHE BOOL "")

# cmake/ carries Findc-ares.cmake, the sole resolver for proxygen's
# find_dependency(c-ares): some distros ship no c-ares CONFIG package, and
# moxygen's fbcode_builder only answers the different module name "Cares".
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

# Some fetched deps declare pre-3.5 minimums that CMake >= 4 refuses to
# configure (yaml-cpp 0.8.0: 3.4; catapult's bundled libcbor/doctest: 3.5).
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)

# Shared source cache with the superbuild, which defaults to the same path. This
# must be the CACHE entry: CPM declares its own, which drops a normal variable of
# the same name and switches the cache off on the first configure.
if(NOT DEFINED CACHE{CPM_SOURCE_CACHE} AND "$ENV{CPM_SOURCE_CACHE}" STREQUAL ""
   AND NOT "$ENV{HOME}" STREQUAL "")
  set(CPM_SOURCE_CACHE "$ENV{HOME}/.cache/moqx/cpm" CACHE PATH
      "Directory to download CPM dependencies")
endif()

include(cmake/CPM.cmake)
include(cmake/dependencies.cmake)

# --- moxygen (openmoq fork) ---------------------------------------------------
# The SOURCE is fetched even in prebuilt mode: the installed folly/moxygen configs
# resolve Glog/Sodium/&c. in MODULE mode and ship no Find*.cmake, so moxygen's
# fbcode_builder find-modules have to be on the module path. See BUILD.md.
CPMAddPackage(
  NAME moxygen
  GITHUB_REPOSITORY ${MOXYGEN_REPOSITORY}
  GIT_TAG ${MOXYGEN_REV}
  DOWNLOAD_ONLY YES
)
list(APPEND CMAKE_MODULE_PATH "${moxygen_SOURCE_DIR}/build/fbcode_builder/CMake")

# Folly's config calls FindBoost which was removed in CMake 3.30 (CMP0167).
# Folly itself works fine with Boost's own config-mode package; silence the
# policy warning until upstream fixes their config files.
if(POLICY CMP0167)
  cmake_policy(SET CMP0167 OLD)
endif()

# An install already on CMAKE_PREFIX_PATH wins, e.g. one from the superbuild.
# Otherwise MOQX_MOXYGEN_PREBUILT decides: ON downloads the prebuilt for
# MOXYGEN_REV, OFF demands a prefix. See BUILD.md.
find_package(moxygen QUIET CONFIG)
if(NOT moxygen_FOUND AND MOQX_MOXYGEN_PREBUILT)
  if(MOQX_ENABLE_SANITIZERS OR MOQX_ENABLE_TSAN)
    message(WARNING
      "Sanitizers are enabled but moxygen resolves to the UNINSTRUMENTED "
      "prebuilt — sanitizer coverage is limited to moqx's own TUs. For full "
      "coverage build an instrumented moxygen: "
      "scripts/configure.sh san|tsan --moxygen from-source (see BUILD.md).")
  endif()
  include(cmake/FetchMoxygenPrebuilt.cmake)
  find_package(moxygen REQUIRED CONFIG)
elseif(NOT moxygen_FOUND)
  message(FATAL_ERROR
    "moxygen not found and MOQX_MOXYGEN_PREBUILT=OFF.\n"
    "Build moxygen from source and point moqx at it:\n"
    "  scripts/configure.sh --moxygen from-source && scripts/build.sh\n"
    "(or pass -DCMAKE_PREFIX_PATH=<moxygen install>; see BUILD.md).")
endif()
message(STATUS "moxygen: using install at ${moxygen_DIR}")

# find_package caches moxygen_DIR and the folly/fizz/… tree with it, so a build
# dir binds to the moxygen it first resolved and a pin bump cannot take effect in
# place.
set(_moqx_moxygen_id "${MOXYGEN_REV}:${moxygen_DIR}")
if(DEFINED MOQX_RESOLVED_MOXYGEN AND NOT "${MOQX_RESOLVED_MOXYGEN}" STREQUAL "${_moqx_moxygen_id}")
  get_filename_component(_moqx_profile "${CMAKE_BINARY_DIR}" NAME)
  if(MOQX_MOXYGEN_PREBUILT)
    set(_moqx_moxygen "prebuilt")
  else()
    set(_moqx_moxygen "from-source")
  endif()
  message(FATAL_ERROR
    "MOXYGEN_REV (or the resolved moxygen install) changed since this build "
    "dir was configured; cached find_package results would mix the two.\n"
    "Reconfigure from scratch — this discards the build dir, so moqx recompiles "
    "in full (a moxygen bump invalidates every moqx TU anyway; ccache and the "
    "dependency cache survive):\n"
    "  scripts/configure.sh ${_moqx_profile} --moxygen ${_moqx_moxygen}\n"
    "(raw cmake: delete the build dir first).")
endif()
set(MOQX_RESOLVED_MOXYGEN "${_moqx_moxygen_id}" CACHE INTERNAL
    "moxygen identity this build dir resolved against")

# ABI-crossing folly inlines branch on kIsDebug (= !NDEBUG), so moqx and its
# moxygen must agree on NDEBUG or they violate ODR. Only Debug leaves it
# undefined, and only the superbuild's san/tsan profiles build a Debug moxygen.
if(CMAKE_BUILD_TYPE STREQUAL "Debug" AND NOT MOQX_ALLOW_ABI_SKEW
   AND ((NOT MOQX_ENABLE_SANITIZERS AND NOT MOQX_ENABLE_TSAN) OR MOQX_MOXYGEN_IS_PREBUILT))
  message(FATAL_ERROR
    "moqx is a Debug build but the moxygen it links is release-flavored (NDEBUG); "
    "folly's kIsDebug differs across the ABI boundary (ODR/UB). Use a "
    "RelWithDebInfo/Release build type (for sanitizers on the prebuilt, add "
    "-DCMAKE_BUILD_TYPE=RelWithDebInfo), or build an instrumented moxygen with "
    "scripts/configure.sh <preset> --moxygen from-source. Pass -DMOQX_ALLOW_ABI_SKEW=ON "
    "to build anyway.")
endif()

# moxygen's sample binaries (moqclient/…) live in <prefix>/bin, which integration
# tests reach through MOQBIN. moxygen_DIR is <prefix>/lib/cmake/moxygen.
get_filename_component(MOXYGEN_BIN_DIR "${moxygen_DIR}/../../../bin" ABSOLUTE)

# Test and perf scripts run outside ctest source this instead of re-deriving
# MOQBIN from CMakeCache.txt. Regenerated on every configure.
file(GENERATE OUTPUT "${CMAKE_BINARY_DIR}/moqx-tools.env"
     CONTENT "MOQBIN='${MOXYGEN_BIN_DIR}'\n")

find_package(OpenSSL REQUIRED)

# --- yaml-cpp (CPM) ---
CPMAddPackage(
        NAME yaml-cpp
        GITHUB_REPOSITORY jbeder/yaml-cpp
        GIT_TAG ${YAMLCPP_VERSION}
        OPTIONS
        "YAML_CPP_BUILD_TESTS OFF"
        "YAML_CPP_BUILD_TOOLS OFF"
        "YAML_CPP_FORMAT_SOURCE OFF"
        "CMAKE_WARN_DEPRECATED OFF"
)

# --- reflect-cpp (CPM, needs yaml-cpp available) ---

CPMAddPackage(
        NAME reflectcpp
        GITHUB_REPOSITORY getml/reflect-cpp
        GIT_TAG ${REFLECTCPP_VERSION}
        OPTIONS "REFLECTCPP_YAML ON" "REFLECTCPP_BUILD_TESTS OFF"
)

# --- XLOG category rooting ---
# Sources live under src/, but folly XLOG categories should read moqx.MoqxRelay
# rather than src.MoqxRelay. FOLLY_XLOG_STRIP_PREFIXES covers what the
# prefix-map misses, notably generated headers. Must precede the moqx_* targets.
add_compile_options(-fmacro-prefix-map=${CMAKE_CURRENT_SOURCE_DIR}/src=moqx)
add_compile_definitions(
  "FOLLY_XLOG_STRIP_PREFIXES=\"${CMAKE_SOURCE_DIR}:${CMAKE_BINARY_DIR}\""
)

# --- catapult (CAT/CWT/MOQT auth implementation) ---
# Nested deps (external/libcbor, …) arrive through FetchContent's recursive
# submodule clone. Develop against a local checkout with -DCPM_catapult_SOURCE,
# which must have its own submodules initialised.
CPMAddPackage(
  NAME catapult
  GITHUB_REPOSITORY ${CATAPULT_REPOSITORY}
  GIT_TAG ${CATAPULT_REV}
  EXCLUDE_FROM_ALL YES
  OPTIONS
    "ENABLE_LOGGING OFF"
    "CATAPULT_ENABLE_JSON OFF"
)

# Strict warnings for first-party code; linked PRIVATE per target so nothing
# propagates to consumers.
add_library(moqx_warnings INTERFACE)
target_compile_options(moqx_warnings INTERFACE -Wall -Wextra -Wpedantic)

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

target_link_libraries(moqx_cache PRIVATE moqx_warnings)

# --- HMAC key derivation (shared by the relay's verifier and the issuer) ---

add_library(moqx_hmac_key STATIC
  src/auth/HmacKey.cpp
)

target_include_directories(moqx_hmac_key
  PUBLIC
    ${PROJECT_SOURCE_DIR}/src
)

target_link_libraries(moqx_hmac_key PUBLIC OpenSSL::Crypto)

target_link_libraries(moqx_hmac_key PRIVATE moqx_warnings)

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

target_link_libraries(moqx_core PRIVATE moqx_warnings)
# PUBLIC so dependents can include moqx/Version.h.
target_link_libraries(moqx_core PUBLIC moqx_version)

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
# Pulls in rfl/yaml-cpp. Consumers that only need the resolved Config types can
# depend on moqx_config alone.

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

target_link_libraries(moqx_config_loader PRIVATE moqx_warnings)

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

target_link_libraries(moqx_issuer_lib PRIVATE moqx_warnings)

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

target_link_libraries(moqx-issuer PRIVATE moqx_warnings)

# --- Tests ---

if(MOQX_BUILD_TESTS)
  enable_testing()
  add_subdirectory(test)
endif()

if(MOQX_BUILD_BENCHMARKS)
  add_subdirectory(benchmark)
endif()

install(TARGETS moqx moqx-issuer)

# Identifies an unpacked tarball without running the binary.
install(FILES ${CMAKE_BINARY_DIR}/VERSION DESTINATION .)
