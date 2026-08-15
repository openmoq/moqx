# CheckSystemDeps.cmake — name the missing system -dev packages up front, instead
# of leaving them to surface as "Could NOT find ..." deep inside folly's config.
#
# Linux only; macOS/brew and non-standard prefixes are left to find_package.
# Skip with -DMOQX_SKIP_SYSTEM_DEP_CHECK=ON.

# The Boost components folly's config find_package()s. Set before the early
# return: superbuild/CMakeLists.txt reads this list even when the check is
# skipped.
set(MOQX_BOOST_COMPONENTS context filesystem program_options regex thread)

if(MOQX_SKIP_SYSTEM_DEP_CHECK OR NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
  return()
endif()

# "<header>|<debian pkg>|<fedora pkg>" — needed in both dependency modes, since
# folly resolves these from the system even under a prebuilt moxygen. A new
# library goes here and in scripts/install-system-deps.sh.
set(_moqx_reqs
  "openssl/ssl.h|libssl-dev|openssl-devel"
  "gflags/gflags.h|libgflags-dev|gflags-devel"
  "glog/logging.h|libgoogle-glog-dev|glog-devel"
  "double-conversion/double-conversion.h|libdouble-conversion-dev|double-conversion-devel"
  "event2/event.h|libevent-dev|libevent-devel"
  "sodium.h|libsodium-dev|libsodium-devel"
  "zstd.h|libzstd-dev|libzstd-devel"
  "boost/version.hpp|libboost-dev|boost-devel"
  # folly's config find_dependency(ZLIB)s and proxygen's find_dependency(c-ares)s;
  # both resolve from the system, the latter via cmake/Findc-ares.cmake. fmt does
  # not belong here — moxygen builds and installs it into the prefix.
  "zlib.h|zlib1g-dev|zlib-devel"
  "ares.h|libc-ares-dev|c-ares-devel"
)

set(_moqx_missing "")
foreach(_req IN LISTS _moqx_reqs)
  string(REPLACE "|" ";" _parts "${_req}")
  list(GET _parts 0 _hdr)
  list(GET _parts 1 _apt)
  list(GET _parts 2 _dnf)
  string(MAKE_C_IDENTIFIER "sysdep_${_hdr}" _key)
  find_path(${_key} NAMES "${_hdr}")
  if(NOT ${_key})
    list(APPEND _moqx_missing "  ${_hdr}  (Debian: ${_apt} / Fedora: ${_dnf})")
  endif()
  # Don't cache the result: a stale NOTFOUND would survive installing the dep.
  unset(${_key} CACHE)
endforeach()

# Debian's libboost-dev ships headers only, and folly find_package()s each
# component separately. The probe goes through the compiler driver because this
# file also runs in the language-less superbuild, where find_library is blind.
find_program(_moqx_probe_cxx NAMES $ENV{CXX} c++ g++ clang++)
if(_moqx_probe_cxx)
  foreach(_comp IN LISTS MOQX_BOOST_COMPONENTS)
    set(_found FALSE)
    foreach(_ext so a)
      execute_process(COMMAND "${_moqx_probe_cxx}" -print-file-name=libboost_${_comp}.${_ext}
        OUTPUT_VARIABLE _loc OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
      if(_loc MATCHES "^/")
        set(_found TRUE)
        break()
      endif()
    endforeach()
    if(NOT _found)
      string(REPLACE "_" "-" _pkg "${_comp}")
      list(APPEND _moqx_missing
        "  libboost_${_comp}  (Debian: libboost-${_pkg}-dev / Fedora: boost-devel)")
    endif()
  endforeach()
endif()
unset(_moqx_probe_cxx CACHE)

if(_moqx_missing)
  string(REPLACE ";" "\n" _moqx_missing_str "${_moqx_missing}")
  message(FATAL_ERROR
    "Missing system dependencies — these dev packages were not found:\n"
    "${_moqx_missing_str}\n\n"
    "Install them with:  scripts/install-system-deps.sh\n"
    "(or bypass this check with -DMOQX_SKIP_SYSTEM_DEP_CHECK=ON).")
endif()
