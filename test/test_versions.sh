#!/usr/bin/env bash
# Shared MoQT draft versions for shell integration tests.
#
# The relay accepts every draft under test. MOQT_TEST_VERSIONS is a YAML flow
# list, emitted into generated relay configs as:  moqt_versions: ${...}
#
# Each client pins one draft with --versions=${MOQT_CLIENT_VERSION}, so it
# cannot negotiate down to an older draft. The per-draft ctest variants set
# MOQ_HARNESS_MOQT_VERSION to override it. Relay-to-relay peering is always
# draft 16.
#
# test_conformance.sh is intentionally exempt. It parameterizes the version
# from its CLI arg so it can run against every supported draft.
MOQT_TEST_VERSIONS="[16, 18]"
MOQT_CLIENT_VERSION=16
# One line, so test/lib/shellvars.py does not read the override as the default.
if [[ -n "${MOQ_HARNESS_MOQT_VERSION:-}" ]]; then MOQT_CLIENT_VERSION=$MOQ_HARNESS_MOQT_VERSION; fi
