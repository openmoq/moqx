#!/usr/bin/env bash
set -euo pipefail

BUILD_DIR=${1:-build}

if [[ ! -f "${BUILD_DIR}/compile_commands.json" ]]; then
  echo "compile_commands.json not found in ${BUILD_DIR}. Run configure first." >&2
  exit 1
fi

FILES=$(rg --files -g '*.{h,hpp,cc,cpp,cxx}' include src tests tools)

if [[ -z "${FILES}" ]]; then
  echo "No source files found."
  exit 0
fi

echo "Running clang-tidy..."
clang-tidy -p "${BUILD_DIR}" ${FILES}
