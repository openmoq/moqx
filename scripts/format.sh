#!/usr/bin/env bash
set -euo pipefail

FILES=$(rg --files -g '*.{h,hpp,cc,cpp,cxx}' include src tests tools)

if [[ -z "${FILES}" ]]; then
  echo "No source files found."
  exit 0
fi

echo "Formatting files..."
clang-format -i ${FILES}
