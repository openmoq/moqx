#!/usr/bin/env bash
# Resolve MOQBIN — the moxygen install's bin/ (moqtest_client, moqdateserver, …)
# — for a shell test.
#
#   source "$(dirname "$0")/test_moqbin.sh"
#   resolve_moqbin "$BINARY"        # $BINARY = the moqx binary under test
#
# An MOQBIN already in the environment wins (ctest sets it per test); otherwise
# it comes from the tool-paths file the configure writes next to the binary.
# Always leaves MOQBIN set — possibly empty — so callers report a clear
# not-found error instead of aborting under `set -u`.
resolve_moqbin() {
  local build_dir
  build_dir="$(dirname "${1:-}")"
  if [[ -z "${MOQBIN:-}" && -f "$build_dir/moqx-tools.env" ]]; then
    # shellcheck disable=SC1091  # generated at configure time
    source "$build_dir/moqx-tools.env"
  fi
  MOQBIN="${MOQBIN:-}"
}
