#!/usr/bin/env bash
set -euo pipefail

IMAGE=${1:-o-rly-ubuntu20}

docker build -t "${IMAGE}" -f docker/Dockerfile .
