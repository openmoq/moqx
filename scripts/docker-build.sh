#!/usr/bin/env bash
set -euo pipefail
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"

IMAGE=${1:-moqx-ubuntu}

docker build -t "${IMAGE}" -f "$PROJECT_ROOT"/docker/Dockerfile "$PROJECT_ROOT"
