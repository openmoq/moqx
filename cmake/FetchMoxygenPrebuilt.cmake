# FetchMoxygenPrebuilt.cmake — resolve MOXYGEN_REV to a published moxygen release,
# download its prebuilt install for this platform, and append that to
# CMAKE_PREFIX_PATH for the find_package(moxygen CONFIG) that follows.
#
# The prebuilt is a pure function of MOXYGEN_REV:
#   1. moqx_moxygen_release_tag() (MoxygenRelease.cmake) maps the rev to a tag.
#   2. One GitHub API read of that release returns both the commit it was built
#      from and every asset's digest, so a tag that repoints mid-fetch cannot
#      deliver wrong-rev binaries. The digest is a consistency check, not tamper
#      resistance — it shares its origin with the asset.
#   3. The download uses the browser_download_url from that same response.
#
# No tag at the rev, no release on the tag, or no asset for this platform is a
# hard error pointing at the superbuild. See BUILD.md.
#
# Included by CMakeLists.txt, after cmake/dependencies.cmake.
#
# Knobs:
#   MOQX_PLATFORM          override the auto-detected platform tag
#   MOXYGEN_RELEASE_TAG    release tag to fetch from (default: resolve it from
#                          MOXYGEN_REV); either way the release's commit is
#                          verified against the pin
#   MOQX_DEPS_CACHE        cache root, variable or env (default ~/.cache/moqx)
#   GITHUB_TOKEN/GH_TOKEN  env only — authenticate the release read

include("${CMAKE_CURRENT_LIST_DIR}/MoxygenRelease.cmake")

# --- platform tag: the <platform> in moxygen-<platform>.tar.gz ---------------
function(_moqx_detect_platform OUT_VAR)
  if(DEFINED MOQX_PLATFORM AND NOT MOQX_PLATFORM STREQUAL "")
    set(${OUT_VAR} "${MOQX_PLATFORM}" PARENT_SCOPE)
    return()
  endif()
  if(DEFINED ENV{MOQX_PLATFORM} AND NOT "$ENV{MOQX_PLATFORM}" STREQUAL "")
    set(${OUT_VAR} "$ENV{MOQX_PLATFORM}" PARENT_SCOPE)
    return()
  endif()

  set(_arch "${CMAKE_HOST_SYSTEM_PROCESSOR}")
  if(_arch STREQUAL "x86_64" OR _arch STREQUAL "AMD64")
    set(_arch "amd64")
  elseif(_arch STREQUAL "aarch64" OR _arch STREQUAL "arm64")
    set(_arch "arm64")
  endif()

  if(APPLE)
    execute_process(COMMAND sw_vers -productVersion
      OUTPUT_VARIABLE _ver OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
    string(REGEX MATCH "^[0-9]+" _major "${_ver}")
    if(_major STREQUAL "")
      set(${OUT_VAR} "" PARENT_SCOPE)  # sw_vers failed -> clean "unsupported" error
    else()
      # Real arch, not hardcoded arm64: an Intel Mac must get the clean
      # "no prebuilt" error, not a successful arm64 download that fails at link.
      set(${OUT_VAR} "macos-${_major}-${_arch}" PARENT_SCOPE)
    endif()
    return()
  endif()

  if(NOT EXISTS "/etc/os-release")
    set(${OUT_VAR} "" PARENT_SCOPE)
    return()
  endif()
  # Read the few fields we need out of /etc/os-release.
  foreach(_field ID VERSION_ID ID_LIKE UBUNTU_CODENAME)
    file(STRINGS "/etc/os-release" _line REGEX "^${_field}=")
    string(REGEX REPLACE "^${_field}=" "" _line "${_line}")
    string(REGEX REPLACE "\"" "" _line "${_line}")
    set(_osr_${_field} "${_line}")
  endforeach()

  if(_osr_ID STREQUAL "ubuntu")
    set(${OUT_VAR} "ubuntu-${_osr_VERSION_ID}-${_arch}" PARENT_SCOPE)
  elseif(_osr_ID STREQUAL "debian")
    set(${OUT_VAR} "bookworm-${_arch}" PARENT_SCOPE)
  elseif(_osr_ID_LIKE MATCHES "ubuntu")
    if(_osr_UBUNTU_CODENAME STREQUAL "jammy")
      set(${OUT_VAR} "ubuntu-22.04-${_arch}" PARENT_SCOPE)
    elseif(_osr_UBUNTU_CODENAME STREQUAL "noble")
      set(${OUT_VAR} "ubuntu-24.04-${_arch}" PARENT_SCOPE)
    elseif(EXISTS "/etc/upstream-release/lsb-release")
      # Derivative with a codename we don't map (or none): Mint/Pop/... record
      # their Ubuntu base release here.
      file(STRINGS "/etc/upstream-release/lsb-release" _rel REGEX "^DISTRIB_RELEASE=")
      string(REGEX REPLACE "^DISTRIB_RELEASE=" "" _rel "${_rel}")
      string(REGEX REPLACE "\"" "" _rel "${_rel}")
      if(_rel MATCHES "^[0-9.]+$")
        set(${OUT_VAR} "ubuntu-${_rel}-${_arch}" PARENT_SCOPE)
      else()
        set(${OUT_VAR} "" PARENT_SCOPE)
      endif()
    else()
      set(${OUT_VAR} "" PARENT_SCOPE)
    endif()
  elseif(_osr_ID_LIKE MATCHES "debian")
    set(${OUT_VAR} "bookworm-${_arch}" PARENT_SCOPE)
  else()
    set(${OUT_VAR} "" PARENT_SCOPE)
  endif()
endfunction()

_moqx_detect_platform(_moqx_platform)

# --- cache location (per rev + platform) -------------------------------------
# Deliberately beside CPM_SOURCE_CACHE rather than inside it: these extracted
# install prefixes are not CPM sources, and clearing CPM's cache must not take
# them along.
if(DEFINED MOQX_DEPS_CACHE AND NOT MOQX_DEPS_CACHE STREQUAL "")
  set(_cache_root "${MOQX_DEPS_CACHE}")
elseif(DEFINED ENV{MOQX_DEPS_CACHE} AND NOT "$ENV{MOQX_DEPS_CACHE}" STREQUAL "")
  set(_cache_root "$ENV{MOQX_DEPS_CACHE}")
elseif(DEFINED ENV{HOME} AND NOT "$ENV{HOME}" STREQUAL "")
  set(_cache_root "$ENV{HOME}/.cache/moqx")
else()
  # No HOME (bare container): keep the cache inside the build tree.
  set(_cache_root "${CMAKE_BINARY_DIR}/moxygen-prebuilt")
endif()
string(SUBSTRING "${MOXYGEN_REV}" 0 12 _rev_short)
set(_install_dir "${_cache_root}/moxygen-${_rev_short}-${_moqx_platform}")

# Empty means resolve the tag from MOXYGEN_REV below; release branches pin it for
# a stable source.
set(MOXYGEN_RELEASE_TAG ""
    CACHE STRING "moxygen release tag to fetch the prebuilt from (empty = auto-resolve from MOXYGEN_REV)")

# Two cold-cache configures sharing this root would corrupt each other's install,
# since the reuse-check and the download/extract below write the same paths.
# GUARD PROCESS auto-releases when a FATAL_ERROR ends the process.
file(MAKE_DIRECTORY "${_cache_root}")
file(LOCK "${_cache_root}/.moxygen-fetch.lock" GUARD PROCESS TIMEOUT 600
     RESULT_VARIABLE _fetch_lock)
if(_fetch_lock AND NOT _fetch_lock STREQUAL "0")
  message(FATAL_ERROR
    "moxygen: could not acquire the prebuilt cache lock at ${_cache_root} (${_fetch_lock})")
endif()

# --- reuse if already present, else resolve + download -----------------------
# The .moqx-pin-rev marker records the rev the tree was installed for, and reuse
# needs a match, so a pin bump can never be served a stale entry. A hit needs no
# network.
set(_pin_marker "${_install_dir}/.moqx-pin-rev")
set(_cached_pin "")
if(EXISTS "${_pin_marker}")
  file(READ "${_pin_marker}" _cached_pin)
  string(STRIP "${_cached_pin}" _cached_pin)
endif()
if(EXISTS "${_install_dir}/lib/cmake/moxygen/moxygen-config.cmake"
   AND _cached_pin STREQUAL "${MOXYGEN_REV}")
  message(STATUS "moxygen: reusing cached prebuilt at ${_install_dir}")
else()
  if(_moqx_platform STREQUAL "")
    message(FATAL_ERROR
      "moxygen: no prebuilt platform tag for this host. Build from source:\n"
      "  scripts/configure.sh --mode from-source && scripts/build.sh\n"
      "(force a published platform with -DMOQX_PLATFORM=<tag>; see BUILD.md).")
  endif()

  # --- resolve MOXYGEN_REV -> release tag ------------------------------------
  # An explicit MOXYGEN_RELEASE_TAG wins; the release read below verifies the
  # commit against the pin either way, so a wrong tag fails before any download.
  if(NOT MOXYGEN_RELEASE_TAG STREQUAL "")
    set(_rel_tag "${MOXYGEN_RELEASE_TAG}")
  else()
    moqx_moxygen_release_tag(_rel_tag)
  endif()

  # --- read the release: its commit and its asset digests, in one response ---
  set(_api_hdrs
      HTTPHEADER "Accept: application/vnd.github+json"
      HTTPHEADER "X-GitHub-Api-Version: 2022-11-28")
  # A token buys nothing but rate limit here — the repo is public. Anonymous is
  # 60 requests/hour/IP, which shared CI egress IPs can exhaust on their own.
  set(_api_token "$ENV{GITHUB_TOKEN}")
  if(_api_token STREQUAL "")
    set(_api_token "$ENV{GH_TOKEN}")
  endif()
  if(NOT _api_token STREQUAL "")
    list(APPEND _api_hdrs HTTPHEADER "Authorization: Bearer ${_api_token}")
  endif()

  # One fixed filename, not one per tag: the whole fetch runs under the cache
  # lock, and a tag containing '/' must not turn into a path.
  file(MAKE_DIRECTORY "${_cache_root}/downloads")
  set(_rel_json_file "${_cache_root}/downloads/release.json")
  file(DOWNLOAD
       "https://api.github.com/repos/${MOXYGEN_REPOSITORY}/releases/tags/${_rel_tag}"
       "${_rel_json_file}"
       STATUS _api_status LOG _api_log TLS_VERIFY ON INACTIVITY_TIMEOUT 30
       ${_api_hdrs})
  list(GET _api_status 0 _api_rc)
  if(NOT _api_rc EQUAL 0)
    file(REMOVE "${_rel_json_file}")
    list(GET _api_status 1 _api_msg)
    # The status message only says "HTTP response code said error"; the code
    # itself is in the response headers, which LOG captured.
    string(REGEX MATCHALL "HTTP/[0-9.]+ [0-9]+" _api_codes "${_api_log}")
    set(_http_code "")
    if(_api_codes)
      list(GET _api_codes -1 _http_code)
      string(REGEX REPLACE "^.* " "" _http_code "${_http_code}")
    endif()
    if(_http_code STREQUAL "404")
      message(FATAL_ERROR
        "moxygen: tag '${_rel_tag}' has no GitHub release, so it publishes no "
        "prebuilt. Build from source:\n"
        "  scripts/configure.sh --mode from-source && scripts/build.sh")
    elseif(_http_code STREQUAL "403" OR _http_code STREQUAL "429")
      message(FATAL_ERROR
        "moxygen: the GitHub API rate limit is exhausted for this IP (anonymous "
        "reads get 60/hour). Export GITHUB_TOKEN (or GH_TOKEN) to authenticate the "
        "read, wait for the limit to reset, or build from source:\n"
        "  scripts/configure.sh --mode from-source && scripts/build.sh")
    elseif(_http_code STREQUAL "401")
      message(FATAL_ERROR
        "moxygen: the GitHub API rejected the token in GITHUB_TOKEN/GH_TOKEN. The "
        "release is public — unset it to read anonymously.")
    else()
      if(NOT _http_code STREQUAL "")
        set(_api_msg "HTTP ${_http_code}")
      endif()
      message(FATAL_ERROR
        "moxygen: could not read release '${_rel_tag}' from the GitHub API "
        "(${_api_msg}).")
    endif()
  endif()
  file(READ "${_rel_json_file}" _rel_json)
  file(REMOVE "${_rel_json_file}")

  string(JSON _rel_commit ERROR_VARIABLE _json_err GET "${_rel_json}" target_commitish)
  if(NOT _json_err STREQUAL "NOTFOUND")
    message(FATAL_ERROR
      "moxygen: unexpected release JSON for '${_rel_tag}' (${_json_err}).")
  endif()
  string(TOLOWER "${_rel_commit}" _rel_commit)
  string(TOLOWER "${MOXYGEN_REV}" _pin_lc)
  if(NOT _rel_commit STREQUAL _pin_lc)
    message(FATAL_ERROR
      "moxygen: release '${_rel_tag}' was built from\n"
      "  ${_rel_commit}\n"
      "not the pinned\n"
      "  ${MOXYGEN_REV}\n"
      "(the tag moved, or MOXYGEN_RELEASE_TAG disagrees with the pin). Fix the pin\n"
      "in cmake/dependencies.cmake, or build from source "
      "(scripts/configure.sh --mode from-source).")
  endif()

  # --- pick this platform's asset, then download + verify it -----------------
  set(_want "moxygen-${_moqx_platform}.tar.gz")
  set(_asset_url "")
  set(_asset_digest "")
  set(_published "")
  string(JSON _asset_count ERROR_VARIABLE _json_err LENGTH "${_rel_json}" assets)
  if(_asset_count GREATER 0)
    math(EXPR _last_asset "${_asset_count} - 1")
    foreach(_i RANGE ${_last_asset})
      string(JSON _name GET "${_rel_json}" assets ${_i} name)
      if(_name MATCHES "^moxygen-(.+)\\.tar\\.gz$")
        list(APPEND _published "${CMAKE_MATCH_1}")
      endif()
      if(_name STREQUAL "${_want}")
        string(JSON _asset_url GET "${_rel_json}" assets ${_i} browser_download_url)
        # digest is null until GitHub finishes computing it, and older assets
        # predate the field entirely.
        string(JSON _digest_type ERROR_VARIABLE _dg_err TYPE "${_rel_json}" assets ${_i} digest)
        if(_dg_err STREQUAL "NOTFOUND" AND _digest_type STREQUAL "STRING")
          string(JSON _asset_digest GET "${_rel_json}" assets ${_i} digest)
        endif()
      endif()
    endforeach()
  endif()

  if(_asset_url STREQUAL "")
    list(SORT _published)
    list(JOIN _published ", " _published_str)
    if(_published_str STREQUAL "")
      set(_published_str "nothing")
    endif()
    message(FATAL_ERROR
      "moxygen: release '${_rel_tag}' publishes no '${_want}'.\n"
      "It publishes: ${_published_str}\n"
      "Build from source:\n"
      "  scripts/configure.sh --mode from-source && scripts/build.sh\n"
      "(or force a published platform with -DMOQX_PLATFORM=<tag>; see BUILD.md).")
  endif()

  if(_asset_digest STREQUAL "")
    message(WARNING
      "moxygen: release '${_rel_tag}' publishes no digest for '${_want}' — "
      "downloading it unverified. GitHub computes digests asynchronously, so a "
      "just-published asset can be missing one for a few minutes.")
  elseif(NOT _asset_digest MATCHES "^sha256:")
    message(WARNING
      "moxygen: '${_want}' advertises an unsupported digest (${_asset_digest}) — "
      "downloading it unverified.")
    set(_asset_digest "")
  endif()

  set(_dl "${_cache_root}/downloads/${_want}")
  message(STATUS "moxygen: downloading prebuilt ${_asset_url}")
  # INACTIVITY_TIMEOUT, not TIMEOUT: the tarball is hundreds of MB and a wall
  # clock cap would punish slow links. A stall must still fail, because this runs
  # under the cache lock and would block every other configure sharing the root.
  file(DOWNLOAD "${_asset_url}" "${_dl}" STATUS _dl_status SHOW_PROGRESS TLS_VERIFY ON
       INACTIVITY_TIMEOUT 60)
  list(GET _dl_status 0 _dl_rc)
  if(NOT _dl_rc EQUAL 0)
    list(GET _dl_status 1 _dl_msg)
    file(REMOVE "${_dl}")
    message(FATAL_ERROR
      "moxygen: could not download '${_want}' from release '${_rel_tag}' (${_dl_msg}).\n"
      "Re-run the configure, or build from source:\n"
      "  scripts/configure.sh --mode from-source && scripts/build.sh")
  endif()

  # The digest came from the same read as the commit the pin was checked against,
  # so a match proves these bytes belong to that one snapshot of the release.
  if(NOT _asset_digest STREQUAL "")
    string(REGEX REPLACE "^sha256:" "" _want_sha "${_asset_digest}")
    string(TOLOWER "${_want_sha}" _want_sha)
    file(SHA256 "${_dl}" _got_sha)
    string(TOLOWER "${_got_sha}" _got_sha)
    if(NOT _got_sha STREQUAL _want_sha)
      file(REMOVE "${_dl}")
      message(FATAL_ERROR
        "moxygen: '${_want}' does not match the digest release '${_rel_tag}' advertises\n"
        "  got      ${_got_sha}\n"
        "  expected ${_want_sha}\n"
        "The release's assets changed while it downloaded. Re-run the configure.")
    endif()
  endif()

  # Extract atomically: into a temp dir, then rename into place.
  set(_tmp "${_install_dir}.tmp")
  file(REMOVE_RECURSE "${_tmp}")
  file(MAKE_DIRECTORY "${_tmp}")
  file(ARCHIVE_EXTRACT INPUT "${_dl}" DESTINATION "${_tmp}")

  # The platform mapping is name-based (every Debian maps to bookworm), so run
  # one shipped binary to catch a host that cannot load it. rc 127 or a
  # non-numeric rc means it never ran; any other code proves it loaded.
  if(EXISTS "${_tmp}/bin/moqtest_client")
    execute_process(COMMAND "${_tmp}/bin/moqtest_client" --help
      RESULT_VARIABLE _probe_rc OUTPUT_QUIET ERROR_QUIET)
    if(_probe_rc STREQUAL "127" OR NOT _probe_rc MATCHES "^[0-9]+$")
      file(REMOVE_RECURSE "${_tmp}")
      message(FATAL_ERROR
        "moxygen: the '${_moqx_platform}' prebuilt does not run on this host "
        "(bin/moqtest_client failed to load: ${_probe_rc}) — likely a system-library "
        "mismatch with the platform it was built for. Build from source instead:\n"
        "  scripts/configure.sh --mode from-source && scripts/build.sh")
    endif()
  endif()

  file(WRITE "${_tmp}/.moqx-pin-rev" "${MOXYGEN_REV}\n")
  file(REMOVE_RECURSE "${_install_dir}")
  file(RENAME "${_tmp}" "${_install_dir}")
  message(STATUS
    "moxygen: installed prebuilt '${_rel_tag}' (rev ${_rev_short}...) to ${_install_dir}")
endif()

file(LOCK "${_cache_root}/.moxygen-fetch.lock" RELEASE)

list(APPEND CMAKE_PREFIX_PATH "${_install_dir}")
# Persist that this build dir's moxygen is the (NDEBUG) prebuilt: the ABI-skew
# guard in CMakeLists.txt keys on it.
set(MOQX_MOXYGEN_IS_PREBUILT TRUE CACHE INTERNAL
    "this build dir resolved moxygen to the release-flavored prebuilt")
