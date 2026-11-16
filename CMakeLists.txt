cmake_minimum_required(VERSION 3.23)  # reflect-cpp requires >= 3.23

project(moqx VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# scripts/dev/lint.sh reads compile_commands.json from the build dir.
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

include(cmake/PgoFlags.cmake)

# Route the compiler through ccache when it is installed, without a toolchain edit.
# PGO builds run every compile with ccache disabled instead (cmake/PgoFlags.cmake).
if(NOT MOQX_PGO STREQUAL "off")
  set(CMAKE_C_COMPILER_LAUNCHER ${MOQX_PGO_LAUNCHER})
  set(CMAKE_CXX_COMPILER_LAUNCHER ${MOQX_PGO_LAUNCHER})
elseif(NOT DEFINED CMAKE_CXX_COMPILER_LAUNCHER)
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

# The sanitizer profile this build carries — the enable options are inert
# under Release. Derived once, so profile-keyed checks cannot drift from the
# flags actually applied.
set(_moqx_own_profile "default")
if(MOQX_ENABLE_SANITIZERS AND NOT CMAKE_BUILD_TYPE STREQUAL "Release")
  set(_moqx_own_profile "san")
elseif(MOQX_ENABLE_TSAN AND NOT CMAKE_BUILD_TYPE STREQUAL "Release")
  set(_moqx_own_profile "tsan")
endif()

if(_moqx_own_profile STREQUAL "san")
  add_compile_options(${MOQX_ASAN_FLAGS})
  add_link_options(${MOQX_ASAN_FLAGS})
elseif(_moqx_own_profile STREQUAL "tsan")
  add_compile_options(${MOQX_TSAN_FLAGS})
  add_link_options(${MOQX_TSAN_FLAGS})
endif()

add_compile_options(${MOQX_OPT_COMPILE_FLAGS})
add_link_options(${MOQX_OPT_LINK_FLAGS})

include(cmake/CheckSystemDeps.cmake)

# GFLAGS_SHARED=OFF makes moxygen's FindGflags steer glog at a second, static
# libgflags.a. Both copies load and gflags' startup self-check aborts on the
# duplicate registration, at test startup, with nothing naming the cause.
# FORCE, so a -DGFLAGS_SHARED=OFF on the command line cannot reintroduce it.
set(GFLAGS_SHARED ON CACHE BOOL "" FORCE)

# cmake/ carries Findc-ares.cmake, the sole resolver for proxygen's
# find_dependency(c-ares): some distros ship no c-ares CONFIG package, and
# moxygen's fbcode_builder only answers the different module name "Cares".
list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

# Some fetched deps declare pre-3.5 minimums that CMake >= 4 refuses to
# configure (yaml-cpp 0.8.0: 3.4; catapult's bundled libcbor/doctest: 3.5).
set(CMAKE_POLICY_VERSION_MINIMUM 3.5)

# MOQX_DEPS_CACHE + CPM_SOURCE_CACHE, shared with the superbuild.
include(cmake/DepsCache.cmake)

include(cmake/CPM.cmake)
include(cmake/dependencies.cmake)

# --- moxygen (openmoq fork) ---------------------------------------------------
# The SOURCE is fetched even in prebuilt mode: the installed folly/moxygen configs
# resolve Glog/Sodium/&c. in MODULE mode and ship no Find*.cmake, so moxygen's
# fbcode_builder find-modules have to be on the module path. See BUILD.md#how-dependencies-work.
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
# MOXYGEN_REV, OFF demands a prefix. See BUILD.md#how-dependencies-work.
find_package(moxygen QUIET CONFIG)
if(NOT moxygen_FOUND AND MOQX_MOXYGEN_PREBUILT)
  # The published prebuilts are uninstrumented; refuse before the
  # multi-hundred-MB download, not at the probe after it.
  if(NOT _moqx_own_profile STREQUAL "default" AND NOT MOQX_ALLOW_ABI_SKEW)
    message(FATAL_ERROR
      "moqx builds the '${_moqx_own_profile}' profile but MOQX_MOXYGEN_PREBUILT=ON "
      "resolves to the uninstrumented prebuilt. Build a matching stack:\n"
      "  scripts/configure.sh ${_moqx_own_profile} --moxygen from-source\n"
      "(or pass -DMOQX_ALLOW_ABI_SKEW=ON to knowingly link the uninstrumented one).")
  endif()
  # The prebuilt carries neither a profile nor LTO bytecode.
  if(NOT MOQX_PGO STREQUAL "off" OR MOQX_LTO)
    message(FATAL_ERROR
      "MOQX_PGO=${MOQX_PGO} MOQX_LTO=${MOQX_LTO} needs the stack built with the same flags, "
      "not the prebuilt:\n"
      "  scripts/configure.sh release-pgo --moxygen from-source")
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

# The sanitizer profile the moxygen stack carries, measured: every TU compiled
# with -fsanitize references __asan_init/__tsan_init, so one nm probe of an
# installed archive answers for any prefix, downloads included.
get_filename_component(_moqx_moxygen_prefix "${moxygen_DIR}/../../.." ABSOLUTE)
if(NOT CMAKE_NM)
  find_program(CMAKE_NM nm)
endif()
# Smallest archive wins: the probe reads the whole file, and every moxygen
# archive carries at least one instrumented TU.
file(GLOB _moqx_probe_candidates "${_moqx_moxygen_prefix}/lib/libmoxygen*.a"
     "${_moqx_moxygen_prefix}/lib64/libmoxygen*.a")
set(_moqx_probe_lib "")
foreach(_lib IN LISTS _moqx_probe_candidates)
  file(SIZE "${_lib}" _sz)
  if(_moqx_probe_lib STREQUAL "" OR _sz LESS _moqx_probe_size)
    set(_moqx_probe_lib "${_lib}")
    set(_moqx_probe_size "${_sz}")
  endif()
endforeach()
set(_moqx_moxygen_profile "")
if(CMAKE_NM AND NOT _moqx_probe_lib STREQUAL "")
  execute_process(
    COMMAND "${CMAKE_NM}" -u "${_moqx_probe_lib}"
    COMMAND grep -m1 -oE "__(a|t)san_init"
    RESULTS_VARIABLE _moqx_probe_rcs
    OUTPUT_VARIABLE _moqx_probe_sym
    OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
  list(GET _moqx_probe_rcs 0 _moqx_nm_rc)
  list(GET _moqx_probe_rcs 1 _moqx_grep_rc)
  if(_moqx_nm_rc EQUAL 0)
    if(_moqx_probe_sym STREQUAL "__asan_init")
      set(_moqx_moxygen_profile "san")
    elseif(_moqx_probe_sym STREQUAL "__tsan_init")
      set(_moqx_moxygen_profile "tsan")
    elseif(_moqx_grep_rc EQUAL 1)
      set(_moqx_moxygen_profile "default")
    endif()
  endif()
endif()
if(_moqx_moxygen_profile STREQUAL "")
  if(MOQX_ALLOW_ABI_SKEW)
    message(WARNING
      "could not verify the sanitizer instrumentation of the moxygen at "
      "${_moqx_moxygen_prefix} (nm or a libmoxygen*.a archive missing) — "
      "linking it unverified (MOQX_ALLOW_ABI_SKEW).")
  else()
    message(FATAL_ERROR
      "could not verify the sanitizer instrumentation of the moxygen at\n"
      "  ${_moqx_moxygen_prefix}\n"
      "(nm or a libmoxygen*.a archive missing). Pass -DMOQX_ALLOW_ABI_SKEW=ON "
      "to link it unverified.")
  endif()
elseif(NOT _moqx_moxygen_profile STREQUAL _moqx_own_profile)
  if(NOT _moqx_own_profile STREQUAL "default" AND _moqx_moxygen_profile STREQUAL "default")
    # The one opt-in-able degradation: knowingly sanitize moqx's TUs alone.
    if(MOQX_ALLOW_ABI_SKEW)
      message(WARNING
        "Sanitizers are enabled but the moxygen at ${_moqx_moxygen_prefix} "
        "carries no sanitizer instrumentation — coverage is limited to moqx's "
        "own TUs.")
    else()
      message(FATAL_ERROR
        "moqx builds the '${_moqx_own_profile}' profile but the moxygen at\n"
        "  ${_moqx_moxygen_prefix}\n"
        "carries no sanitizer instrumentation; coverage would silently stop at "
        "moqx's own TUs. Build a matching stack:\n"
        "  scripts/configure.sh ${_moqx_own_profile} --moxygen from-source\n"
        "(or pass -DMOQX_ALLOW_ABI_SKEW=ON to knowingly link the uninstrumented one).")
    endif()
  else()
    message(FATAL_ERROR
      "the moxygen at\n"
      "  ${_moqx_moxygen_prefix}\n"
      "is ${_moqx_moxygen_profile}-instrumented but moqx builds "
      "'${_moqx_own_profile}'; the link would fail or misbehave (sanitizer "
      "runtime/interceptor mismatch, Debug-built deps skewing kIsDebug). No "
      "opt-in covers this — build the matching stack:\n"
      "  scripts/configure.sh ${_moqx_own_profile} --moxygen from-source\n"
      "(or configure the '${_moqx_moxygen_profile}' preset to match this install).")
  endif()
endif()

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
# tests reach through MOQBIN.
set(MOXYGEN_BIN_DIR "${_moqx_moxygen_prefix}/bin")

# Test and perf scripts run outside ctest source this instead of re-deriving
# MOQBIN from CMakeCache.txt. Regenerated on every configure.
file(GENERATE OUTPUT "${CMAKE_BINARY_DIR}/moqx-tools.env"
     CONTENT "MOQBIN='${MOXYGEN_BIN_DIR}'\n")

find_package(OpenSSL REQUIRED)
# Other deps' include dirs can hold headers from a different OpenSSL.
include_directories(BEFORE SYSTEM ${OPENSSL_INCLUDE_DIR})

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

# --- XLOG category and debug path rooting ---
# Sources live under src/, but folly XLOG categories should read moqx.MoqxRelay
# rather than src.MoqxRelay. Must precede the moqx_* targets.
#
# - Both spellings of every map: ccache with base_dir rewrites the source path
#   it forwards to the compiler, but not -f*-prefix-map.
# - macro- and debug- rather than file-: categories want an opaque root, debug
#   info wants paths a debugger started at the worktree root can open.
# - STRIP_PREFIXES takes the build root alone, which leaves generated headers
#   at generated.Foo. Adding moqx would strip the root this block exists to set.
file(RELATIVE_PATH _moqx_src_rel ${CMAKE_BINARY_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/src)
file(RELATIVE_PATH _moqx_root_rel ${CMAKE_BINARY_DIR} ${CMAKE_CURRENT_SOURCE_DIR})
# RELATIVE_PATH yields "../../" here, and GCC swallows the separator with the
# prefix, so an unstripped value maps ../../src to .src rather than ./src.
string(REGEX REPLACE "/$" "" _moqx_root_rel "${_moqx_root_rel}")
add_compile_options(
  -fmacro-prefix-map=${CMAKE_BINARY_DIR}=moqx-build
  -fmacro-prefix-map=${CMAKE_CURRENT_SOURCE_DIR}/src=moqx
  -fmacro-prefix-map=${_moqx_src_rel}=moqx
  -fdebug-prefix-map=${CMAKE_CURRENT_SOURCE_DIR}=.
  -fdebug-prefix-map=${_moqx_root_rel}=.)
add_compile_definitions("FOLLY_XLOG_STRIP_PREFIXES=\"moqx-build\"")

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
# -Wmissing-field-initializers off: GCC fires it on C++20 designated initializers
# that omit members carrying default member initializers — the idiom throughout
# src/ and test/, and correct. Clang does not warn there.
add_library(moqx_warnings INTERFACE)
target_compile_options(moqx_warnings INTERFACE
  -Wall -Wextra -Wpedantic -Wno-missing-field-initializers)

# --- moq-peering --------------------------------------------------------------
# A static archive. moq-peering's picoquic is linked into one relocatable
# object and every symbol except MoqPeeringRuntime is made local, so this copy
# stays separate from the picoquic moxygen links into the relay. Objects are
# built hidden and position-independent.
set(MOQ_PEERING_BUILD_TESTS OFF CACHE BOOL "Build moq-peering tests")
set(MOQ_PEERING_BUILD_EXAMPLES OFF CACHE BOOL "Build moq-peering examples")
set(MOQ_PEERING_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../moq-peering" CACHE PATH
    "Local moq-peering checkout. Used when that directory contains CMakeLists.txt.")

set(_moqx_peering_src "")
if(EXISTS "${MOQ_PEERING_SOURCE_DIR}/CMakeLists.txt")
  set(_moqx_peering_src "${MOQ_PEERING_SOURCE_DIR}")
elseif(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../../ws-media10x/moq-peering/CMakeLists.txt")
  set(_moqx_peering_src "${CMAKE_CURRENT_SOURCE_DIR}/../../ws-media10x/moq-peering")
endif()

# spdlog 1.17 does not compile against the fmt folly already provides. Bundle
# fmt inside spdlog, and add that target before moq-peering so the library
# keeps it instead of switching on SPDLOG_FMT_EXTERNAL.
set(SPDLOG_FMT_EXTERNAL OFF CACHE BOOL
    "Bundle fmt inside the moq-peering library" FORCE)
CPMAddPackage("gh:gabime/spdlog@1.17.0")

if(_moqx_peering_src)
  message(STATUS "moq-peering: using ${_moqx_peering_src}")
  CPMAddPackage(
    NAME moq-peering
    SOURCE_DIR "${_moqx_peering_src}"
  )
else()
  message(STATUS "moq-peering: fetching ${MOQ_PEERING_REPOSITORY}@${MOQ_PEERING_REV}")
  CPMAddPackage(
    NAME moq-peering
    GITHUB_REPOSITORY ${MOQ_PEERING_REPOSITORY}
    GIT_TAG ${MOQ_PEERING_REV}
  )
endif()

function(moqx_hide_peering_target tgt)
  if(NOT TARGET ${tgt})
    return()
  endif()
  get_target_property(_moqx_type ${tgt} TYPE)
  if(_moqx_type STREQUAL "INTERFACE_LIBRARY" OR _moqx_type STREQUAL "UTILITY")
    return()
  endif()
  set_target_properties(${tgt} PROPERTIES
    C_VISIBILITY_PRESET hidden
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON
    POSITION_INDEPENDENT_CODE ON
  )
  # Localization happens on the native object. LTO bytecode would undo it.
  if(MOQX_LTO)
    target_compile_options(${tgt} PRIVATE -fno-lto)
  endif()
  set_property(GLOBAL APPEND PROPERTY MOQX_HIDDEN_PEERING_TARGETS "${tgt}")
endfunction()

function(moqx_hide_peering_dir dir)
  get_property(_moqx_tgts DIRECTORY "${dir}" PROPERTY BUILDSYSTEM_TARGETS)
  foreach(_moqx_tgt IN LISTS _moqx_tgts)
    moqx_hide_peering_target(${_moqx_tgt})
  endforeach()
  get_property(_moqx_subs DIRECTORY "${dir}" PROPERTY SUBDIRECTORIES)
  foreach(_moqx_sub IN LISTS _moqx_subs)
    moqx_hide_peering_dir("${_moqx_sub}")
  endforeach()
endfunction()

set_property(GLOBAL PROPERTY MOQX_HIDDEN_PEERING_TARGETS "")
if(DEFINED spdlog_SOURCE_DIR)
  moqx_hide_peering_dir("${spdlog_SOURCE_DIR}")
endif()
if(DEFINED moq-peering_SOURCE_DIR)
  moqx_hide_peering_dir("${moq-peering_SOURCE_DIR}")
endif()
get_property(_moqx_hidden_targets GLOBAL PROPERTY MOQX_HIDDEN_PEERING_TARGETS)
list(LENGTH _moqx_hidden_targets _moqx_hidden_count)
if(_moqx_hidden_count EQUAL 0)
  message(FATAL_ERROR "moq-peering: failed to mark its targets hidden")
endif()
message(STATUS "moq-peering: hid ${_moqx_hidden_count} targets inside libmoqx_peering")

# Static libraries in the moq::peering closure are absorbed into one object.
# Shared libraries and link flags (OpenSSL, brotli) stay on the final link.
# picotls records brotli with link_directories() plus -lbrotlidec, which does
# not travel with a bare -l flag, so resolve those against the collected dirs.
function(moqx_peering_resolve_link item dirs out)
  set(_name "")
  if(item MATCHES "^-l(.+)$")
    set(_name "${CMAKE_MATCH_1}")
  elseif(item MATCHES "^[A-Za-z0-9_.+-]+$")
    # picotls records brotli as a bare library name, not a target.
    set(_name "${item}")
  endif()
  if(_name)
    foreach(_dir IN LISTS dirs)
      foreach(_ext IN ITEMS .dylib .so .a .tbd)
        if(EXISTS "${_dir}/lib${_name}${_ext}")
          set(${out} "${_dir}/lib${_name}${_ext}" PARENT_SCOPE)
          return()
        endif()
      endforeach()
    endforeach()
  endif()
  set(${out} "${item}" PARENT_SCOPE)
endfunction()

function(moqx_peering_closure root static_out link_out)
  set(_queue "${root}")
  set(_seen "")
  set(_static "")
  set(_raw_link "")
  set(_dirs "")
  while(_queue)
    list(POP_FRONT _queue _item)
    if(_item IN_LIST _seen)
      continue()
    endif()
    list(APPEND _seen "${_item}")
    if(_item MATCHES "^\\$<")
      if(_item MATCHES "^\\$<LINK_ONLY:(.+)>$")
        list(APPEND _queue "${CMAKE_MATCH_1}")
      else()
        message(FATAL_ERROR "moq-peering: unrecognized link item '${_item}'")
      endif()
      continue()
    endif()
    if(NOT TARGET "${_item}")
      list(APPEND _raw_link "${_item}")
      continue()
    endif()
    get_target_property(_aliased "${_item}" ALIASED_TARGET)
    if(_aliased)
      list(APPEND _queue "${_aliased}")
      continue()
    endif()
    get_target_property(_type "${_item}" TYPE)
    if(_type STREQUAL "STATIC_LIBRARY")
      list(APPEND _static "${_item}")
    elseif(_type STREQUAL "SHARED_LIBRARY" OR _type STREQUAL "UNKNOWN_LIBRARY")
      list(APPEND _raw_link "${_item}")
      continue()
    elseif(NOT _type STREQUAL "INTERFACE_LIBRARY")
      continue()
    endif()
    foreach(_prop LINK_DIRECTORIES INTERFACE_LINK_DIRECTORIES)
      get_target_property(_ldirs "${_item}" ${_prop})
      if(_ldirs)
        list(APPEND _dirs ${_ldirs})
      endif()
    endforeach()
    foreach(_dirprop BINARY_DIR SOURCE_DIR)
      get_target_property(_tdir "${_item}" ${_dirprop})
      if(_tdir)
        get_property(_dldirs DIRECTORY "${_tdir}" PROPERTY LINK_DIRECTORIES)
        if(_dldirs)
          list(APPEND _dirs ${_dldirs})
        endif()
      endif()
    endforeach()
    foreach(_prop LINK_LIBRARIES INTERFACE_LINK_LIBRARIES)
      get_target_property(_deps "${_item}" ${_prop})
      if(_deps)
        list(APPEND _queue ${_deps})
      endif()
    endforeach()
  endwhile()
  if(_dirs)
    list(REMOVE_DUPLICATES _dirs)
  endif()
  set(_link "")
  foreach(_item IN LISTS _raw_link)
    moqx_peering_resolve_link("${_item}" "${_dirs}" _resolved)
    list(APPEND _link "${_resolved}")
  endforeach()
  if(_link)
    list(REMOVE_DUPLICATES _link)
  endif()
  set(${static_out} "${_static}" PARENT_SCOPE)
  set(${link_out} "${_link}" PARENT_SCOPE)
endfunction()

moqx_peering_closure(moq::peering _moqx_peering_static _moqx_peering_link)
if(NOT _moqx_peering_static)
  message(FATAL_ERROR "moq-peering: no static libraries to bundle")
endif()
message(STATUS "moq-peering: bundling ${_moqx_peering_static}")
message(STATUS "moq-peering: linking ${_moqx_peering_link}")

if(NOT APPLE)
  find_program(MOQX_OBJCOPY NAMES objcopy llvm-objcopy REQUIRED)
endif()

add_library(moqx_peering_obj OBJECT
  src/peering/MoqPeeringRuntime.cpp
)
target_include_directories(moqx_peering_obj
  PUBLIC
    ${PROJECT_SOURCE_DIR}/src
)
target_link_libraries(moqx_peering_obj
  PRIVATE
    moq::peering
    moqx_warnings
)
set_target_properties(moqx_peering_obj PROPERTIES
  CXX_VISIBILITY_PRESET hidden
  VISIBILITY_INLINES_HIDDEN ON
  POSITION_INDEPENDENT_CODE ON
)
if(MOQX_LTO)
  target_compile_options(moqx_peering_obj PRIVATE -fno-lto)
endif()

set(_moqx_peering_bundle "${CMAKE_CURRENT_BINARY_DIR}/moqx_peering_bundle.o")
set(_moqx_peering_archives "")
foreach(_moqx_tgt IN LISTS _moqx_peering_static)
  list(APPEND _moqx_peering_archives "$<TARGET_FILE:${_moqx_tgt}>")
endforeach()
add_custom_command(
  OUTPUT "${_moqx_peering_bundle}"
  COMMAND ${CMAKE_COMMAND}
    -DMOQX_COMPILER=${CMAKE_CXX_COMPILER}
    -DMOQX_NM=${CMAKE_NM}
    -DMOQX_OUTPUT=${_moqx_peering_bundle}
    -DMOQX_APPLE=${APPLE}
    -DMOQX_OBJCOPY=${MOQX_OBJCOPY}
    -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/bundle-peering.cmake
    --
    $<TARGET_OBJECTS:moqx_peering_obj>
    ${_moqx_peering_archives}
  DEPENDS moqx_peering_obj ${_moqx_peering_static}
    "${CMAKE_CURRENT_SOURCE_DIR}/cmake/bundle-peering.cmake"
  COMMENT "Bundling moq-peering into a static object"
  VERBATIM
)
set_source_files_properties("${_moqx_peering_bundle}" PROPERTIES
  EXTERNAL_OBJECT TRUE
  GENERATED TRUE
)
add_library(moqx_peering STATIC "${_moqx_peering_bundle}")
set_target_properties(moqx_peering PROPERTIES LINKER_LANGUAGE CXX)
target_include_directories(moqx_peering
  PUBLIC
    ${PROJECT_SOURCE_DIR}/src
)
if(_moqx_peering_link)
  target_link_libraries(moqx_peering PRIVATE ${_moqx_peering_link})
endif()

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
  src/TracksTree.cpp
  src/PendingRendezvousTree.cpp
  src/SubscriptionRegistry.cpp
  src/MoqxRelay.cpp
  src/MoqxRelayContext.cpp
  src/MoqxRelayServer.cpp
  src/MoqxPicoRelayServer.cpp
  src/MoqxQmuxRelayServer.cpp
  src/ServiceMatcher.cpp
  src/tls/FizzContextBuilder.cpp
  src/admin/AdminServer.cpp
  src/admin/BuiltinRoutes.cpp
  src/admin/CachePurgeHandler.cpp
  src/admin/ConfigHandler.cpp
  src/admin/ConnectionLogsHandler.cpp
  src/admin/MetricsHandler.cpp
  src/admin/PeeringHandler.cpp
  src/admin/QLogCaptureHandler.cpp
  src/admin/TrackMetricsHandler.cpp
  src/admin/StateHandler.cpp
  src/admin/StateResponse.cpp
  src/peering/PeerJson.cpp
  src/peering/MoqPeeringBackend.cpp
  src/peering/MoqPeeringParticipant.cpp
  src/peering/PeeringObject.cpp
  src/stats/StatsRegistry.cpp
  src/stats/TrackStatsRegistry.cpp
  src/stats/MoQStatsCollector.cpp
  src/stats/PicoQuicStatsCollector.cpp
  src/stats/QuicStatsCollector.cpp
  src/stats/EventBaseStatsCollector.cpp
  src/UpstreamProvider.cpp
  src/peering/MoqtRelayBackend.cpp
  src/relay/MoqClusterRouteCodec.cpp
  src/SafeTrackName.cpp
  src/relay/AuthFilters.cpp
  src/relay/Participant.cpp
  src/relay/SessionParticipant.cpp
  src/relay/TopNFilter.cpp
  src/relay/TrackStatsFilter.cpp
  src/relay/PropertyRanking.cpp
  src/relay/CrossExecFilter.cpp
  src/relay/CrossExecForwarderCallback.cpp
  src/relay/WeakRelayForwarderCallback.cpp
  src/relay/PublisherCrossExecFilter.cpp
  src/relay/SubscriberCrossExecFilter.cpp
  src/logging/LogSetup.cpp
  src/logging/QLogCapture.cpp
)

target_include_directories(moqx_core
  PUBLIC
    ${PROJECT_SOURCE_DIR}/src
)

target_link_libraries(moqx_core PUBLIC
  moqx_cache
  moqx_hmac_key
  moqx_peering
  moxygen::moxygen_relay_moq_forwarder
  moxygen::moxygen_moq_relay_session
  moxygen::moxygen_moq_server
  moxygen::moxygen_moq_qmux_server
  moxygen::moxygen_qmux_utils
  moxygen::openmoq_pico_evb_sharded_server_transport
  moxygen::openmoq_extensions
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
  src/config/YamlDuplicateKeys.cpp
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
  # A PGO/LTO superbuild leaves out moxygen's tests and the GTest config they install.
  find_package(GTest QUIET CONFIG)
  if(NOT GTest_FOUND AND (NOT MOQX_PGO STREQUAL "off" OR MOQX_LTO))
    message(FATAL_ERROR
      "MOQX_BUILD_TESTS needs GTest, which a PGO/LTO moxygen build does not install; "
      "pass -DMOQX_BUILD_TESTS=OFF")
  endif()
  enable_testing()
  add_subdirectory(test)
endif()

if(MOQX_BUILD_BENCHMARKS)
  add_subdirectory(benchmark)
endif()

install(TARGETS moqx moqx-issuer)

# Identifies an unpacked tarball without running the binary.
install(FILES ${CMAKE_BINARY_DIR}/VERSION DESTINATION .)
