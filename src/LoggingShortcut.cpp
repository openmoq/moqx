/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "LoggingShortcut.h"

#include <folly/String.h>

#include <list>
#include <string>
#include <string_view>

namespace openmoq::moqx {

std::string normalizeLoggingConfig(folly::StringPiece raw) {
  std::string out;
  out.reserve(raw.size() + 4);

  size_t pos = 0;
  while (pos < raw.size()) {
    size_t end = raw.find_first_of(";,", pos);
    if (end == folly::StringPiece::npos) {
      end = raw.size();
    }
    auto rawClause = raw.subpiece(pos, end - pos);
    auto trimmed = folly::trimWhitespace(rawClause);
    if (!trimmed.empty() && !trimmed.contains('=')) {
      // Bare level → rewrite to ".=<level>", dropping any whitespace the
      // operator wrote around the token.
      out.append(".=");
      out.append(trimmed.begin(), trimmed.end());
    } else {
      // Already a full spec (or empty) → pass through verbatim so we don't
      // mangle whitespace the operator deliberately put around `;` or `,`.
      out.append(rawClause.begin(), rawClause.end());
    }
    if (end < raw.size()) {
      out.push_back(raw[end]);
    }
    pos = end + 1;
  }
  return out;
}

namespace {

// Holds the rewritten argv strings for the lifetime of the process.
// std::list (not vector) so that emplace_back never invalidates the
// pointer we hand back to argv.
std::list<std::string>& argStash() {
  static std::list<std::string> instance;
  return instance;
}

// If `value` needs rewriting, stash `prefix + normalized` and re-point
// `slot` at the stable storage. Returns true if a rewrite happened.
bool maybeStash(char*& slot, folly::StringPiece value, folly::StringPiece prefix) {
  auto normalized = normalizeLoggingConfig(value);
  if (folly::StringPiece(normalized) == value) {
    return false;
  }
  auto& stored = argStash().emplace_back();
  stored.reserve(prefix.size() + normalized.size());
  stored.append(prefix.data(), prefix.size());
  stored.append(normalized);
  slot = stored.data();
  return true;
}

} // namespace

void normalizeLoggingArgv(int argc, char** argv) {
  constexpr folly::StringPiece kLongEq{"--logging="};
  constexpr folly::StringPiece kLong{"--logging"};

  for (int i = 1; i < argc; ++i) {
    folly::StringPiece arg{argv[i]};
    if (arg.startsWith(kLongEq)) {
      maybeStash(argv[i], arg.subpiece(kLongEq.size()), kLongEq);
    } else if (arg == kLong && i + 1 < argc) {
      maybeStash(argv[i + 1], folly::StringPiece(argv[i + 1]), folly::StringPiece{});
    }
  }
}

} // namespace openmoq::moqx
