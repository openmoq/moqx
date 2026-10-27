# PGO and LTO flags shared by the moqx build and the superbuild's moxygen.
# Profiles are keyed by object file path, so both passes reconfigure the same
# build dirs, and they compile with the same flags apart from the profile ones.
#
#   MOQX_PGO      off | generate | use
#   MOQX_PGO_DIR  profile directory (default <repo>/build/pgo-profile)
#   MOQX_LTO      LTO bytecode in every object; moqx links with LTO
#
# Outputs (lists): MOQX_OPT_COMPILE_FLAGS, MOQX_OPT_LINK_FLAGS for moqx's
# executables, MOQX_OPT_DEPS_LINK_FLAGS for moxygen's own executables, and
# MOQX_PGO_LAUNCHER, the compiler launcher for PGO builds.

set(MOQX_PGO "off" CACHE STRING "Profile-guided optimization pass: off | generate | use")
set_property(CACHE MOQX_PGO PROPERTY STRINGS off generate use)
if(NOT MOQX_PGO MATCHES "^(off|generate|use)$")
  message(FATAL_ERROR "MOQX_PGO must be off|generate|use (got '${MOQX_PGO}')")
endif()
get_filename_component(_moqx_pgo_top "${CMAKE_CURRENT_LIST_DIR}/.." ABSOLUTE)
set(MOQX_PGO_DIR "${_moqx_pgo_top}/build/pgo-profile" CACHE PATH
    "Profile directory the generate pass writes and the use pass reads")
option(MOQX_LTO "Link-time optimization across moqx and the moxygen stack" OFF)

set(MOQX_OPT_COMPILE_FLAGS "")
set(MOQX_OPT_LINK_FLAGS "")
set(MOQX_OPT_DEPS_LINK_FLAGS "")
# A ccache hit would ignore the profile, and ccache can come in as a launcher,
# from the environment or as a wrapper on PATH; disabling it covers all three.
set(MOQX_PGO_LAUNCHER "")
if(NOT MOQX_PGO STREQUAL "off")
  set(MOQX_PGO_LAUNCHER "${CMAKE_COMMAND}" -E env CCACHE_DISABLE=1)
endif()
if(MOQX_PGO STREQUAL "generate")
  list(APPEND MOQX_OPT_COMPILE_FLAGS "-fprofile-generate=${MOQX_PGO_DIR}" -fprofile-update=atomic)
  # Pulls in libgcov's __gcov_dump, which src/main.cpp references weakly.
  list(APPEND MOQX_OPT_LINK_FLAGS "-fprofile-generate=${MOQX_PGO_DIR}" -Wl,--undefined=__gcov_dump)
  list(APPEND MOQX_OPT_DEPS_LINK_FLAGS "-fprofile-generate=${MOQX_PGO_DIR}")
elseif(MOQX_PGO STREQUAL "use")
  # Code the training never reached stays plain -O3.
  list(APPEND MOQX_OPT_COMPILE_FLAGS "-fprofile-use=${MOQX_PGO_DIR}"
       -fprofile-partial-training -Wno-missing-profile)
endif()
if(MOQX_LTO)
  # Fat objects keep machine code beside the bytecode, so -fno-lto links still work.
  list(APPEND MOQX_OPT_COMPILE_FLAGS -flto=auto -ffat-lto-objects)
  list(APPEND MOQX_OPT_DEPS_LINK_FLAGS -fno-lto)
  if(MOQX_PGO STREQUAL "generate")
    # Counters are per translation unit; the training binary needs no LTO.
    list(APPEND MOQX_OPT_LINK_FLAGS -fno-lto)
  else()
    list(APPEND MOQX_OPT_LINK_FLAGS -flto=auto)
  endif()
endif()
