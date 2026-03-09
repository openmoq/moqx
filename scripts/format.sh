#!/usr/bin/env bash
set -euo pipefail

REQUIRED_CF_VERSION=19

CF_BIN=${CF_BIN:-$(command -v clang-format-${REQUIRED_CF_VERSION} || command -v clang-format)}
CF_VERSION=$(${CF_BIN} --version | sed -n 's/.*version \([0-9]*\)\..*/\1/p' | head -1)
if [[ -z "${CF_VERSION}" ]]; then
  echo "error: could not determine clang-format version" >&2
  exit 1
fi
if [[ "${CF_VERSION}" -ne "${REQUIRED_CF_VERSION}" ]]; then
  echo "error: clang-format ${REQUIRED_CF_VERSION} required, found ${CF_VERSION}" >&2
  echo "Install clang-format ${REQUIRED_CF_VERSION}:" >&2
  echo "  Debian/Ubuntu:  apt install clang-format-${REQUIRED_CF_VERSION}  (or https://apt.llvm.org)" >&2
  echo "  macOS:          brew install llvm@${REQUIRED_CF_VERSION}" >&2
  echo "  CentOS/RHEL:    yum install clang-tools-extra-${REQUIRED_CF_VERSION}.0  (EPEL/AppStream)" >&2
  exit 1
fi

FILES=$(find include src tests tools -name '*.h' -o -name '*.hpp' -o -name '*.cc' -o -name '*.cpp' -o -name '*.cxx')

if [[ -z "${FILES}" ]]; then
  echo "No source files found."
  exit 0
fi

if [[ "${1:-}" == "--check" ]]; then
  echo "Checking formatting..."
  ${CF_BIN} --dry-run -Werror ${FILES}
else
  echo "Formatting files..."
  ${CF_BIN} -i ${FILES}
fi
