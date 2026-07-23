#!/usr/bin/env bash
# setup-deps-tarball.sh — Populate .scratch with prebuilt moxygen release artifacts.
#
# Downloads the moxygen `snapshot-latest` GitHub release tarball matching
# the current platform. By default, verifies the snapshot commit matches
# the submodule SHA (the moxygen-sync workflow keeps these aligned); on
# mismatch, exits non-zero so the caller (build.sh setup) can fall back
# to setup-deps-standalone.sh (source build).
#
# Pass --use-latest (or set MOQX_TARBALL_USE_LATEST=1) to bypass the SHA
# check and use the snapshot regardless of submodule pin. This is useful
# for quick development against the current moxygen tip when you don't
# need exact submodule reproducibility.
#
# Anonymous downloads from the public moxygen repo — no authentication
# required, works for fork PRs.
#
# Usage:
#   ./scripts/setup-deps-tarball.sh [--use-latest]
#
# Requires: curl, deps/moxygen submodule initialized.

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
SCRATCH="${MOQX_SCRATCH_PATH:-${PROJECT_ROOT}/.scratch}"
MOXYGEN_DIR="${PROJECT_ROOT}/deps/moxygen"
MOXYGEN_REPO="${MOQX_MOXYGEN_REPO:-openmoq/moxygen}"
RELEASE_TAG="${MOQX_MOXYGEN_RELEASE_TAG:-snapshot-latest}"
USE_LATEST="${MOQX_TARBALL_USE_LATEST:-}"

while [[ $# -gt 0 ]]; do
    case "$1" in
        --use-latest) USE_LATEST=1; shift ;;
        -h|--help)
            sed -n '2,22p' "$0" | sed 's/^# \?//'
            exit 0
            ;;
        *)
            echo "Error: unknown argument: $1" >&2
            echo "Usage: $0 [--use-latest]" >&2
            exit 2
            ;;
    esac
done

if [[ ! -e "$MOXYGEN_DIR/.git" ]]; then
    echo "Error: deps/moxygen submodule not initialized." >&2
    echo "  Run: git submodule update --init" >&2
    exit 1
fi

# ── Platform detection ────────────────────────────────────────────────────────
# Shared with setup-deps-dev-artifact.sh; handles Ubuntu/Debian derivatives
# (Linux Mint, Pop!_OS, ...) via ID_LIKE. See detect-platform.sh.

# shellcheck source=scripts/detect-platform.sh
source "$SCRIPT_DIR/detect-platform.sh"

PLATFORM="${MOQX_PLATFORM:-$(detect_platform)}"
echo "==> Platform: $PLATFORM"

# ── Verify snapshot SHA matches submodule ─────────────────────────────────────

SUBMODULE_SHA=$(git -C "$MOXYGEN_DIR" rev-parse HEAD)
echo "==> Moxygen submodule SHA: ${SUBMODULE_SHA:0:7}"

echo "==> Fetching ${RELEASE_TAG} release metadata..."
# Authenticate when GITHUB_TOKEN/GH_TOKEN is set (in CI). The api.github.com
# unauthenticated rate limit (60/hour/IP) is exhausted quickly on shared
# runners, especially macOS. Authenticated requests get 1000+/hour, which
# applies even to read-only fork-PR GITHUB_TOKENs.
RELEASE_API_URL="https://api.github.com/repos/${MOXYGEN_REPO}/releases/tags/${RELEASE_TAG}"
TOKEN="${GITHUB_TOKEN:-${GH_TOKEN:-}}"
if [[ -n "$TOKEN" ]]; then
    RELEASE_JSON=$(curl -fsSL -H "Authorization: Bearer ${TOKEN}" "$RELEASE_API_URL") || {
        echo "Error: failed to fetch release metadata for ${MOXYGEN_REPO}@${RELEASE_TAG}" >&2
        exit 1
    }
else
    RELEASE_JSON=$(curl -fsSL "$RELEASE_API_URL") || {
        echo "Error: failed to fetch release metadata for ${MOXYGEN_REPO}@${RELEASE_TAG}" >&2
        echo "       (no GITHUB_TOKEN set; api.github.com unauthenticated rate limit may be exhausted)" >&2
        exit 1
    }
fi

# Extract embedded commit SHA from release body. publish-artifacts.sh writes
# `**Commit:** \`<sha>\`` into the body — match the first 40-hex backtick group.
SNAPSHOT_SHA=$(printf '%s' "$RELEASE_JSON" | grep -oE '`[a-f0-9]{40}`' | head -1 | tr -d '`')

if [[ -z "$SNAPSHOT_SHA" ]]; then
    echo "Error: could not parse snapshot commit SHA from release body" >&2
    exit 1
fi

echo "==> Snapshot release SHA:  ${SNAPSHOT_SHA:0:7}"

if [[ "$SNAPSHOT_SHA" != "$SUBMODULE_SHA" ]]; then
    if [[ -n "$USE_LATEST" ]]; then
        echo "Warning: snapshot SHA does not match submodule pin." >&2
        echo "         submodule: ${SUBMODULE_SHA}" >&2
        echo "         snapshot:  ${SNAPSHOT_SHA}" >&2
        echo "         --use-latest set; proceeding with snapshot anyway." >&2
    else
        echo "Error: ${RELEASE_TAG} does not match the moxygen submodule pin." >&2
        echo "       submodule: ${SUBMODULE_SHA}" >&2
        echo "       snapshot:  ${SNAPSHOT_SHA}" >&2
        echo "       The moxygen-sync workflow normally keeps these aligned." >&2
        echo "       Options:" >&2
        echo "         scripts/build.sh setup --from-source     # build pinned SHA from source" >&2
        echo "         scripts/build.sh setup --use-latest      # use snapshot anyway" >&2
        echo "         git submodule update --remote deps/moxygen && git add deps/moxygen" >&2
        exit 1
    fi
fi

# ── Download tarball ──────────────────────────────────────────────────────────

TARBALL="moxygen-${PLATFORM}.tar.gz"
DOWNLOAD_DIR="${SCRATCH}/downloads"
mkdir -p "$DOWNLOAD_DIR"

DOWNLOAD_URL="https://github.com/${MOXYGEN_REPO}/releases/download/${RELEASE_TAG}/${TARBALL}"
echo "==> Downloading $TARBALL..."
rm -f "${DOWNLOAD_DIR}/${TARBALL}"
curl -fsSL "$DOWNLOAD_URL" -o "${DOWNLOAD_DIR}/${TARBALL}" || {
    echo "Error: failed to download ${DOWNLOAD_URL}" >&2
    echo "       (the platform tarball may not be in this snapshot)" >&2
    exit 1
}

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
