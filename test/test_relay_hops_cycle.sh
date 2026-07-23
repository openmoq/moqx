#!/usr/bin/env bash
# Verifies that RELAY_HOPS stabilizes namespace propagation around A -> B -> C -> A.

set -euo pipefail

REPO="$(cd "$(dirname "$0")/.." && pwd)"
BINARY="${1:-$REPO/build/moqx}"
MOQBIN="${MOQBIN:-$REPO/.scratch/moxygen-install/bin}"
source "$REPO/test/test_ports.sh"
source "$REPO/test/test_versions.sh"

DATESERVER="$MOQBIN/moqdateserver"
NAMESPACE="relay-hop-cycle"
TMPDIR_SCRIPT="$(mktemp -d)"
PIDS=()
RELAY_PIDS=()

cleanup() {
  for pid in "${PIDS[@]:-}" "${RELAY_PIDS[@]:-}"; do
    kill "$pid" 2>/dev/null || true
  done
  wait "${PIDS[@]:-}" "${RELAY_PIDS[@]:-}" 2>/dev/null || true
  rm -rf "$TMPDIR_SCRIPT"
}
trap cleanup EXIT

for file in "$BINARY" "$DATESERVER"; do
  if [[ ! -x "$file" ]]; then
    echo "ERROR: not found or not executable: $file" >&2
    exit 1
  fi
done

for port in \
  "$TEST_RELAY_HOPS_A" "$TEST_RELAY_HOPS_A_ADMIN" \
  "$TEST_RELAY_HOPS_B" "$TEST_RELAY_HOPS_B_ADMIN" \
  "$TEST_RELAY_HOPS_C" "$TEST_RELAY_HOPS_C_ADMIN"; do
  if ss -ulnp 2>/dev/null | grep -q ":$port "; then
    echo "ERROR: port $port already in use" >&2
    exit 1
  fi
done

write_config() {
  local path="$1" relay_id="$2" listen_port="$3" admin_port="$4" upstream_port="$5"
  cat >"$path" <<EOF
relay_id: "$relay_id"
threads: 2
use_relay_thread: true
use_local_forwarders: true
listeners:
  - name: relay
    udp:
      socket:
        address: "::"
        port: $listen_port
    tls:
      insecure: true
    endpoint: "/moq-relay"
    moqt_versions: ${MOQT_TEST_VERSIONS}
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
      url: "moqt://localhost:$upstream_port/moq-relay"
      tls:
        insecure: true
      idle_timeout_ms: 60000
admin:
  port: $admin_port
  address: "::"
  plaintext: true
EOF
}

write_config \
  "$TMPDIR_SCRIPT/a.yaml" "cycle-a" \
  "$TEST_RELAY_HOPS_A" "$TEST_RELAY_HOPS_A_ADMIN" "$TEST_RELAY_HOPS_B"
write_config \
  "$TMPDIR_SCRIPT/b.yaml" "cycle-b" \
  "$TEST_RELAY_HOPS_B" "$TEST_RELAY_HOPS_B_ADMIN" "$TEST_RELAY_HOPS_C"
write_config \
  "$TMPDIR_SCRIPT/c.yaml" "cycle-c" \
  "$TEST_RELAY_HOPS_C" "$TEST_RELAY_HOPS_C_ADMIN" "$TEST_RELAY_HOPS_A"

for relay in a b c; do
  "$BINARY" --config="$TMPDIR_SCRIPT/$relay.yaml" \
    >"$TMPDIR_SCRIPT/$relay.log" 2>&1 &
  RELAY_PIDS+=($!)
done

wait_ready() {
  local admin_port="$1" label="$2"
  local deadline=$(( $(date +%s) + 15 ))
  until curl -sf "http://localhost:$admin_port/info" >/dev/null 2>&1; do
    if (( $(date +%s) >= deadline )); then
      echo "ERROR: relay $label was not ready" >&2
      cat "$TMPDIR_SCRIPT/$label.log" >&2
      exit 1
    fi
    sleep 0.1
  done
}

wait_namespace() {
  local admin_port="$1" label="$2"
  local deadline=$(( $(date +%s) + 15 ))
  until curl -sf "http://localhost:$admin_port/state" |
      jq -e --arg ns "$NAMESPACE" \
        '.services.default.namespace_tree.children | has($ns)' >/dev/null; do
    if (( $(date +%s) >= deadline )); then
      echo "ERROR: namespace did not reach relay $label" >&2
      curl -sf "http://localhost:$admin_port/state" >&2 || true
      exit 1
    fi
    sleep 0.1
  done
}

wait_ready "$TEST_RELAY_HOPS_A_ADMIN" a
wait_ready "$TEST_RELAY_HOPS_B_ADMIN" b
wait_ready "$TEST_RELAY_HOPS_C_ADMIN" c

"$DATESERVER" \
  --relay_url="https://localhost:$TEST_RELAY_HOPS_A/moq-relay" \
  --ns="$NAMESPACE" --insecure \
  >"$TMPDIR_SCRIPT/dateserver.log" 2>&1 &
PIDS+=($!)

wait_namespace "$TEST_RELAY_HOPS_A_ADMIN" a
wait_namespace "$TEST_RELAY_HOPS_B_ADMIN" b
wait_namespace "$TEST_RELAY_HOPS_C_ADMIN" c

snapshot_tree() {
  local admin_port="$1"
  curl -sf "http://localhost:$admin_port/state" |
    jq -Sc '.services.default.namespace_tree'
}

sleep 1
tree_a_1="$(snapshot_tree "$TEST_RELAY_HOPS_A_ADMIN")"
tree_b_1="$(snapshot_tree "$TEST_RELAY_HOPS_B_ADMIN")"
tree_c_1="$(snapshot_tree "$TEST_RELAY_HOPS_C_ADMIN")"
sleep 2
tree_a_2="$(snapshot_tree "$TEST_RELAY_HOPS_A_ADMIN")"
tree_b_2="$(snapshot_tree "$TEST_RELAY_HOPS_B_ADMIN")"
tree_c_2="$(snapshot_tree "$TEST_RELAY_HOPS_C_ADMIN")"

if [[ "$tree_a_1" != "$tree_a_2" || "$tree_b_1" != "$tree_b_2" ||
      "$tree_c_1" != "$tree_c_2" ]]; then
  echo "ERROR: namespace state did not stabilize around the relay cycle" >&2
  exit 1
fi

echo "Relay-hop cycle reached every relay and stabilized."
