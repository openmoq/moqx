#!/usr/bin/env bash
# Central port registry for shell integration tests.
#
# Every test that binds a port must claim it here so tests can run in
# parallel without conflicts.  Ports are assigned in sequential pairs
# (UDP listener + admin HTTP).
#
# To add a new test:
#   1. Append a new block below, incrementing from the highest port used.
#   2. Source this file in your test: source "$(dirname "$0")/test_ports.sh"
#   3. Use the exported variables for your LISTEN_PORT / ADMIN_PORT.
#
# The Python harness reads this file too (test/lib/shellvars.py), so keep every
# entry a plain NAME=value assignment — no expansion, no command substitution.
#
# To verify no duplicate ports exist:
#   grep -Eo '[0-9]{4,5}' test/test_ports.sh | sort | uniq -d
#   (should print nothing)

# test_admin_info.sh
TEST_ADMIN_INFO_LISTEN=9660
TEST_ADMIN_INFO_ADMIN=9661

# test_admin_metrics.sh
TEST_ADMIN_METRICS_LISTEN=9662
TEST_ADMIN_METRICS_ADMIN=9663

# test_admin_tls.sh
TEST_ADMIN_TLS_LISTEN=9664
TEST_ADMIN_TLS_ADMIN=9665

# test_admin_cache_purge.sh
TEST_CACHE_PURGE_LISTEN=9666
TEST_CACHE_PURGE_ADMIN=9667

# test_admin_config.sh
TEST_ADMIN_CONFIG_LISTEN=9668
TEST_ADMIN_CONFIG_ADMIN=9669

# test_admin_connection_logs.sh
TEST_ADMIN_CONNECTION_LOGS_LISTEN=9670
TEST_ADMIN_CONNECTION_LOGS_ADMIN=9671

# The ten ports between the block above and the one below are free: they held
# test_relay_chain.sh and test_relay_hops_cycle.sh, which now allocate from the
# harness bases at the end of this file. Claim them before extending the range.

# test_admin_cache_purge_race.sh
TEST_CACHE_PURGE_RACE_RELAY=19678
TEST_CACHE_PURGE_RACE_ADMIN=19679

# test_qmux_relay.sh  (relay listener + admin, plus a throwaway dateserver listener)
TEST_QMUX_RELAY_LISTEN=19680
TEST_QMUX_RELAY_ADMIN=19681
TEST_QMUX_DATESERVER_LISTEN=19682

# test_admin_track_metrics.sh
TEST_ADMIN_TRACK_METRICS_LISTEN=19684
TEST_ADMIN_TRACK_METRICS_ADMIN=19685

# test_admin_state.sh
TEST_ADMIN_STATE_LISTEN=19686
TEST_ADMIN_STATE_ADMIN=19687

# test_conformance.sh does NOT source this file — it self-assigns a random relay
# port inside the span below (base + RANDOM % 100, admin one above), so the
# whole span is spoken for and nothing here may allocate inside it. Recorded as
# variables rather than prose so the range is visible to the duplicate check.
TEST_CONFORMANCE_PORT_MIN=19700
TEST_CONFORMANCE_PORT_MAX=19799

# test/lib/moq_harness.py — each harness test gets its own base, and each
# h.relay() call takes the next listener/admin pair counting up from it.
#
# These MUST be distinct per test: scripts/test.sh runs ctest --parallel, so
# harness tests execute concurrently. Sharing a base makes them fight over the
# same ports, and a relay can even pass its readiness check against another
# test's relay listening on the same admin port.
#
# Placed above the conformance span, not inside it. Ten ports per test = five
# relay pairs. Set HARNESS_BASE_PORT to override.
# Do not write literal port numbers in comments here: the duplicate check
# greps every number in this file and cannot tell a comment from an assignment.
TEST_HARNESS_CHAIN_BASE=19800
TEST_HARNESS_HOPS_BASE=19810
TEST_HARNESS_PYRAMID_BASE=19820
# Reserved for future harness tests through: 19859
