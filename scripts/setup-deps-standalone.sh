#!/usr/bin/env bash
# setup-deps-standalone.sh — Build moxygen + deps from source (standalone/FetchContent).
#
# Uses deps/moxygen/standalone/CMakeLists.txt which fetches Meta OSS deps
# (folly, fizz, wangle, mvfst, proxygen) via FetchContent and builds them
# as static libraries alongside moxygen. Installs everything to .scratch/
# moxygen-install and writes cmake_prefix_path.txt for configure.sh.
#
# This is the "deep" build — slower first time but fully self-contained.
# Subsequent builds are incremental (cmake only rebuilds what changed).
#
# Usage:
#   ./scripts/setup-deps-standalone.sh
#
# System deps required (Ubuntu):
#   deps/moxygen/standalone/install-system-deps.sh

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
SCRATCH="${MOQX_SCRATCH_PATH:-${PROJECT_ROOT}/.scratch}"
MOXYGEN_DIR="${MOQX_MOXYGEN_DIR:-${PROJECT_ROOT}/deps/moxygen}"
STANDALONE_SRC="${MOXYGEN_DIR}/standalone"

PROFILE="default"
while (( $# > 0 )); do
  case "$1" in
    --profile) PROFILE="$2"; shift 2 ;;
    *) echo "Unknown option: $1" >&2; exit 1 ;;
  esac
done

if [[ "$PROFILE" == "default" ]]; then
  BUILD_DIR="${SCRATCH}/standalone-build"
  INSTALL_DIR="${SCRATCH}/moxygen-install"
  PREFIX_PATH_FILE="${SCRATCH}/cmake_prefix_path.txt"
else
  BUILD_DIR="${SCRATCH}/standalone-build-${PROFILE}"
  INSTALL_DIR="${SCRATCH}/moxygen-install-${PROFILE}"
  PREFIX_PATH_FILE="${SCRATCH}/cmake_prefix_path-${PROFILE}.txt"
fi

if [[ -z "${MOQX_MOXYGEN_DIR:-}" ]] && [[ ! -e "$MOXYGEN_DIR/.git" ]]; then
    echo "Error: deps/moxygen submodule not initialized." >&2
    echo "  Run: git submodule update --init" >&2
    exit 1
fi

if [[ ! -f "${STANDALONE_SRC}/CMakeLists.txt" ]]; then
    echo "Error: standalone/CMakeLists.txt not found in ${MOXYGEN_DIR}" >&2
    exit 1
fi

NPROC=$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)

# Profile-specific cmake flags
CMAKE_BUILD_TYPE="RelWithDebInfo"
EXTRA_CMAKE_ARGS=()
if [[ "$PROFILE" == "san" ]]; then
  CMAKE_BUILD_TYPE="Debug"
  # ASAN only (no UBSAN): folly uses static_assert on syscall function addresses
  # (recvmmsg, sendmmsg) which become non-constant under UBSAN's function
  # interposition. ASAN alone is sufficient for memory safety in deps.
  SAN_FLAGS="-fsanitize=address -fno-omit-frame-pointer"
  EXTRA_CMAKE_ARGS+=(
    "-DCMAKE_C_FLAGS=${SAN_FLAGS}"
    "-DCMAKE_CXX_FLAGS=${SAN_FLAGS}"
    "-DCMAKE_EXE_LINKER_FLAGS=-fsanitize=address"
    "-DCMAKE_SHARED_LINKER_FLAGS=-fsanitize=address"
  )
fi

echo "==> Configuring standalone moxygen build (profile: ${PROFILE})..."
cmake -S "$STANDALONE_SRC" -B "$BUILD_DIR" \
    -G Ninja \
    -DCMAKE_BUILD_TYPE="$CMAKE_BUILD_TYPE" \
    -DCMAKE_INSTALL_PREFIX="$INSTALL_DIR" \
    -DINSTALL_DEPS=ON \
    -DBUILD_TESTS=OFF \
    -DBUILD_SHARED_LIBS=OFF \
    "${EXTRA_CMAKE_ARGS[@]+"${EXTRA_CMAKE_ARGS[@]}"}"

echo "==> Building ($NPROC jobs)..."
cmake --build "$BUILD_DIR" -j"$NPROC"

echo "==> Installing to $INSTALL_DIR..."
rm -rf "$INSTALL_DIR"
cmake --install "$BUILD_DIR"

# ── Write cmake_prefix_path.txt ───────────────────────────────────────────────

mkdir -p "$SCRATCH"
echo "$INSTALL_DIR" > "$PREFIX_PATH_FILE"

echo "from-source" > "${SCRATCH}/deps-mode"

NLIBS=$(find "$INSTALL_DIR/lib" -name '*.a' 2>/dev/null | wc -l)
echo "==> Done: $NLIBS static libs in $INSTALL_DIR"
