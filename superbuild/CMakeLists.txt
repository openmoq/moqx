# From-source build of moxygen (ExternalProject around moxygen's standalone/
# tree) — installs a CMake package prefix into <binary-dir>/moxygen-install for
# the moqx build to consume via find_package. moqx is NOT built here.
# Usage + knobs: README.md; how it fits the build: BUILD.md.

cmake_minimum_required(VERSION 3.23)
project(moqx-moxygen-superbuild NONE)

include(ExternalProject)

get_filename_component(_top "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)

# Same fail-fast dependency check as the moqx build — this build compiles the
# folly stack, where a missing -dev package surfaces far more cryptically.
include(${_top}/cmake/CheckSystemDeps.cmake)

# moqx is a separate CMake build, so the moxygen source needs one stable home
# both can read. This must be the CACHE entry: CPM declares its own, which drops
# a normal variable of the same name and switches the cache off.
if(NOT DEFINED CACHE{CPM_SOURCE_CACHE} AND "$ENV{CPM_SOURCE_CACHE}" STREQUAL "")
  if(NOT "$ENV{HOME}" STREQUAL "")
    set(_cpm_cache "$ENV{HOME}/.cache/moqx/cpm")
  else()
    set(_cpm_cache "${CMAKE_BINARY_DIR}/cpm-cache")
  endif()
  set(CPM_SOURCE_CACHE "${_cpm_cache}" CACHE PATH
      "Directory to download CPM dependencies")
endif()

include(${_top}/cmake/CPM.cmake)
include(${_top}/cmake/dependencies.cmake)
CPMAddPackage(
  NAME moxygen
  GITHUB_REPOSITORY ${MOXYGEN_REPOSITORY}
  GIT_TAG ${MOXYGEN_REV}
  DOWNLOAD_ONLY YES  # CPM only places the source; the ExternalProject builds it
)

find_program(CCACHE_PROGRAM ccache)
set(_launchers "")
if(CCACHE_PROGRAM)
  set(_launchers
    -DCMAKE_C_COMPILER_LAUNCHER=${CCACHE_PROGRAM}
    -DCMAKE_CXX_COMPILER_LAUNCHER=${CCACHE_PROGRAM})
endif()

# Mirrors moqx's san/tsan presets. moxygen has no sanitizer option, so it takes
# the raw flags, joined into strings to cross the ExternalProject boundary.
include(${_top}/cmake/SanitizerFlags.cmake)
set(MOQX_MOXYGEN_PROFILE "default" CACHE STRING
    "moxygen build profile: default | san | tsan")
set(_build_type RelWithDebInfo)
set(_san_flags "")          # -fsanitize=... for moxygen's compile + link
if(MOQX_MOXYGEN_PROFILE STREQUAL "default")
elseif(MOQX_MOXYGEN_PROFILE STREQUAL "san")
  set(_build_type Debug)
  string(JOIN " " _san_flags ${MOQX_ASAN_DEPS_FLAGS})
elseif(MOQX_MOXYGEN_PROFILE STREQUAL "tsan")
  set(_build_type Debug)
  string(JOIN " " _san_flags ${MOQX_TSAN_FLAGS})
else()
  message(FATAL_ERROR "MOQX_MOXYGEN_PROFILE must be default|san|tsan (got '${MOQX_MOXYGEN_PROFILE}')")
endif()
set(_moxygen_install "${CMAKE_BINARY_DIR}/moxygen-install")
message(STATUS "moxygen will install to ${_moxygen_install}")

# moxygen compile/link flags: sanitizers, plus a macOS-only fmt workaround —
# Apple Clang 21+ rejects fmt 10.2.1's consteval format-string check
# (fmtlib/fmt#4740), so make fmt use runtime checking there.
set(_moxygen_cxx "${_san_flags}")
if(APPLE)
  string(APPEND _moxygen_cxx " -DFMT_CONSTEVAL=")
endif()
set(_moxygen_flags "")
if(NOT _san_flags STREQUAL "")
  list(APPEND _moxygen_flags "-DCMAKE_C_FLAGS=${_san_flags}"
       "-DCMAKE_EXE_LINKER_FLAGS=${_san_flags}" "-DCMAKE_SHARED_LINKER_FLAGS=${_san_flags}")
endif()
string(STRIP "${_moxygen_cxx}" _moxygen_cxx)
if(NOT _moxygen_cxx STREQUAL "")
  list(APPEND _moxygen_flags "-DCMAKE_CXX_FLAGS=${_moxygen_cxx}")
endif()

# Editing a local moxygen checkout needs the build re-run on every `cmake --build`
# — ExternalProject stamps the build, so source edits alone don't invalidate it.
option(MOQX_MOXYGEN_BUILD_ALWAYS
  "Rebuild moxygen on every build (for editing a local moxygen checkout)" OFF)

# Static archives make the more portable install, but Fedora/RHEL package no
# static Boost, so the compiler driver decides. The env override is validated,
# not passed through: "auto" would otherwise read as truthy.
if(NOT DEFINED BOOST_USE_STATIC_LIBS)
  string(TOLOWER "$ENV{BOOST_USE_STATIC_LIBS}" _boost_env)
  if(_boost_env MATCHES "^(on|1|true|yes)$")
    set(BOOST_USE_STATIC_LIBS ON)
  elseif(_boost_env MATCHES "^(off|0|false|no)$")
    set(BOOST_USE_STATIC_LIBS OFF)
  elseif(_boost_env STREQUAL "" OR _boost_env STREQUAL "auto")
    find_program(_moqx_cxx NAMES $ENV{CXX} c++ g++ clang++)
    set(BOOST_USE_STATIC_LIBS ON)
    if(_moqx_cxx)
      # MOQX_BOOST_COMPONENTS comes from cmake/CheckSystemDeps.cmake, above.
      foreach(_comp IN LISTS MOQX_BOOST_COMPONENTS)
        execute_process(COMMAND "${_moqx_cxx}" -print-file-name=libboost_${_comp}.a
          OUTPUT_VARIABLE _loc OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
        if(NOT _loc MATCHES "^/")
          set(BOOST_USE_STATIC_LIBS OFF)
          break()
        endif()
      endforeach()
    endif()
  else()
    message(FATAL_ERROR
      "BOOST_USE_STATIC_LIBS must be auto|on|off (got '$ENV{BOOST_USE_STATIC_LIBS}')")
  endif()
endif()
message(STATUS "moxygen superbuild: Boost static libs: ${BOOST_USE_STATIC_LIBS}")

# INSTALL_DEPS ships folly and friends into the prefix. BUILD_TESTS gates the
# bundled GTest config that moqx's find_package(GTest) needs. BUILD_SAMPLES gives
# the integration tests the binaries they reach through MOXYGEN_BIN_DIR.
ExternalProject_Add(moxygen
  SOURCE_DIR    "${moxygen_SOURCE_DIR}"
  SOURCE_SUBDIR standalone
  INSTALL_DIR   "${_moxygen_install}"
  BUILD_ALWAYS  ${MOQX_MOXYGEN_BUILD_ALWAYS}
  USES_TERMINAL_CONFIGURE ON
  USES_TERMINAL_BUILD     ON
  USES_TERMINAL_INSTALL   ON
  CMAKE_ARGS
    -DCMAKE_BUILD_TYPE=${_build_type}
    # INSTALL_DIR only fills the <INSTALL_DIR> placeholder; the prefix must be
    # passed explicitly or the standalone build installs into /usr/local.
    -DCMAKE_INSTALL_PREFIX=${_moxygen_install}
    -DINSTALL_DEPS=ON
    -DBUILD_TESTS=ON
    -DBUILD_SAMPLES=ON
    -DBUILD_SHARED_LIBS=OFF
    -DBoost_USE_STATIC_LIBS=${BOOST_USE_STATIC_LIBS}
    # Never let moxygen's find_package probes satisfy themselves from a
    # PREVIOUS install in this prefix (e.g. its GTest probe finding the old
    # GTestConfig and skipping the bundled GoogleTest).
    -DCMAKE_FIND_NO_INSTALL_PREFIX=ON
    -DCMAKE_POLICY_VERSION_MINIMUM=3.5
    ${_moxygen_flags}
    ${_launchers}
)
