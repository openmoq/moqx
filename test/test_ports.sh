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

# test_relay_chain.sh  (two relay instances)
TEST_RELAY_CHAIN_UPSTREAM=19668
TEST_RELAY_CHAIN_UPSTREAM_ADMIN=19669
TEST_RELAY_CHAIN_DOWNSTREAM=19670
TEST_RELAY_CHAIN_DOWNSTREAM_ADMIN=19671

# test_admin_cache_purge_race.sh
TEST_CACHE_PURGE_RACE_RELAY=19678
TEST_CACHE_PURGE_RACE_ADMIN=19679
