#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

SCRATCH_PATH="${ORLY_SCRATCH_PATH:-${PROJECT_ROOT}/.scratch}"
PREFIX_PATH_FILE="${SCRATCH_PATH}/cmake_prefix_path.txt"

BUILD_DIR=${1:-build}
PRESET=${2:-default}

PREFIX_ARG=()
if [[ -f "$PREFIX_PATH_FILE" ]]; then
  PREFIX_ARG=("-DCMAKE_PREFIX_PATH=$(cat "$PREFIX_PATH_FILE")")
fi

cmake -S "$PROJECT_ROOT" -B "${BUILD_DIR}" --preset "${PRESET}" "${PREFIX_ARG[@]}"
