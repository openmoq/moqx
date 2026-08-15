#!/usr/bin/env bash
set -euo pipefail

IMAGE=${1:-moqx-ubuntu}

docker run --rm -it \
  -v "$(pwd)":/workspace \
  -w /workspace \
  "${IMAGE}" bash
