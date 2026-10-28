# From-source build of moxygen (ExternalProject around moxygen's standalone/
# tree) — installs a CMake package prefix into <binary-dir>/moxygen-install for
# the moqx build to consume via find_package. moqx is NOT built here.
# Usage + knobs: superbuild/README.md; how it fits: BUILD.md#how-dependencies-work.

cmake_minimum_required(VERSION 3.23)
project(moqx-moxygen-superbuild NONE)

include(ExternalProject)

get_filename_component(_top "${CMAKE_CURRENT_SOURCE_DIR}/.." ABSOLUTE)

# Same fail-fast dependency check as the moqx build — this build compiles the
# folly stack, where a missing -dev package surfaces far more cryptically.
include(${_top}/cmake/CheckSystemDeps.cmake)

# moqx is a separate CMake build, so the moxygen source needs one stable home
# both can read — the shared root the moqx build resolves the same way.
include(${_top}/cmake/DepsCache.cmake)

include(${_top}/cmake/CPM.cmake)
include(${_top}/cmake/dependencies.cmake)
CPMAddPackage(
  NAME moxygen
  GITHUB_REPOSITORY ${MOXYGEN_REPOSITORY}
  GIT_TAG ${MOXYGEN_REV}
  DOWNLOAD_ONLY YES  # CPM only places the source; the ExternalProject builds it
)

# CPM gives every rev its own source directory, and CMake refuses a build dir
# whose source moved. That binding lives only in CMakeCache.txt + CMakeFiles/;
# dropping just those keeps _deps/ (the folly stack) as ninja no-ops.
set(_moxygen_build "${CMAKE_BINARY_DIR}/moxygen-build")
if(EXISTS "${_moxygen_build}/CMakeCache.txt"
   AND NOT "${moxygen_SOURCE_DIR}" STREQUAL "${MOQX_MOXYGEN_SOURCE_DIR}")
  message(STATUS "moxygen: source moved to ${moxygen_SOURCE_DIR} — resetting ${_moxygen_build}'s cache (keeping _deps/)")
  file(REMOVE "${_moxygen_build}/CMakeCache.txt")
  file(REMOVE_RECURSE "${_moxygen_build}/CMakeFiles")
endif()
set(MOQX_MOXYGEN_SOURCE_DIR "${moxygen_SOURCE_DIR}" CACHE INTERNAL
    "moxygen source the ExternalProject below last configured against")

include(${_top}/cmake/PgoFlags.cmake)

find_program(CCACHE_PROGRAM ccache)
set(_launchers "")
if(NOT MOQX_PGO STREQUAL "off")
  # A list; LIST_SEPARATOR on the ExternalProject turns | back into ;.
  string(REPLACE ";" "|" _pgo_launcher "${MOQX_PGO_LAUNCHER}")
  set(_launchers
    -DCMAKE_C_COMPILER_LAUNCHER=${_pgo_launcher}
    -DCMAKE_CXX_COMPILER_LAUNCHER=${_pgo_launcher})
elseif(CCACHE_PROGRAM)
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

# Built the way moxygen's portable release is, for moqx's portable tarballs.
option(MOQX_MOXYGEN_PORTABLE
  "Build moxygen like its portable release: Release, static system libs, no libunwind" OFF)
set(MOQX_MOXYGEN_CXX_FLAGS "" CACHE STRING
    "Extra C and C++ compile flags for moxygen and its stack, e.g. -march")
set(_portable_args "")
if(MOQX_MOXYGEN_PORTABLE OR NOT MOQX_PGO STREQUAL "off" OR MOQX_LTO)
  if(NOT MOQX_MOXYGEN_PROFILE STREQUAL "default")
    message(FATAL_ERROR
      "MOQX_MOXYGEN_PROFILE=${MOQX_MOXYGEN_PROFILE} does not combine with "
      "MOQX_MOXYGEN_PORTABLE, MOQX_PGO or MOQX_LTO, which build Release")
  endif()
  set(_build_type Release)
endif()
if(MOQX_MOXYGEN_PORTABLE)
  list(APPEND _portable_args
    -DBUNDLE_DEPS=ON -DBUNDLE_DEPS_STATIC_SYSLIBS=ON -DBUILD_TESTING=OFF
    -DCMAKE_POSITION_INDEPENDENT_CODE=ON -DCMAKE_DISABLE_FIND_PACKAGE_LibUnwind=TRUE)
endif()

# moxygen compile/link flags: sanitizers, extra and PGO/LTO flags, plus a macOS
# fmt workaround. Apple Clang 21+ rejects fmt 10.2.1's consteval format-string
# check (fmtlib/fmt#4740); applied on all macOS since project(NONE) leaves no
# compiler version to gate on.
set(_c ${_san_flags} ${MOQX_MOXYGEN_CXX_FLAGS} ${MOQX_OPT_COMPILE_FLAGS})
set(_ld ${_san_flags} ${MOQX_OPT_DEPS_LINK_FLAGS})
# As moxygen's portable lane: keeps Ubuntu's fat-LTO static system libs out of LTO.
if(MOQX_MOXYGEN_PORTABLE AND NOT MOQX_LTO)
  list(APPEND _c -fno-lto)
  list(APPEND _ld -fno-lto)
endif()
set(_cxx ${_c})
if(APPLE)
  list(APPEND _cxx -DFMT_CONSTEVAL=)
endif()
string(JOIN " " _moxygen_c ${_c})
string(JOIN " " _moxygen_cxx ${_cxx})
string(JOIN " " _moxygen_ld ${_ld})
set(_moxygen_flags "")
if(NOT _moxygen_c STREQUAL "")
  list(APPEND _moxygen_flags "-DCMAKE_C_FLAGS=${_moxygen_c}")
endif()
if(NOT _moxygen_cxx STREQUAL "")
  list(APPEND _moxygen_flags "-DCMAKE_CXX_FLAGS=${_moxygen_cxx}")
endif()
if(NOT _moxygen_ld STREQUAL "")
  list(APPEND _moxygen_flags
       "-DCMAKE_EXE_LINKER_FLAGS=${_moxygen_ld}" "-DCMAKE_SHARED_LINKER_FLAGS=${_moxygen_ld}")
endif()

# Editing a local moxygen checkout needs the build re-run on every `cmake --build`
# — ExternalProject stamps the build, so source edits alone don't invalidate it.
option(MOQX_MOXYGEN_BUILD_ALWAYS
  "Rebuild moxygen on every build (for editing a local moxygen checkout)" OFF)

# Static archives make the more portable install, but Fedora/RHEL package no
# static Boost, so under "auto" the compiler driver decides. Both spellings, -D
# and env, are validated rather than passed through: "auto" reaching
# Boost_USE_STATIC_LIBS below would read as truthy and force static anyway.
if(DEFINED BOOST_USE_STATIC_LIBS)
  set(_boost_req "${BOOST_USE_STATIC_LIBS}")
else()
  set(_boost_req "$ENV{BOOST_USE_STATIC_LIBS}")
endif()
string(TOLOWER "${_boost_req}" _boost_req)
if(_boost_req MATCHES "^(on|1|true|yes)$")
  set(BOOST_USE_STATIC_LIBS ON)
elseif(_boost_req MATCHES "^(off|0|false|no)$")
  set(BOOST_USE_STATIC_LIBS OFF)
elseif(_boost_req STREQUAL "" OR _boost_req STREQUAL "auto")
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
    "BOOST_USE_STATIC_LIBS must be auto|on|off (got '${_boost_req}')")
endif()
message(STATUS "moxygen superbuild: Boost static libs: ${BOOST_USE_STATIC_LIBS}")

# INSTALL_DEPS ships folly and friends into the prefix. BUILD_TESTS gates the
# bundled GTest config that moqx's find_package(GTest) needs. BUILD_SAMPLES gives
# the integration tests the binaries they reach through MOXYGEN_BIN_DIR.
# PGO/LTO builds serve a moqx without tests: libraries only, plus the moqtest
# tools for the training pass. Non-LTO links of moxygen executables from
# profile-use fat objects fail with duplicate definitions.
set(_build_tests ON)
set(_build_samples ON)
set(_build_moqtest ON)
if(NOT MOQX_PGO STREQUAL "off" OR MOQX_LTO)
  set(_build_tests OFF)
  set(_build_samples OFF)
  if(NOT MOQX_PGO STREQUAL "generate")
    set(_build_moqtest OFF)
  endif()
endif()
ExternalProject_Add(moxygen
  SOURCE_DIR    "${moxygen_SOURCE_DIR}"
  SOURCE_SUBDIR standalone
  # Named rather than left at ExternalProject's default, so the pin-change wipe
  # above owns the path instead of reaching into moxygen-prefix/src/.
  BINARY_DIR    "${_moxygen_build}"
  INSTALL_DIR   "${_moxygen_install}"
  BUILD_ALWAYS  ${MOQX_MOXYGEN_BUILD_ALWAYS}
  LIST_SEPARATOR |
  USES_TERMINAL_CONFIGURE ON
  USES_TERMINAL_BUILD     ON
  USES_TERMINAL_INSTALL   ON
  # Replace the prefix, don't merge into it: a rev bump reuses this build dir,
  # and anything the new moxygen no longer installs would linger for moqx's
  # find_package and includes to pick up.
  INSTALL_COMMAND ${CMAKE_COMMAND} -E rm -rf <INSTALL_DIR>
          COMMAND ${CMAKE_COMMAND} --install <BINARY_DIR> --config ${_build_type}
  CMAKE_ARGS
    -DCMAKE_BUILD_TYPE=${_build_type}
    # INSTALL_DIR only fills the <INSTALL_DIR> placeholder; the prefix must be
    # passed explicitly or the standalone build installs into /usr/local.
    -DCMAKE_INSTALL_PREFIX=${_moxygen_install}
    -DINSTALL_DEPS=ON
    -DBUILD_TESTS=${_build_tests}
    -DBUILD_SAMPLES=${_build_samples}
    -DBUILD_MOQTEST=${_build_moqtest}
    # Discover tests at ctest startup instead of POST_BUILD: parallel POST_BUILD
    # discovery races on a shared JSON file under cmake 4.4.0.
    -DCMAKE_GTEST_DISCOVER_TESTS_DISCOVERY_MODE=PRE_TEST
    -DBUILD_SHARED_LIBS=OFF
    -DBoost_USE_STATIC_LIBS=${BOOST_USE_STATIC_LIBS}
    # Never let moxygen's find_package probes satisfy themselves from a
    # PREVIOUS install in this prefix (e.g. its GTest probe finding the old
    # GTestConfig and skipping the bundled GoogleTest).
    -DCMAKE_FIND_NO_INSTALL_PREFIX=ON
    -DCMAKE_POLICY_VERSION_MINIMUM=3.5
    ${_portable_args}
    ${_moxygen_flags}
    ${_launchers}
)
