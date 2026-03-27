#!/usr/bin/env bash
set -euo pipefail
REPO="$(cd "$(dirname "$0")/.." && pwd)"
cd "$REPO"

TMPD=$(mktemp -d)
trap 'kill "${PID:-}" 2>/dev/null; wait 2>/dev/null || true; rm -rf "$TMPD"' EXIT

cat >"$TMPD/up.yaml" <<'YAML'
relay_id: "upstream-test"
listeners:
  - name: upstream
    udp:
      socket:
        address: "::"
        port: 19668
    tls:
      insecure: true
    endpoint: "/moq-relay"
services:
  default:
    match:
      - authority: {any: true}
        path: {prefix: "/"}
    cache:
      enabled: false
      max_tracks: 100
      max_groups_per_track: 3
YAML

echo "Starting upstream relay..."
./build/o_rly --config="$TMPD/up.yaml" 2>&1 &
PID=$!
sleep 3
if kill -0 $PID 2>/dev/null; then
    echo "Relay still running after 3s - OK"
else
    wait $PID; echo "Relay exited with code $?"
fi
