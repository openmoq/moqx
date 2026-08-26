#!/usr/bin/env bash
# Shared process-reaping helpers for the relay integration tests.
# Source this file; it defines functions only.
#
# The Python harness reimplements this policy in Harness.cleanup()
# (test/lib/moq_harness.py); the two must stay in agreement.
#
# Callers pass "${ARRAY[@]:-}", which expands to a single empty string when the
# array is empty, so both helpers tolerate an empty argument.

# reap_helpers <pid>...
# Reaps non-relay helpers (dateserver, text clients). Their exit status is not
# meaningful: cleanup times them out and SIGKILLs the survivors.
reap_helpers() {
  wait "$@" 2>/dev/null || true
}

# reap_relays <pid>...
# Waits for each relay and reports any that failed. Relays must shut down
# cleanly: a non-zero exit means a crash, a hung teardown (watchdog _Exit(1)),
# or a sanitizer-detected leak (ASan flips the exit code to 1).
#
# Returns 1 if any relay failed rather than exiting, so the caller can finish
# its own cleanup first. Call it as `reap_relays ... || failed=1`; a bare call
# under `set -e` would abort the trap.
reap_relays() {
  local pid failed=0
  for pid in "$@"; do
    [[ -n "$pid" ]] || continue
    if ! wait "$pid"; then
      echo "FAIL: relay (pid $pid) exited non-zero — crash, hang, or sanitizer leak" >&2
      failed=1
    fi
  done
  return "$failed"
}
