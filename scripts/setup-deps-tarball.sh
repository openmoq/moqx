#!/usr/bin/env bash
# setup-deps-tarball.sh — Populate .scratch with prebuilt moxygen release artifacts.
#
# Uses the submodule commit SHA to find the exact publish workflow run
# on openmoq/moxygen, then downloads the matching platform artifact.
# Writes .scratch/cmake_prefix_path.txt for configure.sh.
#
# Usage:
#   ./scripts/setup-deps-tarball.sh
#
# Requires: gh CLI authenticated (with actions:read on openmoq/moxygen),
#           deps/moxygen submodule initialized.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
SCRATCH="${ORLY_SCRATCH_PATH:-${PROJECT_ROOT}/.scratch}"
MOXYGEN_DIR="${PROJECT_ROOT}/deps/moxygen"

if [[ ! -e "$MOXYGEN_DIR/.git" ]]; then
    echo "Error: deps/moxygen submodule not initialized." >&2
    echo "  Run: git submodule update --init" >&2
    exit 1
fi

# ── Platform detection ────────────────────────────────────────────────────────

detect_platform() {
    local os arch
    os=$(uname -s)
    arch=$(uname -m)

    if [[ "$os" == "Darwin" ]]; then
        local ver
        ver=$(sw_vers -productVersion | cut -d. -f1)
        echo "macos-${ver}-arm64"
    elif [[ "$os" == "Linux" ]]; then
        if [[ ! -f /etc/os-release ]]; then
            echo "Error: cannot detect Linux distro (no /etc/os-release)" >&2
            exit 1
        fi
        # shellcheck disable=SC1091
        local ID VERSION_ID
        ID=$(. /etc/os-release && echo "$ID")
        VERSION_ID=$(. /etc/os-release && echo "${VERSION_ID:-}")
        local darch="${arch/x86_64/amd64}"
        darch="${darch/aarch64/arm64}"
        case "$ID" in
            ubuntu) echo "ubuntu-${VERSION_ID}-${darch}" ;;
            debian) echo "bookworm-${darch}" ;;
            *)
                echo "Error: unsupported Linux distro: $ID" >&2
                exit 1
                ;;
        esac
    else
        echo "Error: unsupported OS: $os" >&2
        exit 1
    fi
}

PLATFORM=$(detect_platform)
echo "==> Platform: $PLATFORM"

# ── Find publish run matching submodule SHA ───────────────────────────────────

SHA=$(git -C "$MOXYGEN_DIR" rev-parse HEAD)
echo "==> Moxygen submodule SHA: ${SHA:0:7}"

TARBALL="moxygen-${PLATFORM}.tar.gz"
DOWNLOAD_DIR="${SCRATCH}/downloads"
mkdir -p "$DOWNLOAD_DIR"

echo "==> Searching for publish run at ${SHA:0:7}..."
RUN_ID=$(gh api "repos/openmoq/moxygen/actions/workflows/omoq-publish-artifacts.yml/runs?head_sha=${SHA}&status=success&per_page=1" \
    --jq '.workflow_runs[0].id // empty')

if [[ -n "$RUN_ID" ]]; then
    echo "==> Found publish run $RUN_ID, downloading $TARBALL..."
    rm -f "${DOWNLOAD_DIR}/${TARBALL}"
    gh run download "$RUN_ID" \
        --repo openmoq/moxygen \
        --name "$TARBALL" \
        --dir "$DOWNLOAD_DIR"
else
    echo "Error: no successful publish run found for moxygen SHA ${SHA:0:7}." >&2
    echo "  The publish workflow may not have run yet for this commit." >&2
    echo "  Check: https://github.com/openmoq/moxygen/actions/workflows/omoq-publish-artifacts.yml" >&2
    exit 1
fi

# ── Extract ───────────────────────────────────────────────────────────────────

INSTALL_DIR="${SCRATCH}/moxygen-install"
echo "==> Extracting to $INSTALL_DIR..."
rm -rf "$INSTALL_DIR"
mkdir -p "$INSTALL_DIR"
tar xzf "${DOWNLOAD_DIR}/${TARBALL}" -C "$INSTALL_DIR"

# ── Write cmake_prefix_path.txt ───────────────────────────────────────────────

echo "$INSTALL_DIR" > "${SCRATCH}/cmake_prefix_path.txt"
echo "tarball" > "${SCRATCH}/deps-mode"

NLIBS=$(find "$INSTALL_DIR/lib" -name '*.a' 2>/dev/null | wc -l)
echo "==> Done: $NLIBS static libs in $INSTALL_DIR"
