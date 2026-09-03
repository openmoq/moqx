#!/usr/bin/env bash
# test_sni.sh — end-to-end test of SNI-based multi-cert selection (tls.fizz.cert_dir).
#
# Setup: one moqx relay, quic_stack: proxygen_qmux (QMUX-on-TCP + Fizz),
# a cert_dir of two generated certs plus a cert_file/key_file fallback.
# Probe: `openssl s_client -servername` handshakes against the TCP port,
# asserting the served leaf CN:
#   - per-hostname certs for the two cert_dir identities (incl. a wildcard),
#   - the fallback cert for an unknown name,
#   - a cert added to the dir is served after the rescan interval.
#
# s_client speaks TLS-over-TCP, hence the qmux listener.
# The mvfst listener uses the same builder (src/tls/FizzContextBuilder.h).
#
# Usage: bash test/test_sni.sh [path/to/moqx]

set -euo pipefail

REPO="$(cd "$(dirname "$0")/.." && pwd)"
BINARY="${1:-}"
if [[ -z "$BINARY" ]]; then
  BINARY="$(ls -t "$REPO"/build/*/moqx 2>/dev/null | head -1 || true)"
  BINARY="${BINARY:-$REPO/build/default/moqx}"
fi
# shellcheck source=test_ports.sh
source "$REPO/test/test_ports.sh"
# shellcheck source=test_versions.sh
source "$REPO/test/test_versions.sh"

LISTEN_PORT=$TEST_SNI_LISTEN
ADMIN_PORT=$TEST_SNI_ADMIN
# Matches getAlpnFromVersion(<draft>, useStandard=true) for the pinned draft.
ALPN="moqt-$(tr -dc '0-9' <<<"$MOQT_TEST_VERSIONS" | head -c2)"

if [[ ! -x "$BINARY" ]]; then
  echo "ERROR: binary not found or not executable: $BINARY" >&2
  exit 1
fi

TMPDIR_SCRIPT="$(mktemp -d)"
CERT_DIR="$TMPDIR_SCRIPT/certs"
mkdir -p "$CERT_DIR"
MOQX_PID=""
cleanup() {
  # Guarded, not `[[ ... ]] && kill`: that list returns 1 with no PID, and
  # under `set -e` the trap would abort before the rm below.
  if [[ -n "$MOQX_PID" ]]; then
    kill "$MOQX_PID" 2>/dev/null || true
    wait "$MOQX_PID" 2>/dev/null || true
  fi
  rm -rf "$TMPDIR_SCRIPT"
}
trap cleanup EXIT

# $1 = base filename (in CERT_DIR unless absolute), $2 = CN, $3 = optional SAN list
make_cert() {
  local base="$1" cn="$2" sans="${3:-}"
  local dir="$CERT_DIR"
  [[ "$base" == /* ]] && dir="$(dirname "$base")" && base="$(basename "$base")"
  local args=(-x509 -newkey ec -pkeyopt ec_paramgen_curve:P-256 -nodes -days 1
              -subj "/CN=$cn" -keyout "$dir/$base.key" -out "$dir/$base.pem")
  [[ -n "$sans" ]] && args+=(-addext "subjectAltName=$sans")
  openssl req "${args[@]}" 2>/dev/null
}

make_cert a "a.example.com"
make_cert wild "*.wild.example.com" "DNS:*.wild.example.com"
mkdir -p "$TMPDIR_SCRIPT/fallback"
make_cert "$TMPDIR_SCRIPT/fallback/fb" "fallback.example.com"

RESCAN_INTERVAL=1
cat > "$TMPDIR_SCRIPT/config.yaml" <<EOF
listeners:
  - name: main
    udp:
      socket:
        address: "::"
        port: ${LISTEN_PORT}
    quic_stack: proxygen_qmux
    tls:
      insecure: false
      cert_file: ${TMPDIR_SCRIPT}/fallback/fb.pem
      key_file: ${TMPDIR_SCRIPT}/fallback/fb.key
      fizz:
        cert_dir: ${CERT_DIR}
        cert_reload_interval_s: ${RESCAN_INTERVAL}
    endpoint: "/moq-relay"
    moqt_versions: ${MOQT_TEST_VERSIONS}
services:
  default:
    match:
      - authority: {any: true}
        path: {prefix: "/"}
    cache:
      enabled: true
      max_tracks: 100
      max_groups_per_track: 3
admin:
  port: ${ADMIN_PORT}
  address: "::1"
  plaintext: true
EOF

"$BINARY" --config="$TMPDIR_SCRIPT/config.yaml" &
MOQX_PID=$!

# Admin readiness implies the listener is up.
deadline=$(( $(date +%s) + 5 ))
while ! curl --silent --fail --max-time 1 "http://[::1]:${ADMIN_PORT}/info" >/dev/null 2>&1; do
  if (( $(date +%s) >= deadline )); then
    echo "ERROR: moqx did not become ready in time" >&2
    exit 1
  fi
  sleep 0.1
done

# $1 = SNI to send ("" = none), prints the served leaf cert's subject line.
served_subject() {
  local sni="$1"
  local args=(-connect "localhost:${LISTEN_PORT}" -alpn "$ALPN")
  [[ -n "$sni" ]] && args+=(-servername "$sni") || args+=(-noservername)
  echo | openssl s_client "${args[@]}" 2>/dev/null | openssl x509 -noout -subject 2>/dev/null
}

check() {
  local label="$1" sni="$2" want_cn="$3"
  local subject
  subject="$(served_subject "$sni")"
  if [[ "$subject" != *"$want_cn"* ]]; then
    echo "FAIL [${label}]: SNI '${sni}' served '${subject}', expected CN ${want_cn}" >&2
    exit 1
  fi
  echo "PASS [${label}]"
}

check "exact match" "a.example.com" "a.example.com"
check "wildcard match" "x.wild.example.com" "*.wild.example.com"
check "unknown name -> fallback" "unknown.example.com" "fallback.example.com"
check "no SNI -> fallback" "" "fallback.example.com"

# A pair dropped into the dir is picked up by the background rescan.
# (No "before" assertion: the timer may fire between the write and a probe.)
make_cert late "late.example.com"
sleep $(( RESCAN_INTERVAL + 1 ))
check "after rescan" "late.example.com" "late.example.com"

echo "OK"
