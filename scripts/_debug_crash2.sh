#!/usr/bin/env bash
set -euo pipefail
REPO="$(cd "$(dirname "$0")/.." && pwd)"
cd "$REPO"

TMPD=$(mktemp -d)
trap 'kill "${UPID:-}" "${DNID:-}" 2>/dev/null; wait 2>/dev/null || true; rm -rf "$TMPD"' EXIT

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

cat >"$TMPD/down.yaml" <<'YAML'
relay_id: "downstream-test"
listeners:
  - name: downstream
    udp:
      socket:
        address: "::"
        port: 19670
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
upstream:
  url: "moqt://localhost:19668/moq-relay"
  tls:
    insecure: true
YAML

echo "Starting upstream relay..."
./build/o_rly --config="$TMPD/up.yaml" 2>"$TMPD/up.log" &
UPID=$!

echo "Starting downstream relay..."
./build/o_rly --config="$TMPD/down.yaml" 2>"$TMPD/down.log" &
DNID=$!

sleep 3

echo "=== Upstream alive? ==="
kill -0 $UPID 2>/dev/null && echo "YES" || echo "NO (crashed)"

echo "=== Downstream alive? ==="
kill -0 $DNID 2>/dev/null && echo "YES" || echo "NO (crashed)"

echo "=== Upstream log ==="
cat "$TMPD/up.log" | head -20

echo "=== Downstream log ==="
cat "$TMPD/down.log" | head -20
