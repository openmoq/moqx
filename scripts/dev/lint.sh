#!/usr/bin/env bash
set -euo pipefail

BUILD_DIR=${1:-build/default}

run-clang-tidy -p "${BUILD_DIR}"
