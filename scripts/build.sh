#!/usr/bin/env bash
set -euo pipefail

BUILD_DIR=${1:-build}
TARGET=${2:-""}

if [[ -n "${TARGET}" ]]; then
  cmake --build "${BUILD_DIR}" --target "${TARGET}" -j
else
  cmake --build "${BUILD_DIR}" -j
fi
