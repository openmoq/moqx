#!/usr/bin/env bash
# test_validate_config.sh — `moqx validate-config` on the two forms of `listeners`.
#
# Runs on test/test.config.yaml (map form, warning-free) and the same config
# rewritten into the deprecated list form:
# - list form: valid, prints the deprecation warning
# - list form with --strict_config: rejected
# - map form with --strict_config: valid
#
# Usage: bash test/test_validate_config.sh [path/to/moqx]

set -euo pipefail

REPO="$(cd "$(dirname "$0")/.." && pwd)"
BINARY="${1:-$REPO/build/default/moqx}"
if [[ ! -x "$BINARY" ]]; then
  echo "ERROR: not found or not executable: $BINARY" >&2
  exit 1
fi

TMPDIR_SCRIPT="$(mktemp -d)"
trap 'rm -rf "$TMPDIR_SCRIPT"' EXIT
MAP_CFG="$REPO/test/test.config.yaml"
LIST_CFG="$TMPDIR_SCRIPT/list.yaml"
OUT="$TMPDIR_SCRIPT/out"
sed 's/^  main:$/  - name: main/' "$MAP_CFG" >"$LIST_CFG"
grep -q '^  - name: main$' "$LIST_CFG" || { echo "ERROR: list-form rewrite failed" >&2; exit 1; }

WARNING="listeners: the list form is deprecated"
failures=0
fail() {
  echo "FAIL: $1" >&2
  cat "$OUT" >&2
  failures=$((failures + 1))
}

if "$BINARY" validate-config --config="$LIST_CFG" >"$OUT" 2>&1; then
  grep -q "$WARNING" "$OUT" && echo "PASS: list form is valid and warns" ||
    fail "list form: no deprecation warning"
else
  fail "list form: rejected without --strict_config"
fi

if "$BINARY" validate-config --config="$LIST_CFG" --strict_config >"$OUT" 2>&1; then
  fail "list form: accepted with --strict_config"
else
  grep -q "$WARNING" "$OUT" && echo "PASS: list form is rejected with --strict_config" ||
    fail "list form with --strict_config: rejected, but not for the deprecation"
fi

if "$BINARY" validate-config --config="$MAP_CFG" --strict_config >"$OUT" 2>&1; then
  echo "PASS: map form is valid with --strict_config"
else
  fail "map form: rejected with --strict_config"
fi

exit $((failures > 0))
