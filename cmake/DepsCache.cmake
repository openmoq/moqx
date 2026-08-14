# DepsCache.cmake — one root for everything moqx downloads.
#
#   <root>/cpm                    CPM's source clones (CPM_SOURCE_CACHE)
#   <root>/moxygen-<rev>-<plat>   extracted prebuilt installs (FetchMoxygenPrebuilt.cmake)
#
# scripts/configure.sh symlinks the resolved directories into the tree for
# humans — see deps/README.md.
#
# Root precedence: -DMOQX_DEPS_CACHE, $MOQX_DEPS_CACHE, $HOME/.cache/moqx, else a
# directory in the build tree (a container with no HOME). Relocating the root
# moves both halves; an explicit CPM_SOURCE_CACHE still wins for the clones alone.
#
# Included by CMakeLists.txt, superbuild/CMakeLists.txt and
# cmake/FetchMoxygenPrebuilt.cmake. The superbuild and the moqx build have to
# land on the same clones, or a from-source moqx compiles against different
# source than the moxygen prefix it links.

include_guard(GLOBAL)

if(NOT DEFINED MOQX_DEPS_CACHE OR MOQX_DEPS_CACHE STREQUAL "")
  if(NOT "$ENV{MOQX_DEPS_CACHE}" STREQUAL "")
    set(_moqx_deps_cache "$ENV{MOQX_DEPS_CACHE}")
  elseif(NOT "$ENV{HOME}" STREQUAL "")
    set(_moqx_deps_cache "$ENV{HOME}/.cache/moqx")
  else()
    set(_moqx_deps_cache "${CMAKE_BINARY_DIR}/deps-cache")
  endif()
  set(MOQX_DEPS_CACHE "${_moqx_deps_cache}" CACHE PATH
      "Root for moqx's dependency downloads (CPM sources + prebuilt installs)")
endif()

# This must be the CACHE entry: CPM declares its own, which drops a normal
# variable of the same name and switches the cache off on the first configure.
if(NOT DEFINED CACHE{CPM_SOURCE_CACHE} AND "$ENV{CPM_SOURCE_CACHE}" STREQUAL "")
  set(CPM_SOURCE_CACHE "${MOQX_DEPS_CACHE}/cpm" CACHE PATH
      "Directory to download CPM dependencies")
endif()
