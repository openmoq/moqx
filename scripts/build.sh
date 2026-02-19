#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
SCRATCH="${ORLY_SCRATCH_PATH:-${PROJECT_ROOT}/.scratch}"

HASHES_DIR="${PROJECT_ROOT}/deps/moxygen/build/deps/github_hashes"
META_HASH_FILE="${SCRATCH}/meta-deps.hash"
MOXYGEN_REV_FILE="${SCRATCH}/moxygen.rev"
PREFIX_PATH_FILE="${SCRATCH}/cmake_prefix_path.txt"

# Map from rev file org/name to repo dir name
declare -A REPO_DIRS=(
  [facebook/folly]=github.com-facebook-folly.git
  [facebook/wangle]=github.com-facebook-wangle.git
  [facebook/mvfst]=github.com-facebook-mvfst.git
  [facebook/proxygen]=github.com-facebook-proxygen.git
  [facebookincubator/fizz]=github.com-facebookincubator-fizz.git
)

needs_setup() {
  [[ ! -f "$PREFIX_PATH_FILE" ]]
}

meta_deps_hash() {
  cat "$HASHES_DIR"/*/*-rev.txt 2>/dev/null | sha256sum | awk '{print $1}'
}

meta_deps_changed() {
  local current
  current=$(meta_deps_hash)
  if [[ ! -f "$META_HASH_FILE" ]] || [[ "$current" != "$(cat "$META_HASH_FILE")" ]]; then
    return 0
  fi
  return 1
}

moxygen_rev() {
  git -C "${PROJECT_ROOT}/deps/moxygen" rev-parse HEAD
}

moxygen_changed() {
  local current
  current=$(moxygen_rev)
  if [[ ! -f "$MOXYGEN_REV_FILE" ]] || [[ "$current" != "$(cat "$MOXYGEN_REV_FILE")" ]]; then
    return 0
  fi
  return 1
}

update_meta_repos() {
  echo "Updating meta dep sources to pinned revisions..."
  for key in "${!REPO_DIRS[@]}"; do
    local repo_dir="${SCRATCH}/repos/${REPO_DIRS[$key]}"
    local rev_file="${HASHES_DIR}/${key}-rev.txt"
    local name
    name=$(basename "$key")
    local pinned
    pinned=$(awk '{print $3}' "$rev_file")

    if [[ ! -d "$repo_dir" ]]; then
      echo "  ${name}: repo not found, skipping (run setup-deps.sh first)"
      continue
    fi

    local current
    current=$(git -C "$repo_dir" rev-parse HEAD 2>/dev/null || echo "none")
    if [[ "$pinned" == "$current" ]]; then
      echo "  ${name}: already at ${pinned:0:12}"
    else
      echo "  ${name}: ${current:0:12} -> ${pinned:0:12}"
      git -C "$repo_dir" fetch origin main --quiet
      git -C "$repo_dir" checkout "$pinned" --quiet
    fi
  done
}

save_stamps() {
  mkdir -p "$SCRATCH"
  meta_deps_hash > "$META_HASH_FILE"
  moxygen_rev > "$MOXYGEN_REV_FILE"
}

# --- Main ---

if needs_setup; then
  echo "No prior build found. Running initial setup..."
  "${SCRIPT_DIR}/setup-deps.sh"
  "${SCRIPT_DIR}/configure.sh"
  save_stamps
  cmake --build "${PROJECT_ROOT}/build"
  exit 0
fi

NEED_CONFIGURE=false

if meta_deps_changed; then
  echo "Meta dep pinned revisions changed."
  update_meta_repos
  "${SCRIPT_DIR}/build-meta-deps.sh"
  NEED_CONFIGURE=true
elif moxygen_changed; then
  echo "Moxygen source changed."
  "${SCRIPT_DIR}/build-moxygen.sh"
  NEED_CONFIGURE=true
fi

if [[ "$NEED_CONFIGURE" == true ]]; then
  "${SCRIPT_DIR}/configure.sh"
fi

save_stamps
cmake --build "${PROJECT_ROOT}/build"
