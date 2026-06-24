/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "LoggingMultiFlag.h"

#include <list>
#include <string>

namespace openmoq::moqx {

namespace {

void joinNonEmpty(std::string& out, char sep, const std::vector<folly::StringPiece>& values) {
  for (auto v : values) {
    if (v.empty()) {
      continue;
    }
    if (!out.empty()) {
      out.push_back(sep);
    }
    out.append(v.begin(), v.end());
  }
}

} // namespace

std::string combineLoggingValues(
    const std::vector<folly::StringPiece>& categoryValues,
    const std::vector<folly::StringPiece>& handlerValues
) {
  std::string cats;
  joinNonEmpty(cats, ',', categoryValues);

  std::string handlers;
  joinNonEmpty(handlers, ';', handlerValues);

  if (handlers.empty()) {
    return cats;
  }
  std::string out;
  out.reserve(cats.size() + 1 + handlers.size());
  out.append(cats);
  out.push_back(';');
  out.append(handlers);
  return out;
}

namespace {

// Stable storage for the rewritten argv strings; never freed.
std::list<std::string>& argStash() {
  static std::list<std::string> instance;
  return instance;
}

struct Slot {
  int idx;        // argv slot to overwrite
  bool hasPrefix; // true when the slot is "--logging=X" / "--log_handler=X"
};

} // namespace

void combineLoggingArgs(int argc, char** argv) {
  constexpr folly::StringPiece kLogEq{"--logging="};
  constexpr folly::StringPiece kLog{"--logging"};
  constexpr folly::StringPiece kHandlerEq{"--log-handler="};
  constexpr folly::StringPiece kHandler{"--log-handler"};

  std::vector<Slot> slots;
  std::vector<folly::StringPiece> categoryValues;
  std::vector<folly::StringPiece> handlerValues;

  for (int i = 1; i < argc; ++i) {
    folly::StringPiece arg{argv[i]};
    if (arg.startsWith(kLogEq)) {
      slots.push_back({i, true});
      categoryValues.push_back(arg.subpiece(kLogEq.size()));
    } else if (arg == kLog && i + 1 < argc) {
      slots.push_back({i + 1, false});
      categoryValues.push_back(folly::StringPiece{argv[i + 1]});
      ++i;
    } else if (arg.startsWith(kHandlerEq)) {
      slots.push_back({i, true});
      handlerValues.push_back(arg.subpiece(kHandlerEq.size()));
    } else if (arg == kHandler && i + 1 < argc) {
      slots.push_back({i + 1, false});
      handlerValues.push_back(folly::StringPiece{argv[i + 1]});
      ++i;
    }
  }

  if (slots.empty()) {
    return;
  }
  // No combination work needed if we saw exactly one `--logging` and no
  // `--log_handler`: gflags already does the right thing.
  if (slots.size() == 1 && handlerValues.empty()) {
    return;
  }

  auto composite = combineLoggingValues(categoryValues, handlerValues);

  // Replace EVERY slot (including --log_handler slots) with the same
  // --logging=<composite>. gflags last-wins then picks the composite,
  // and folly never sees the unknown --log_handler flag because we've
  // rewritten its name slot too.
  for (const auto& slot : slots) {
    auto& stored = argStash().emplace_back();
    if (slot.hasPrefix) {
      stored.reserve(kLogEq.size() + composite.size());
      stored.append(kLogEq.data(), kLogEq.size());
    } else {
      stored.reserve(composite.size());
    }
    stored.append(composite);
    argv[slot.idx] = stored.data();
  }

  // For the separated form, we also need to fix the flag-name slots
  // (the one right before the value slot). The original argv had
  // `--log_handler X`; we need to turn that into `--logging <composite>`
  // so gflags sees a recognized flag name. For `--logging X`, the name
  // slot is already `--logging` and needs no change.
  for (int i = 1; i < argc; ++i) {
    folly::StringPiece arg{argv[i]};
    if (arg == kHandler && i + 1 < argc) {
      // The value slot at i+1 has already been overwritten above;
      // rename the flag slot itself.
      auto& stored = argStash().emplace_back();
      stored.append(kLog.data(), kLog.size());
      argv[i] = stored.data();
    }
  }
}

} // namespace openmoq::moqx
