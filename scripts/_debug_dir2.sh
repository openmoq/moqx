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
sleep 3

# Publisher connects to DOWNSTREAM relay
.scratch/moxygen-install/bin/moqdateserver \
    --relay_url="https://localhost:19670/moq-relay" \
    --ns="moq-date-2" --insecure 2>"$TMPD/date.log" &
DATEID=$!
sleep 3

echo "=== DOWNSTREAM: namespace fan-out + peering ==="
grep -iE "doPublish|peerSubNs|isPeer|peer|subscribeNamespace|publishNamespace|Namespace|NAMESPACE|subNs" \
    "$TMPD/down.log" | head -40

echo ""
echo "=== UPSTREAM: namespace receipt + ORelayNamespaceHandle ==="
grep -iE "doPublish|peerSubNs|isPeer|peer|subscribeNamespace|publishNamespace|Namespace|NAMESPACE|subNs" \
    "$TMPD/up.log" | head -40

echo ""
echo "=== UPSTREAM ERRORS ==="
grep -E "^E|^F|Check failed" "$TMPD/up.log" | head -20

echo ""
echo "=== DATESERVER ==="
cat "$TMPD/date.log" | head -5
