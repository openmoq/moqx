#!/usr/bin/env bash
# test_perf_bind.sh — the perf harnesses bind every relay stack to one address family.
#
# The publisher and the subscriber client reach the relay over IPv4 (127.0.0.1, or
# the relay host's first `hostname -I` address, unbracketed), so every listener the
# harnesses start should bind 0.0.0.0, the same family for every stack. Each case
# dry-runs a harness for one stack and reads the bind address it resolves:
# - scripts/perf/perf-test-ci.sh --dry-run, for mvfst and picoquic;
# - scripts/perf/perf-test.sh --dry-run (which shows scripts/moqx-run.sh --dry-run),
#   for mvfst, picoquic and the qmux listener;
# then all of them together must name one family.
#
# The binaries are stand-ins, and ssh and rsync are stubs on PATH: a dry run that
# runs any of them, or reaches a host, fails its case. The perf-test.sh dry runs
# keep their files under the test's own TMPDIR. Without envsubst or bash 4 the
# perf-test.sh cases are skipped and the test exits 77 unless another case fails.
#
# Usage: bash test/test_perf_bind.sh

set -euo pipefail

REPO="$(cd "$(dirname "$0")/.." && pwd)"
WANT="0.0.0.0"

TMPDIR_SCRIPT="$(mktemp -d)"
trap 'rm -rf "$TMPDIR_SCRIPT"' EXIT
OUT="$TMPDIR_SCRIPT/out"
CALLS="$TMPDIR_SCRIPT/calls"

# make_stub PATH: an executable that records its call in $CALLS and fails.
make_stub() {
  cat >"$1" <<EOF
#!/usr/bin/env bash
echo "\$0 \$*" >>"$CALLS"
exit 97
EOF
  chmod +x "$1"
}
RUN_TMP="$TMPDIR_SCRIPT/tmp"   # TMPDIR for the perf-test.sh dry runs
mkdir -p "$TMPDIR_SCRIPT/bin" "$TMPDIR_SCRIPT/moqbin" "$RUN_TMP"
for f in bin/ssh bin/rsync moqx moqbin/moqtest_server moqbin/moqperf_test_client; do
  make_stub "$TMPDIR_SCRIPT/$f"
done
export PATH="$TMPDIR_SCRIPT/bin:$PATH"

failures=0
fail() {
  echo "FAIL: $1" >&2
  failures=$((failures + 1))
}
fail_with_output() {
  fail "$1"
  if [[ -s "$OUT" ]]; then
    sed 's/^/  | /' "$OUT" >&2
  fi
}

family() {
  case "$1" in
    *:*) echo IPv6 ;;
    [0-9]*.*) echo IPv4 ;;
    *) echo "unknown ('$1')" ;;
  esac
}

LABELS=()
BINDS=()

# check LABEL STACK CMD...: run CMD, a harness dry run that should resolve STACK,
# and check the bind address it resolves; the address is kept for the family check.
# With RUN_DIR_IN set, the dry run must also print ("Logs: DIR") a run directory
# inside it, so it never shares a real run's directory.
RUN_DIR_IN=""
check() {
  local label="$1" stack="$2" status=0 bind log_dir
  shift 2
  : >"$CALLS"
  "$@" >"$OUT" 2>&1 || status=$?
  log_dir="$(sed -n 's/^Logs: //p' "$OUT" | head -n1 || true)"
  bind="$(grep -oE '(^|[[:space:]])bind=[^[:space:]]+' "$OUT" | head -n1 | sed 's/.*bind=//' || true)"
  LABELS+=("$label")
  BINDS+=("$bind")

  if [[ -s "$CALLS" ]]; then
    fail_with_output "$label: the dry run ran $(head -n1 "$CALLS")"
  elif ((status != 0)); then
    fail_with_output "$label: the dry run exited $status"
  elif [[ -n "$RUN_DIR_IN" && "$log_dir" != "$RUN_DIR_IN"/* ]]; then
    fail_with_output "$label: the dry run's run directory is '$log_dir', not inside \$TMPDIR ($RUN_DIR_IN)"
  elif ! grep -qE "(^|[[:space:]])stack=$stack([[:space:]]|$)" "$OUT"; then
    fail_with_output "$label: the dry run did not resolve stack=$stack"
  elif [[ "$bind" == "$WANT" ]]; then
    echo "PASS: $label binds $bind"
  else
    fail "$label binds '$bind', want $WANT ($(family "$WANT"), the clients' family)"
  fi
}

for stack in mvfst picoquic; do
  check "perf-test-ci.sh --quic-stack $stack" "$stack" \
    env PERF_RELAY_HOST=relay.invalid PERF_CLIENT_HOST=client.invalid \
    bash "$REPO/scripts/perf/perf-test-ci.sh" --dry-run --quic-stack "$stack" \
    --binary "$TMPDIR_SCRIPT/moqx" --moqbin "$TMPDIR_SCRIPT/moqbin" \
    --output "$TMPDIR_SCRIPT/perf-results.json"
done

# perf-test.sh's dry run runs scripts/moqx-run.sh, which needs envsubst and bash 4.
LOCAL_SKIP=""
if ((BASH_VERSINFO[0] < 4)); then
  LOCAL_SKIP="bash $BASH_VERSION is older than 4"
elif ! command -v envsubst >/dev/null 2>&1; then
  LOCAL_SKIP="envsubst (gettext) is not installed"
fi
if [[ -n "$LOCAL_SKIP" ]]; then
  echo "SKIP: perf-test.sh cases: $LOCAL_SKIP; scripts/moqx-run.sh needs envsubst and bash 4 or later" >&2
else
  LOCAL=(env TMPDIR="$RUN_TMP" bash "$REPO/scripts/perf/perf-test.sh"
    --dry-run --relay "$TMPDIR_SCRIPT/moqx" --moqbin "$TMPDIR_SCRIPT/moqbin")
  RUN_DIR_IN="$RUN_TMP"
  for stack in mvfst picoquic; do
    check "perf-test.sh --quic-stack $stack" "$stack" "${LOCAL[@]}" --quic-stack "$stack"
  done
  check "perf-test.sh --transport qmux" proxygen_qmux "${LOCAL[@]}" --transport qmux
  RUN_DIR_IN=""
fi

want_family="$(family "$WANT")"
summary=""
others=0
for i in "${!LABELS[@]}"; do
  [[ "$(family "${BINDS[$i]}")" == "$want_family" ]] || others=$((others + 1))
  summary+="${summary:+, }${LABELS[$i]}: '${BINDS[$i]}'"
done
if ((others == 0)); then
  echo "PASS: all ${#LABELS[@]} cases bind one family ($want_family)"
else
  fail "not every case binds $want_family, the clients' family: $summary"
fi

if ((failures > 0)); then
  exit 1
fi
if [[ -n "$LOCAL_SKIP" ]]; then
  exit 77   # ctest SKIP_RETURN_CODE: only perf-test-ci.sh was checked
fi
