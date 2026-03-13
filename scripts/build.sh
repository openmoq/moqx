#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
SCRATCH="${ORLY_SCRATCH_PATH:-${PROJECT_ROOT}/.scratch}"

HASHES_DIR="${PROJECT_ROOT}/deps/moxygen/build/deps/github_hashes"
MANIFESTS_DIR="${PROJECT_ROOT}/deps/moxygen/build/fbcode_builder/manifests"
META_HASH_FILE="${SCRATCH}/meta-deps.hash"
MANIFESTS_HASH_FILE="${SCRATCH}/manifests.hash"
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

manifests_hash() {
  local deps
  deps=$(python3 "${MANIFESTS_DIR}/../getdeps.py" list-deps moxygen 2>/dev/null)
  local files=()
  for dep in $deps; do
    files+=("${MANIFESTS_DIR}/${dep}")
  done
  cat "${files[@]}" 2>/dev/null | sha256sum | awk '{print $1}'
}

manifests_changed() {
  local current
  current=$(manifests_hash)
  if [[ ! -f "$MANIFESTS_HASH_FILE" ]] || [[ "$current" != "$(cat "$MANIFESTS_HASH_FILE")" ]]; then
    return 0
  fi
  return 1
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

meta_deps_installed() {
  local inst_dir="${SCRATCH}/installed"
  for key in "${!REPO_DIRS[@]}"; do
    local name
    name=$(basename "$key")
    local pinned
    pinned=$(awk '{print $3}' "${HASHES_DIR}/${key}-rev.txt" 2>/dev/null) || return 1
    local stamp="${inst_dir}/${name}/.build-rev"
    [[ -f "$stamp" ]] && [[ "$(cat "$stamp")" == "$pinned" ]] || return 1
  done
  return 0
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

has_stale_patches() {
  find "$SCRATCH" -name .getdeps_patched -print -quit 2>/dev/null | grep -q .
}

warn_stale_patches() {
  echo ""
  echo "WARNING: .scratch contains previously patched sources that may conflict."
  echo "If the build fails with patch errors, reset them with:"
  echo ""
  echo "  find ${SCRATCH} -name .getdeps_patched -execdir git checkout -- . \\; -delete"
  echo ""
}

save_stamps() {
  mkdir -p "$SCRATCH"
  meta_deps_hash > "$META_HASH_FILE"
  manifests_hash > "$MANIFESTS_HASH_FILE"
  moxygen_rev > "$MOXYGEN_REV_FILE"
  local inst_dir="${SCRATCH}/installed"
  for key in "${!REPO_DIRS[@]}"; do
    local name
    name=$(basename "$key")
    local repo_dir="${SCRATCH}/repos/${REPO_DIRS[$key]}"
    [[ -d "$repo_dir" ]] && git -C "$repo_dir" rev-parse HEAD > "${inst_dir}/${name}/.build-rev"
  done
}

# --- Main ---

if needs_setup; then
  echo "No prior build found. Running initial setup..."
  "${SCRIPT_DIR}/setup-deps.sh"
  update_meta_repos
  "${SCRIPT_DIR}/configure.sh"
  save_stamps
  cmake --build "${PROJECT_ROOT}/build"
  exit 0
fi

NEED_CONFIGURE=false

if manifests_changed; then
  echo "Manifest files changed. Re-running full dependency setup..."
  "${SCRIPT_DIR}/setup-deps.sh"
  NEED_CONFIGURE=true
elif meta_deps_changed || ! meta_deps_installed; then
  echo "Meta dep pinned revisions changed or installs incomplete."
  has_stale_patches && warn_stale_patches
  "${SCRIPT_DIR}/build-meta-deps.sh"
  NEED_CONFIGURE=true
elif moxygen_changed; then
  echo "Moxygen source changed."
  has_stale_patches && warn_stale_patches
  "${SCRIPT_DIR}/build-moxygen.sh"
  NEED_CONFIGURE=true
fi

update_meta_repos

if [[ "$NEED_CONFIGURE" == true ]]; then
  "${SCRIPT_DIR}/configure.sh"
fi

save_stamps
cmake --build "${PROJECT_ROOT}/build"
