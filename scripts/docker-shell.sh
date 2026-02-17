#!/usr/bin/env bash
set -euo pipefail

IMAGE=${1:-o-rly-ubuntu20}

docker run --rm -it \
  -v "$(pwd)":/workspace \
  -w /workspace \
  "${IMAGE}" bash
