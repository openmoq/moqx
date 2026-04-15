#!/usr/bin/env bash
set -euo pipefail

BUILD_DIR=${1:-build}
ctest --test-dir "${BUILD_DIR}" --output-on-failure --parallel
