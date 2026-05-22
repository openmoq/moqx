#!/usr/bin/env bash
# setup-deps-dev-artifact.sh — Populate .scratch with a dev-build moxygen
# artifact produced by the openmoq/moxygen omoq-dev-build workflow.
#
# Used as a fallback when the rolling snapshot release SHA doesn't match the
# moxygen submodule pin (typical case: developer pointed the submodule at a
# moxygen PR/feature branch that hasn't been merged-then-snapshotted yet).
# Looks up the most recent non-expired dev-build artifact whose name encodes
# the submodule's short SHA + the current platform; downloads it; extracts
# to .scratch/moxygen-install.
#
# Exits 0 on success (artifact found and extracted). Exits 1 if no matching
# artifact exists (or auth/network failure) — build.sh then falls through to
# the source-build mode.
#
# Usage:  ./scripts/setup-deps-dev-artifact.sh
#
# Env:
#   GITHUB_TOKEN / GH_TOKEN  required (artifacts API needs auth)
#   MOQX_MOXYGEN_REPO        default: openmoq/moxygen
#   MOQX_PLATFORM            default: auto-detected via uname

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
SCRATCH="${MOQX_SCRATCH_PATH:-${PROJECT_ROOT}/.scratch}"
MOXYGEN_REPO="${MOQX_MOXYGEN_REPO:-openmoq/moxygen}"
MOXYGEN_DIR="${PROJECT_ROOT}/deps/moxygen"
INSTALL_DIR="${SCRATCH}/moxygen-install"

# ── Platform detection ────────────────────────────────────────────────────────
# Match the naming used by the omoq-dev-build workflow's `target` input:
#   macos-15-arm64, ubuntu-22.04-amd64 (extend here as workflow adds targets)
detect_platform() {
    local os arch
    case "$(uname -s)" in
        Darwin) os="macos-15" ;;
        Linux)
            if [[ -f /etc/debian_version ]] && \
               grep -q bookworm /etc/debian_version 2>/dev/null; then
                os="bookworm"
            else
                os="ubuntu-22.04"
            fi
            ;;
        *) echo "Error: unsupported OS $(uname -s)" >&2; exit 1 ;;
    esac
    case "$(uname -m)" in
        arm64|aarch64) arch="arm64" ;;
        x86_64) arch="amd64" ;;
        *) echo "Error: unsupported arch $(uname -m)" >&2; exit 1 ;;
    esac
    echo "${os}-${arch}"
}

PLATFORM="${MOQX_PLATFORM:-$(detect_platform)}"

# ── Resolve target artifact name from submodule SHA + platform ───────────────
SUBMODULE_SHA=$(git -C "$MOXYGEN_DIR" rev-parse HEAD)
SHA7="${SUBMODULE_SHA:0:7}"
ARTIFACT_NAME="moxygen-dev-${PLATFORM}-${SHA7}.tar.gz"

# ── Auth: artifacts API requires a token even for public repos ───────────────
TOKEN="${GITHUB_TOKEN:-${GH_TOKEN:-}}"
if [[ -z "$TOKEN" ]]; then
    echo "==> Dev-build artifact lookup: no GITHUB_TOKEN/GH_TOKEN set, skipping" >&2
    exit 1
fi

# ── Query the artifacts API ───────────────────────────────────────────────────
# `?name=<exact>` filters server-side; we just take the first non-expired hit.
echo "==> Searching for dev-build artifact: ${ARTIFACT_NAME}"
ARTIFACT_JSON=$(curl -fsSL -H "Authorization: Bearer ${TOKEN}" \
    "https://api.github.com/repos/${MOXYGEN_REPO}/actions/artifacts?name=${ARTIFACT_NAME}") || {
    echo "Error: failed to query artifacts API for ${MOXYGEN_REPO}" >&2
    exit 1
}

ARTIFACT_URL=$(printf '%s' "$ARTIFACT_JSON" | python3 -c "
import json, sys
data = json.load(sys.stdin)
for a in data.get('artifacts', []):
    if not a.get('expired', False):
        print(a['archive_download_url'])
        break
")

if [[ -z "$ARTIFACT_URL" ]]; then
    echo "==> No non-expired dev-build artifact found for ${SHA7} on ${PLATFORM}"
    exit 1
fi

# ── Download + extract ────────────────────────────────────────────────────────
echo "==> Downloading dev-build artifact..."
DOWNLOAD_DIR="${SCRATCH}/downloads"
mkdir -p "$DOWNLOAD_DIR"
ZIP="${DOWNLOAD_DIR}/${ARTIFACT_NAME}.zip"
rm -f "$ZIP"
curl -fsSL -H "Authorization: Bearer ${TOKEN}" "${ARTIFACT_URL}" -o "$ZIP"

echo "==> Extracting to ${INSTALL_DIR}..."
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT
unzip -q "$ZIP" -d "$TMP"
mkdir -p "$INSTALL_DIR"
tar -C "$INSTALL_DIR" -xzf "$TMP"/*.tar.gz

# ── Sanity check ──────────────────────────────────────────────────────────────
# Either path is acceptable (CMake layouts have varied across moxygen versions).
if [[ ! -f "$INSTALL_DIR/lib/cmake/moxygen/moxygen-config.cmake" ]] && \
   [[ ! -f "$INSTALL_DIR/lib/cmake/folly/moxygen-config.cmake" ]]; then
    echo "Error: artifact extracted but moxygen-config.cmake not found in install tree" >&2
    exit 1
fi

# Write the sentinel cmake_prefix_path.txt that build.sh checks before
# building. Mirrors setup-deps-tarball.sh / setup-deps-standalone.sh so the
# dev-artifact mode produces the same post-setup state as the other modes.
echo "$INSTALL_DIR" > "${SCRATCH}/cmake_prefix_path.txt"

echo "==> Done: dev-build artifact extracted to ${INSTALL_DIR}"
