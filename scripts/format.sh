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

FILES=$(find src test tools -name '*.h' -o -name '*.hpp' -o -name '*.cc' -o -name '*.cpp' -o -name '*.cxx')

if [[ -z "${FILES}" ]]; then
  echo "No source files found."
  exit 0
fi

APACHE_HEADER='/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */'

if [[ "${1:-}" == "--check" ]]; then
  echo "Checking copyright headers..."
  header_errors=0
  for f in ${FILES}; do
    if ! head -1 "$f" | grep -q '^/\*'; then
      echo "  missing copyright header: $f"
      header_errors=1
    fi
  done

  echo "Checking formatting..."
  cf_exit=0
  ${CF_BIN} --dry-run -Werror ${FILES} || cf_exit=$?

  if [[ $header_errors -ne 0 ]]; then
    echo "error: files missing copyright headers (run scripts/format.sh to fix)" >&2
  fi
  if [[ $header_errors -ne 0 || $cf_exit -ne 0 ]]; then
    exit 1
  fi
else
  echo "Adding missing copyright headers..."
  for f in ${FILES}; do
    if ! head -1 "$f" | grep -q '^/\*'; then
      printf '%s\n\n' "${APACHE_HEADER}" | cat - "$f" > /tmp/hdr_tmp && mv /tmp/hdr_tmp "$f"
    fi
  done

  echo "Formatting files..."
  ${CF_BIN} -i ${FILES}
fi
