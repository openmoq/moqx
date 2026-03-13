#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

SCRATCH="${ORLY_SCRATCH_PATH:-${PROJECT_ROOT}/.scratch}"
PREFIX_PATH_FILE="${SCRATCH}/cmake_prefix_path.txt"
REPOS="${SCRATCH}/repos"

if [[ ! -f "$PREFIX_PATH_FILE" ]]; then
  echo "Error: cmake_prefix_path.txt not found. Run setup-deps.sh first."
  exit 1
fi

PREFIX_PATH=$(cat "$PREFIX_PATH_FILE")

COMMON_ARGS=(
  -DBUILD_SHARED_LIBS=OFF
  -DCMAKE_BUILD_TYPE=RelWithDebInfo
  -DCMAKE_FIND_LIBRARY_SUFFIXES=".a"
  -DBUILD_TESTS=OFF
  -G Ninja
)

build_dep() {
  local name="$1"
  local src="$2"
  shift 2
  local extra_args=("$@")

  local build_dir="${SCRATCH}/build/${name}"
  local inst_dir="${SCRATCH}/installed/${name}"

  echo "Building ${name} ..."
  cmake -S "$src" -B "$build_dir" \
    -DCMAKE_PREFIX_PATH="$PREFIX_PATH" \
    -DCMAKE_INSTALL_PREFIX="$inst_dir" \
    "${COMMON_ARGS[@]}" \
    "${extra_args[@]}"

  cmake --build "$build_dir"
  cmake --install "$build_dir"
  git -C "$src" rev-parse HEAD > "${inst_dir}/.build-rev"
  echo "${name} done."
}

ONLY="${1:-}"

ALL_DEPS=(folly fizz wangle mvfst proxygen moxygen)
if [[ -n "$ONLY" ]]; then
  ALL_DEPS=("$ONLY")
fi

for dep in "${ALL_DEPS[@]}"; do
  case "$dep" in
    folly)    build_dep folly "${REPOS}/github.com-facebook-folly.git" \
                -DBOOST_LINK_STATIC=ON -DBUILD_BENCHMARKS=OFF ;;
    fizz)     build_dep fizz "${REPOS}/github.com-facebookincubator-fizz.git/fizz" \
                -DBUILD_EXAMPLES=OFF ;;
    wangle)   build_dep wangle "${REPOS}/github.com-facebook-wangle.git/wangle" ;;
    mvfst)    build_dep mvfst "${REPOS}/github.com-facebook-mvfst.git" ;;
    proxygen) build_dep proxygen "${REPOS}/github.com-facebook-proxygen.git" ;;
    moxygen)  build_dep moxygen "${PROJECT_ROOT}/deps/moxygen" ;;
    *)        echo "Unknown dep: $dep"; exit 1 ;;
  esac
done
