#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

SCRATCH_PATH="${ORLY_SCRATCH_PATH:-${PROJECT_ROOT}/.scratch}"
GETDEPS="${PROJECT_ROOT}/deps/moxygen/build/fbcode_builder/getdeps.py"

if [[ ! -f "$GETDEPS" ]]; then
  echo "Error: getdeps.py not found. Did you init submodules?"
  echo "  git submodule update --init"
  exit 1
fi

GETDEPS_COMMON=(
  --scratch-path "$SCRATCH_PATH"
  --extra-cmake-defines '{"CMAKE_FIND_LIBRARY_SUFFIXES": ".a", "BUILD_SHARED_LIBS": "OFF"}'
)

echo "Building moxygen + deps into ${SCRATCH_PATH} ..."
python3 "$GETDEPS" build \
  --no-tests \
  "${GETDEPS_COMMON[@]}" \
  --src-dir="moxygen:${PROJECT_ROOT}/deps/moxygen" \
  moxygen

# Generate CMAKE_PREFIX_PATH with all installed dep dirs
python3 "$GETDEPS" show-inst-dir \
  "${GETDEPS_COMMON[@]}" \
  --recursive moxygen 2>/dev/null \
  | grep "^/" \
  | tr '\n' ';' \
  > "${SCRATCH_PATH}/cmake_prefix_path.txt"

echo ""
echo "Done. Configure o-rly with:"
echo "  cmake -S . -B build --preset default"
