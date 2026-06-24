#!/usr/bin/env bash
# Shared MoQT draft version(s) for shell integration tests.
#
# Pinning a single version forces the relay to exercise that draft instead of
# silently negotiating down to an older one if the newest fails. Emitted into
# generated relay configs as:  moqt_versions: ${MOQT_TEST_VERSIONS}
#
# Value is a YAML flow list, e.g. "[16]" or "[14, 16]".
#
# test_conformance.sh is intentionally exempt — it parameterizes the version
# from its CLI arg so it can run against every supported draft.
MOQT_TEST_VERSIONS="[16]"
