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
SCRATCH="${ORLY_SCRATCH_PATH:-${PROJECT_ROOT}/.scratch}"
MOXYGEN_DIR="${ORLY_MOXYGEN_DIR:-${PROJECT_ROOT}/deps/moxygen}"
STANDALONE_SRC="${MOXYGEN_DIR}/standalone"
BUILD_DIR="${SCRATCH}/standalone-build"
INSTALL_DIR="${SCRATCH}/moxygen-install"

if [[ -z "${ORLY_MOXYGEN_DIR:-}" ]] && [[ ! -e "$MOXYGEN_DIR/.git" ]]; then
    echo "Error: deps/moxygen submodule not initialized." >&2
    echo "  Run: git submodule update --init" >&2
    exit 1
fi

if [[ ! -f "${STANDALONE_SRC}/CMakeLists.txt" ]]; then
    echo "Error: standalone/CMakeLists.txt not found in ${MOXYGEN_DIR}" >&2
    exit 1
fi

NPROC=$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)

echo "==> Configuring standalone moxygen build..."
cmake -S "$STANDALONE_SRC" -B "$BUILD_DIR" \
    -G Ninja \
    -DCMAKE_BUILD_TYPE=RelWithDebInfo \
    -DCMAKE_INSTALL_PREFIX="$INSTALL_DIR" \
    -DBUILD_TESTS=OFF \
    -DBUILD_SHARED_LIBS=OFF

echo "==> Building ($NPROC jobs)..."
cmake --build "$BUILD_DIR" -j"$NPROC"

echo "==> Installing to $INSTALL_DIR..."
rm -rf "$INSTALL_DIR"
cmake --install "$BUILD_DIR"

# ── Write cmake_prefix_path.txt ───────────────────────────────────────────────

mkdir -p "$SCRATCH"
echo "$INSTALL_DIR" > "${SCRATCH}/cmake_prefix_path.txt"
echo "standalone" > "${SCRATCH}/deps-mode"

NLIBS=$(find "$INSTALL_DIR/lib" -name '*.a' 2>/dev/null | wc -l)
echo "==> Done: $NLIBS static libs in $INSTALL_DIR"
