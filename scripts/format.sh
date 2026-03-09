#!/usr/bin/env bash
set -euo pipefail

FILES=$(find include src tests tools -name '*.h' -o -name '*.hpp' -o -name '*.cc' -o -name '*.cpp' -o -name '*.cxx')

if [[ -z "${FILES}" ]]; then
  echo "No source files found."
  exit 0
fi

if [[ "${1:-}" == "--check" ]]; then
  echo "Checking formatting..."
  clang-format --dry-run -Werror ${FILES}
else
  echo "Formatting files..."
  clang-format -i ${FILES}
fi
