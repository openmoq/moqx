#!/usr/bin/env bash
#
# sync-relay.sh - Sync ORelay from deps/moxygen/moxygen/relay/
#
# Usage: scripts/sync-relay.sh [--no-build] [--no-test]
#
# Transforms:
#   MoQRelay.h            -> include/o_rly/ORelay.h
#   MoQRelay.cpp          -> src/ORelay.cpp
#   test/MoQRelayTest.cpp -> tests/ORelayTest.cpp
#
# Name/namespace mappings applied:
#   class MoQRelay           -> class ORelay
#   namespace moxygen        -> namespace openmoq::o_rly
#   unqualified moxygen types in header get moxygen:: prefix
#   using namespace moxygen; added before namespace in .cpp

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/.." && pwd)"
MOXYGEN_RELAY="${REPO_ROOT}/deps/moxygen/moxygen/relay"

NO_BUILD=false
NO_TEST=false
for arg in "$@"; do
  case "$arg" in
    --no-build) NO_BUILD=true ;;
    --no-test)  NO_TEST=true ;;
    *) echo "Unknown argument: $arg" >&2; exit 1 ;;
  esac
done

TMP_DIR="$(mktemp -d)"
trap 'rm -rf "${TMP_DIR}"' EXIT

MOXYGEN_REV="$(git -C "${REPO_ROOT}/deps/moxygen" rev-parse --short HEAD 2>/dev/null || echo unknown)"
echo "==> Syncing relay files from moxygen @ ${MOXYGEN_REV}"

# ─────────────────────────────────────────────────────────────────────────────
# Helper: replace Meta-only Apache 2.0 copyright with combined OpenMOQ header
# ─────────────────────────────────────────────────────────────────────────────
replace_copyright() {
  local file="$1"
  python3 - "$file" <<'PYEOF'
import re, sys
path = sys.argv[1]
with open(path) as f:
    content = f.read()
new_header = """\
/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * Originally from github.com/facebookexperimental/moxygen.
 * See deps/moxygen/LICENSE for the original license terms.
 *
 * Copyright (c) OpenMOQ contributors.
 */"""
content = re.sub(
    r'/\*.*?This source code is licensed under.*?\*/',
    new_header, content, count=1, flags=re.DOTALL)
with open(path, 'w') as f:
    f.write(content)
PYEOF
}

# ─────────────────────────────────────────────────────────────────────────────
# Helper: add moxygen:: qualifier to unqualified moxygen types
#
# Called only on the header, where we're now in namespace openmoq::o_rly and
# moxygen types are no longer implicitly visible.  Uses negative lookbehind so
# already-qualified occurrences (e.g. moxygen::Publisher) are left alone.
# ─────────────────────────────────────────────────────────────────────────────
qualify_moxygen_types() {
  local file="$1"
  perl -pi -e '
    next if /^#include/;
    s/(?<!moxygen::)\bPublisher\b/moxygen::Publisher/g;
    s/(?<!moxygen::)\bSubscriber\b/moxygen::Subscriber/g;
    s/(?<!moxygen::)\bMoQForwarder\b/moxygen::MoQForwarder/g;
    s/(?<!moxygen::)\bMoQSession\b/moxygen::MoQSession/g;
    s/(?<!moxygen::)\bMoQCache\b/moxygen::MoQCache/g;
    s/(?<!moxygen::)\bTrackNamespace\b/moxygen::TrackNamespace/g;
    s/(?<!moxygen::)\bTrackConsumer\b/moxygen::TrackConsumer/g;
    s/(?<!moxygen::)\bFetchConsumer\b/moxygen::FetchConsumer/g;
    s/(?<!moxygen::)\bFullTrackName\b/moxygen::FullTrackName/g;
    s/(?<!moxygen::)\bSubscribeRequest\b/moxygen::SubscribeRequest/g;
    s/(?<!moxygen::)\bSubscribeNamespaceOptions\b/moxygen::SubscribeNamespaceOptions/g;
    s/(?<!moxygen::)\bSubscribeNamespace\b/moxygen::SubscribeNamespace/g;
    s/(?<!moxygen::)\bPublishNamespace\b/moxygen::PublishNamespace/g;
    s/(?<!moxygen::)\bPublishRequest\b/moxygen::PublishRequest/g;
    s/(?<!moxygen::)\bGoaway\b/moxygen::Goaway/g;
    s/(?<!moxygen::)\bTrackStatus\b/moxygen::TrackStatus/g;
    s/(?<!moxygen::)\bRequestUpdate\b/moxygen::RequestUpdate/g;
    s/(?<!moxygen::)\bRequestErrorCode\b/moxygen::RequestErrorCode/g;
    s/(?<!moxygen::)\bRequestError\b/moxygen::RequestError/g;
    s/(?<!moxygen::)\bRequestID\b/moxygen::RequestID/g;
    s/(?<!moxygen::)\bFetch\b/moxygen::Fetch/g;
    s/(?<!moxygen::)\bkDefaultMaxCachedTracks\b/moxygen::kDefaultMaxCachedTracks/g;
    s/(?<!moxygen::)\bkDefaultMaxCachedGroupsPerTrack\b/moxygen::kDefaultMaxCachedGroupsPerTrack/g;
  ' "$file"
}

# ─────────────────────────────────────────────────────────────────────────────
# 1. MoQRelay.h → include/o_rly/ORelay.h
# ─────────────────────────────────────────────────────────────────────────────
process_header() {
  local src="${MOXYGEN_RELAY}/MoQRelay.h"
  local dst="${REPO_ROOT}/include/o_rly/ORelay.h"
  local tmp="${TMP_DIR}/ORelay.h"

  echo "  MoQRelay.h -> include/o_rly/ORelay.h"
  cp "$src" "$tmp"

  replace_copyright "$tmp"

  # Include style: "moxygen/relay/MoQRelay.h" -> <o_rly/ORelay.h>; other "..." -> <...>
  sed -i 's|#include "moxygen/relay/MoQRelay\.h"|#include <o_rly/ORelay.h>|g' "$tmp"
  sed -i 's|#include "\(moxygen/[^"]*\)"|#include <\1>|g' "$tmp"

  # Class rename
  sed -i 's/\bMoQRelay\b/ORelay/g' "$tmp"

  # Namespace
  sed -i 's/^namespace moxygen {$/namespace openmoq::o_rly {/' "$tmp"
  sed -i 's|^} // namespace moxygen$|} // namespace openmoq::o_rly|' "$tmp"

  # Qualify unqualified moxygen types (order matters: more specific before less specific)
  qualify_moxygen_types "$tmp"

  cp "$tmp" "$dst"
}

# ─────────────────────────────────────────────────────────────────────────────
# 2. MoQRelay.cpp → src/ORelay.cpp
# ─────────────────────────────────────────────────────────────────────────────
process_source() {
  local src="${MOXYGEN_RELAY}/MoQRelay.cpp"
  local dst="${REPO_ROOT}/src/ORelay.cpp"
  local tmp="${TMP_DIR}/ORelay.cpp"

  echo "  MoQRelay.cpp -> src/ORelay.cpp"
  cp "$src" "$tmp"

  replace_copyright "$tmp"

  # Include style: relay header -> ORelay.h; other "..." -> <...>
  sed -i 's|#include "moxygen/relay/MoQRelay\.h"|#include <o_rly/ORelay.h>|g' "$tmp"
  sed -i 's|#include "\(moxygen/[^"]*\)"|#include <\1>|g' "$tmp"

  # Class/method references
  sed -i 's/\bMoQRelay\b/ORelay/g' "$tmp"

  # Namespace: insert "using namespace moxygen;" before the namespace block
  # so types in the implementation don't need moxygen:: qualification
  sed -i 's/^namespace moxygen {$/using namespace moxygen;\n\nnamespace openmoq::o_rly {/' "$tmp"
  sed -i 's|^} // namespace moxygen$|} // namespace openmoq::o_rly|' "$tmp"

  cp "$tmp" "$dst"
}

# ─────────────────────────────────────────────────────────────────────────────
# 3. test/MoQRelayTest.cpp → tests/ORelayTest.cpp
# ─────────────────────────────────────────────────────────────────────────────
process_test() {
  local src="${MOXYGEN_RELAY}/test/MoQRelayTest.cpp"
  local dst="${REPO_ROOT}/tests/ORelayTest.cpp"
  local tmp="${TMP_DIR}/ORelayTest.cpp"

  echo "  test/MoQRelayTest.cpp -> tests/ORelayTest.cpp"
  cp "$src" "$tmp"

  replace_copyright "$tmp"

  # Replace MoQRelay include with ORelay
  sed -i 's|#include <moxygen/relay/MoQRelay\.h>|#include <o_rly/ORelay.h>|g' "$tmp"

  # Relay class under test
  sed -i 's/\bMoQRelay\b/ORelay/g' "$tmp"

  # Add openmoq::o_rly namespace (and moxygen for types used at file scope).
  # The test lives in namespace moxygen::test, but globals defined before that
  # namespace block need explicit usings.
  sed -i 's/^using namespace testing;$/using namespace testing;\nusing namespace moxygen;\nusing namespace openmoq::o_rly;/' "$tmp"

  cp "$tmp" "$dst"
}

# ─────────────────────────────────────────────────────────────────────────────
# Run transformations
# ─────────────────────────────────────────────────────────────────────────────
echo "--> Transforming files"
process_header
process_source
process_test

# ─────────────────────────────────────────────────────────────────────────────
# Format
# ─────────────────────────────────────────────────────────────────────────────
echo "--> Formatting"
"${SCRIPT_DIR}/format.sh"

# ─────────────────────────────────────────────────────────────────────────────
# Build
# ─────────────────────────────────────────────────────────────────────────────
if ! ${NO_BUILD}; then
  echo "--> Building"
  "${SCRIPT_DIR}/build.sh"
fi

# ─────────────────────────────────────────────────────────────────────────────
# Test
# ─────────────────────────────────────────────────────────────────────────────
if ! ${NO_TEST}; then
  echo "--> Testing"
  "${SCRIPT_DIR}/test.sh"
fi

echo "==> Done"
