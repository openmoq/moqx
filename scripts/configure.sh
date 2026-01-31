#!/usr/bin/env bash
set -euo pipefail

BUILD_DIR=${1:-build}
PRESET=${2:-""}

if [[ -n "${PRESET}" ]]; then
  cmake -S . -B "${BUILD_DIR}" --preset "${PRESET}"
else
  cmake -S . -B "${BUILD_DIR}"
fi
