/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "LoggingMultiFlag.h"

#include <list>
#include <string>
#include <string_view>

namespace openmoq::moqx {

namespace {

void joinNonEmpty(std::string& out, char sep, const std::vector<std::string_view>& values) {
  for (auto v : values) {
    if (v.empty()) {
      continue;
    }
    if (!out.empty()) {
      out.push_back(sep);
    }
    out.append(v);
  }
}

} // namespace

std::string combineLoggingValues(
    const std::vector<std::string_view>& categoryValues,
    const std::vector<std::string_view>& handlerValues
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
  bool hasPrefix; // true when the slot is "--logging=X" / "--log-handler=X"
};

} // namespace

void combineLoggingArgs(int argc, char** argv) {
  constexpr std::string_view kLogEq{"--logging="};
  constexpr std::string_view kLog{"--logging"};
  constexpr std::string_view kHandlerEq{"--log-handler="};
  constexpr std::string_view kHandler{"--log-handler"};

  std::vector<Slot> slots;
  std::vector<std::string_view> categoryValues;
  std::vector<std::string_view> handlerValues;

  for (int i = 1; i < argc; ++i) {
    std::string_view arg{argv[i]};
    if (arg.starts_with(kLogEq)) {
      slots.push_back({i, true});
      categoryValues.push_back(arg.substr(kLogEq.size()));
    } else if (arg == kLog && i + 1 < argc) {
      slots.push_back({i + 1, false});
      categoryValues.push_back(std::string_view{argv[i + 1]});
      ++i;
    } else if (arg.starts_with(kHandlerEq)) {
      slots.push_back({i, true});
      handlerValues.push_back(arg.substr(kHandlerEq.size()));
    } else if (arg == kHandler && i + 1 < argc) {
      slots.push_back({i + 1, false});
      handlerValues.push_back(std::string_view{argv[i + 1]});
      ++i;
    }
  }

  if (slots.empty()) {
    return;
  }
  // gflags already does the right thing if we saw exactly one --logging
  // and no --log-handler.
  if (slots.size() == 1 && handlerValues.empty()) {
    return;
  }

  auto composite = combineLoggingValues(categoryValues, handlerValues);

  // Overwrite every collected slot with --logging=<composite>; gflags
  // last-wins picks the composite, and folly never sees --log-handler.
  for (const auto& slot : slots) {
    auto& stored = argStash().emplace_back();
    if (slot.hasPrefix) {
      stored.reserve(kLogEq.size() + composite.size());
      stored.append(kLogEq);
    } else {
      stored.reserve(composite.size());
    }
    stored.append(composite);
    argv[slot.idx] = stored.data();
  }

  // Separated form: rename `--log-handler` flag-name slots to `--logging`
  // so gflags recognizes them. (--logging X already has the right name.)
  for (int i = 1; i < argc; ++i) {
    std::string_view arg{argv[i]};
    if (arg == kHandler && i + 1 < argc) {
      auto& stored = argStash().emplace_back();
      stored.append(kLog);
      argv[i] = stored.data();
    }
  }
}

} // namespace openmoq::moqx
