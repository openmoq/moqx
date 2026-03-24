#!/usr/bin/env bash
# build.sh — Developer build script for o-rly.
#
# Mirrors the same build flows used by CI. Supports two dependency modes:
#   --from-release  — download released moxygen artifacts (~1 min)
#   --from-source   — build moxygen + all Meta deps from source (~15-30 min)
#
# Usage:
#   ./scripts/build.sh setup [--from-release [SHA]|--from-source [SHA]] [--no-fallback] [--clean]
#   ./scripts/build.sh [--profile NAME] [--build-dir DIR]
#   ./scripts/build.sh test [--build-dir DIR] [-- CTEST_ARGS...]
#
# First time:
#   git submodule update --init
#   sudo deps/moxygen/standalone/install-system-deps.sh   # or see check output
#   ./scripts/build.sh setup
#   ./scripts/build.sh
#   ./scripts/build.sh test
#
# Incremental (after source changes):
#   ./scripts/build.sh          # rebuilds only what changed
#
# After submodule update:
#   ./scripts/build.sh setup    # re-downloads or rebuilds deps
#   ./scripts/build.sh

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
SCRATCH="${ORLY_SCRATCH_PATH:-${PROJECT_ROOT}/.scratch}"
MOXYGEN_DIR="${PROJECT_ROOT}/deps/moxygen"

PREFIX_PATH_FILE="${SCRATCH}/cmake_prefix_path.txt"
DEPS_MODE_FILE="${SCRATCH}/deps-mode"

# ── Helpers ──────────────────────────────────────────────────────────────────

die() { echo "Error: $*" >&2; exit 1; }

usage() {
  cat <<'EOF'
Usage:
  build.sh setup [--from-release [SHA]|--from-source [SHA]] [--no-fallback] [--clean]
  build.sh [--profile NAME] [--build-dir DIR]
  build.sh test [--build-dir DIR] [-- CTEST_ARGS...]

Commands:
  setup     Install moxygen dependencies (from release or source)
  (default) Configure and build o-rly
  test      Run tests

Setup options:
  --from-release [SHA]  Download released artifacts (default, fast)
                        Optional SHA overrides submodule commit
  --from-source [SHA]   Build all deps from source (slow, full control)
                        Optional SHA overrides submodule commit
  --moxygen-dir DIR     Use a local moxygen directory instead of the submodule
                        (--from-source only; mutually exclusive with SHA)
  --no-fallback         Fail if requested mode unavailable (don't auto-switch)
  --clean               Remove .scratch and start fresh

Build options:
  --profile NAME  Build profile: "default" (RelWithDebInfo) or "san" (ASAN/UBSAN)
  --build-dir DIR Build directory (default: per profile)

Test options:
  --build-dir DIR Build directory to test (default: "build")
  -- ARGS...      Extra arguments passed to ctest
EOF
  exit 0
}

# ── System dependency check ──────────────────────────────────────────────────

check_system_deps() {
  local missing=()
  local warnings=()

  # Tools
  command -v cmake >/dev/null 2>&1 || missing+=("cmake")
  command -v ninja >/dev/null 2>&1 || missing+=("ninja")
  command -v git >/dev/null 2>&1   || missing+=("git")

  # Libraries — check via pkg-config where available, fall back to header probes
  if command -v pkg-config >/dev/null 2>&1; then
    pkg-config --exists openssl 2>/dev/null    || missing+=("libssl-dev")
    pkg-config --exists libglog 2>/dev/null    || missing+=("libgoogle-glog-dev")
    pkg-config --exists gflags 2>/dev/null     || missing+=("libgflags-dev")
    pkg-config --exists zlib 2>/dev/null       || missing+=("zlib1g-dev")
    pkg-config --exists fmt 2>/dev/null        || missing+=("libfmt-dev")
    pkg-config --exists libevent 2>/dev/null   || missing+=("libevent-dev")
    pkg-config --exists libsodium 2>/dev/null  || missing+=("libsodium-dev")
    pkg-config --exists libzstd 2>/dev/null    || missing+=("libzstd-dev")
    pkg-config --exists libcares 2>/dev/null   || missing+=("libc-ares-dev")
  else
    warnings+=("pkg-config not found — cannot verify library dependencies")
    # Fall back to header checks for the most critical ones
    for hdr in openssl/ssl.h glog/logging.h gflags/gflags.h boost/version.hpp; do
      if ! find /usr/include /usr/local/include -name "$(basename "$hdr")" -path "*$hdr" 2>/dev/null | grep -q .; then
        missing+=("$hdr (header not found)")
      fi
    done
  fi

  # Boost — special: pkg-config not always available, check header
  if ! find /usr/include /usr/local/include -name "version.hpp" -path "*/boost/*" 2>/dev/null | grep -q .; then
    if ! command -v brew >/dev/null 2>&1 || ! brew --prefix boost >/dev/null 2>&1; then
      missing+=("libboost-all-dev")
    fi
  fi

  # CMake version check (need 3.25+)
  if command -v cmake >/dev/null 2>&1; then
    local ver
    ver=$(cmake --version | head -1 | grep -oP '\d+\.\d+' | head -1)
    local major minor
    major=$(echo "$ver" | cut -d. -f1)
    minor=$(echo "$ver" | cut -d. -f2)
    if (( major < 3 || (major == 3 && minor < 25) )); then
      missing+=("cmake 3.25+ (found $ver)")
    fi
  fi

  for w in "${warnings[@]+"${warnings[@]}"}"; do
    echo "  Warning: $w"
  done

  if (( ${#missing[@]} > 0 )); then
    echo ""
    echo "Missing system dependencies:"
    for dep in "${missing[@]}"; do
      echo "  - $dep"
    done
    echo ""

    # Detect OS and suggest install command
    if [[ "$(uname)" == "Darwin" ]]; then
      echo "Install with:"
      echo "  brew install cmake ninja openssl@3 glog gflags double-conversion \\"
      echo "    libevent libsodium zstd boost fmt c-ares gperf"
    elif [[ -f /etc/os-release ]]; then
      . /etc/os-release
      case "${ID:-}" in
        ubuntu|debian)
          echo "Install with:"
          echo "  sudo apt-get install -y build-essential cmake ninja-build \\"
          echo "    libssl-dev libunwind-dev libgoogle-glog-dev libgflags-dev \\"
          echo "    libdouble-conversion-dev libevent-dev libsodium-dev libzstd-dev \\"
          echo "    libboost-all-dev libfmt-dev zlib1g-dev libc-ares-dev gperf"
          ;;
        fedora|centos|rhel)
          echo "Install with:"
          echo "  sudo dnf install -y cmake ninja-build openssl-devel glog-devel \\"
          echo "    gflags-devel double-conversion-devel libevent-devel libsodium-devel \\"
          echo "    libzstd-devel boost-devel fmt-devel zlib-devel c-ares-devel gperf"
          ;;
        *)
          echo "See deps/moxygen/standalone/install-system-deps.sh for package list."
          ;;
      esac
    fi
    echo ""
    echo "Or run: sudo deps/moxygen/standalone/install-system-deps.sh"
    return 1
  fi
  return 0
}

# ── Submodule check ──────────────────────────────────────────────────────────

check_submodule() {
  if [[ ! -e "$MOXYGEN_DIR/.git" ]]; then
    die "deps/moxygen submodule not initialized.
  Run: git submodule update --init"
  fi
}

# ── Checkout submodule to specific SHA ───────────────────────────────────────

checkout_submodule() {
  local sha="$1"
  echo "==> Checking out moxygen submodule at $sha..."
  git -C "$MOXYGEN_DIR" fetch origin --quiet
  git -C "$MOXYGEN_DIR" checkout "$sha" --quiet
}

# ── Setup command ────────────────────────────────────────────────────────────

cmd_setup() {
  local mode="from-release"
  local no_fallback=false
  local clean=false
  local target_sha=""
  local moxygen_dir=""

  while (( $# > 0 )); do
    case "$1" in
      --from-release)
        mode="from-release"; shift
        # Optional SHA argument (next arg that doesn't start with --)
        if (( $# > 0 )) && [[ "$1" != --* ]]; then
          target_sha="$1"; shift
        fi
        ;;
      --from-source)
        mode="from-source"; shift
        if (( $# > 0 )) && [[ "$1" != --* ]]; then
          target_sha="$1"; shift
        fi
        ;;
      --moxygen-dir)
        [[ $# -gt 1 ]] || die "--moxygen-dir requires a path argument"
        moxygen_dir="$(cd "$2" && pwd)" || die "--moxygen-dir: '$2' not found"
        shift 2
        ;;
      --no-fallback) no_fallback=true; shift ;;
      --clean)       clean=true; shift ;;
      -h|--help)     usage ;;
      *)             die "Unknown setup option: $1" ;;
    esac
  done

  if [[ -n "$moxygen_dir" && -n "$target_sha" ]]; then
    die "--moxygen-dir and a SHA override are mutually exclusive"
  fi

  if [[ -n "$moxygen_dir" ]]; then
    export ORLY_MOXYGEN_DIR="$moxygen_dir"
    echo "Using moxygen from: $moxygen_dir"
  else
    check_submodule
    if [[ -n "$target_sha" ]]; then
      checkout_submodule "$target_sha"
    fi
  fi

  # Skip system dep check for from-release with --no-fallback (e.g. CI publish
  # job that only downloads the tarball, doesn't build on the host).
  if [[ "$mode" != "from-release" ]] || ! $no_fallback; then
    echo "Checking system dependencies..."
    if ! check_system_deps; then
      die "Install missing dependencies and re-run."
    fi
    echo "  All system dependencies found."
  fi

  if $clean; then
    echo "Cleaning .scratch..."
    rm -rf "$SCRATCH"
  fi

  mkdir -p "$SCRATCH"

  if [[ "$mode" == "from-release" ]]; then
    echo ""
    echo "==> Setting up dependencies (from release)..."
    if bash "$SCRIPT_DIR/setup-deps-release.sh"; then
      echo "from-release" > "$DEPS_MODE_FILE"
    else
      if $no_fallback; then
        die "Release artifacts not available and --no-fallback specified."
      fi
      echo ""
      echo "Release artifacts not available — falling back to source build..."
      mode="from-source"
    fi
  fi

  if [[ "$mode" == "from-source" ]]; then
    echo ""
    echo "==> Setting up dependencies (from source)..."
    bash "$SCRIPT_DIR/setup-deps-source.sh"
    echo "from-source" > "$DEPS_MODE_FILE"
  fi

  echo ""
  echo "Setup complete (mode: $(cat "$DEPS_MODE_FILE"))."
  echo "Run: ./scripts/build.sh"
}

# ── Build command (default) ──────────────────────────────────────────────────

cmd_build() {
  local profile="default"
  local build_dir=""

  while (( $# > 0 )); do
    case "$1" in
      --profile)   profile="$2"; shift 2 ;;
      --build-dir) build_dir="$2"; shift 2 ;;
      -h|--help)   usage ;;
      *)           die "Unknown build option: $1" ;;
    esac
  done

  # Default build dir from profile
  if [[ -z "$build_dir" ]]; then
    case "$profile" in
      default) build_dir="build" ;;
      san)     build_dir="build-san" ;;
      *)       build_dir="build-${profile}" ;;
    esac
  fi

  if [[ ! -f "$PREFIX_PATH_FILE" ]]; then
    die "Dependencies not set up. Run: ./scripts/build.sh setup"
  fi

  check_submodule

  local prefix_path
  prefix_path=$(cat "$PREFIX_PATH_FILE")

  local nproc
  nproc=$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4)

  # Map profile to cmake preset
  local preset="$profile"

  # from-source builds use system libs (e.g. gflags shared-only on CentOS);
  # override the preset's .a-only suffix to allow shared system libraries.
  local extra_cmake_args=()
  local deps_mode=""
  [[ -f "$DEPS_MODE_FILE" ]] && deps_mode=$(cat "$DEPS_MODE_FILE")
  if [[ "$deps_mode" == "from-source" ]]; then
    extra_cmake_args+=("-DCMAKE_FIND_LIBRARY_SUFFIXES=.so;.a")
    extra_cmake_args+=("-DGFLAGS_SHARED=ON")
  fi

  echo "==> Configuring (profile: $profile, build: $build_dir)..."
  cmake -S "$PROJECT_ROOT" -B "$build_dir" \
    --preset "$preset" \
    "${extra_cmake_args[@]+"${extra_cmake_args[@]}"}" \
    -DCMAKE_PREFIX_PATH="$prefix_path"

  echo "==> Building ($nproc jobs)..."
  cmake --build "$build_dir" -j"$nproc"

  echo "==> Build complete."
}

# ── Test command ─────────────────────────────────────────────────────────────

cmd_test() {
  local build_dir="build"
  local ctest_args=()

  while (( $# > 0 )); do
    case "$1" in
      --build-dir) build_dir="$2"; shift 2 ;;
      --)          shift; ctest_args=("$@"); break ;;
      -h|--help)   usage ;;
      *)           die "Unknown test option: $1" ;;
    esac
  done

  if [[ ! -d "$build_dir" ]]; then
    die "Build directory '$build_dir' not found. Run: ./scripts/build.sh"
  fi

  echo "==> Running tests (build: $build_dir)..."
  ctest --test-dir "$build_dir" --output-on-failure "${ctest_args[@]+"${ctest_args[@]}"}"
}

# ── Main ─────────────────────────────────────────────────────────────────────

if (( $# == 0 )); then
  cmd_build
  exit 0
fi

case "$1" in
  setup)  shift; cmd_setup "$@" ;;
  test)   shift; cmd_test "$@" ;;
  -h|--help) usage ;;
  -*)     cmd_build "$@" ;;
  *)      die "Unknown command: $1. Use: setup, test, or build options (--profile, --build-dir)" ;;
esac
