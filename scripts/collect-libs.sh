#!/usr/bin/env bash
# collect-libs.sh — Extract all shared library dependencies from a binary
# and optionally copy them to a destination directory.
#
# Usage: scripts/collect-libs.sh <binary_path> [dest_dir]
#   binary_path  Path to the binary to analyze
#   dest_dir     (Optional) Directory to copy libs to

set -euo pipefail

BINARY="${1:?Binary path required}"
DEST_DIR="${2:-}"

if [[ ! -f "$BINARY" ]]; then
  echo "ERROR: Binary not found: $BINARY" >&2
  exit 1
fi

# Get all dependencies using ldd
DEPS=$(ldd "$BINARY" 2>/dev/null | grep -o '/[^ ]*.so[^ ]*' | sort -u || true)

if [[ -z "$DEST_DIR" ]]; then
  # Just print the list
  echo "$DEPS"
else
  # Copy libs to destination
  mkdir -p "$DEST_DIR"
  
  echo "Copying shared libraries to $DEST_DIR..."
  for lib in $DEPS; do
    if [[ -f "$lib" ]]; then
      cp -v "$lib" "$DEST_DIR/" || true
    fi
  done
  
  echo "Done. Libraries in $DEST_DIR:"
  ls -lh "$DEST_DIR"/*.so* 2>/dev/null | awk '{print $9, "(" $5 ")"}'
fi
