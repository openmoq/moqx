#!/usr/bin/env bash
set -euo pipefail
REPO="$(cd "$(dirname "$0")/.." && pwd)"
cd "$REPO"

TMPD=$(mktemp -d)
trap 'kill "${UPID:-}" "${DNID:-}" "${DATEID:-}" 2>/dev/null; wait 2>/dev/null || true; rm -rf "$TMPD"' EXIT

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

./build/o_rly --config="$TMPD/up.yaml" --logging=DBG4 2>"$TMPD/up.log" &
UPID=$!
sleep 1

./build/o_rly --config="$TMPD/down.yaml" --logging=DBG4 2>"$TMPD/down.log" &
DNID=$!
sleep 2

.scratch/moxygen-install/bin/moqdateserver \
    --relay_url="https://localhost:19668/moq-relay" \
    --ns="moq-date" --insecure 2>"$TMPD/date.log" &
DATEID=$!
sleep 3

echo "=== UPSTREAM (peering/namespace lines) ==="
grep -iE "isPeer|peerSubNs|PublishNamespace|subscribeNamespace|doPublish|Negotiated|draft|ALPN|relayID|peer" \
    "$TMPD/up.log" 2>/dev/null | head -30 || echo "(no matches)"

echo ""
echo "=== DOWNSTREAM (peering/connect lines) ==="
grep -iE "isPeer|peerSubNs|PublishNamespace|subscribeNamespace|doPublish|Negotiated|draft|ALPN|relayID|peer|connect|reconnect" \
    "$TMPD/down.log" 2>/dev/null | head -30 || echo "(no matches)"

echo ""
echo "=== DOWNSTREAM ERRORS ==="
grep -E "^E|^F|Check failed" "$TMPD/down.log" 2>/dev/null | head -20 || echo "(none)"

echo ""
echo "=== DATESERVER ==="
cat "$TMPD/date.log" 2>/dev/null | head -10 || echo "(no output)"
