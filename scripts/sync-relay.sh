#!/usr/bin/env bash
#
# sync-relay.sh - Sync MoqxRelay from deps/moxygen/moxygen/relay/
#
# Usage: scripts/sync-relay.sh [--no-build] [--no-test]
#
# Transforms:
#   MoQRelay.h            -> include/moqx/MoqxRelay.h
#   MoQRelay.cpp          -> src/MoqxRelay.cpp
#   test/MoQRelayTest.cpp -> test/MoqxRelayTest.cpp
#
# Name/namespace mappings applied:
#   class MoQRelay           -> class MoqxRelay
#   namespace moxygen        -> namespace openmoq::moqx
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
# Called only on the header, where we're now in namespace openmoq::moqx and
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
# 1. MoQRelay.h → include/moqx/MoqxRelay.h
# ─────────────────────────────────────────────────────────────────────────────
process_header() {
  local src="${MOXYGEN_RELAY}/MoQRelay.h"
  local dst="${REPO_ROOT}/include/moqx/MoqxRelay.h"
  local tmp="${TMP_DIR}/MoqxRelay.h"

  echo "  MoQRelay.h -> include/moqx/MoqxRelay.h"
  cp "$src" "$tmp"

  replace_copyright "$tmp"

  # Include style: "moxygen/relay/MoQRelay.h" -> <moqx/MoqxRelay.h>; other "..." -> <...>
  sed -i 's|#include "moxygen/relay/MoQRelay\.h"|#include <moqx/MoqxRelay.h>|g' "$tmp"
  sed -i 's|#include "\(moxygen/[^"]*\)"|#include <\1>|g' "$tmp"

  # Class rename
  sed -i 's/\bMoQRelay\b/MoqxRelay/g' "$tmp"

  # Namespace
  sed -i 's/^namespace moxygen {$/namespace openmoq::moqx {/' "$tmp"
  sed -i 's|^} // namespace moxygen$|} // namespace openmoq::moqx|' "$tmp"

  # Qualify unqualified moxygen types (order matters: more specific before less specific)
  qualify_moxygen_types "$tmp"

  cp "$tmp" "$dst"
}

# ─────────────────────────────────────────────────────────────────────────────
# 2. MoQRelay.cpp → src/MoqxRelay.cpp
# ─────────────────────────────────────────────────────────────────────────────
process_source() {
  local src="${MOXYGEN_RELAY}/MoQRelay.cpp"
  local dst="${REPO_ROOT}/src/MoqxRelay.cpp"
  local tmp="${TMP_DIR}/MoqxRelay.cpp"

  echo "  MoQRelay.cpp -> src/MoqxRelay.cpp"
  cp "$src" "$tmp"

  replace_copyright "$tmp"

  # Include style: relay header -> MoqxRelay.h; other "..." -> <...>
  sed -i 's|#include "moxygen/relay/MoQRelay\.h"|#include <moqx/MoqxRelay.h>|g' "$tmp"
  sed -i 's|#include "\(moxygen/[^"]*\)"|#include <\1>|g' "$tmp"

  # Class/method references
  sed -i 's/\bMoQRelay\b/MoqxRelay/g' "$tmp"

  # Namespace: insert "using namespace moxygen;" before the namespace block
  # so types in the implementation don't need moxygen:: qualification
  sed -i 's/^namespace moxygen {$/using namespace moxygen;\n\nnamespace openmoq::moqx {/' "$tmp"
  sed -i 's|^} // namespace moxygen$|} // namespace openmoq::moqx|' "$tmp"

  cp "$tmp" "$dst"
}

# ─────────────────────────────────────────────────────────────────────────────
# 3. test/MoQRelayTest.cpp → tests/MoqxRelayTest.cpp
# ─────────────────────────────────────────────────────────────────────────────
process_test() {
  local src="${MOXYGEN_RELAY}/test/MoQRelayTest.cpp"
  local dst="${REPO_ROOT}/test/MoqxRelayTest.cpp"
  local tmp="${TMP_DIR}/MoqxRelayTest.cpp"

  echo "  test/MoQRelayTest.cpp -> test/MoqxRelayTest.cpp"
  cp "$src" "$tmp"

  replace_copyright "$tmp"

  # Replace MoQRelay include with MoqxRelay
  sed -i 's|#include <moxygen/relay/MoQRelay\.h>|#include <moqx/MoqxRelay.h>|g' "$tmp"

  # Use moqx's own TestUtils instead of moxygen's
  sed -i 's|#include <moxygen/test/TestUtils\.h>|#include "TestUtils.h"|g' "$tmp"

  # Relay class under test
  sed -i 's/\bMoQRelay\b/MoqxRelay/g' "$tmp"

  # Add openmoq::moqx namespace (and moxygen for types used at file scope).
  # The test lives in namespace moxygen::test, but globals defined before that
  # namespace block need explicit usings.
  sed -i 's/^using namespace testing;$/using namespace testing;\nusing namespace moxygen;\nusing namespace openmoq::moqx;/' "$tmp"

  # Bring makeBuf into scope inside namespace moxygen::test
  sed -i 's/^namespace moxygen::test {$/namespace moxygen::test {\n\nusing openmoq::moqx::test::makeBuf;/' "$tmp"

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
