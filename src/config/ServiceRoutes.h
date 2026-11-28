/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <map>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>
#include <vector>

#include "config/Config.h"

namespace openmoq::moqx::config {

// (listener, rule key) -> the service that holds the rule on that listener.
using RuleOwners = std::map<std::pair<std::string, std::string>, std::string>;

// Registers `service`'s claim on `ruleKey` for each of its listeners. Returns the services that
// already held the rule, mapped to the listeners both are bound to. Two services only conflict on
// a listener they share. Takes keys rather than config types so the resolver can call it during
// validation, before a ServiceConfig exists
inline std::map<std::string, std::vector<std::string>> claimRule(
    RuleOwners& owners,
    const std::string& service,
    const std::vector<std::string>& listeners,
    const std::string& ruleKey
) {
  std::map<std::string, std::vector<std::string>> clashes;
  for (const auto& listener : listeners) {
    auto [it, inserted] = owners.try_emplace({listener, ruleKey}, service);
    if (!inserted) {
      clashes[it->second].push_back(listener);
    }
  }
  return clashes;
}

// Identity of a match entry's (authority, path) pair, for conflict detection.
inline std::string ruleKey(const ServiceConfig::MatchEntry& entry) {
  using ME = ServiceConfig::MatchEntry;
  std::string key = std::visit(
      [](const auto& a) -> std::string {
        using A = std::decay_t<decltype(a)>;
        if constexpr (std::is_same_v<A, ME::ExactAuthority>) {
          return "exact:" + a.value;
        } else if constexpr (std::is_same_v<A, ME::WildcardAuthority>) {
          return "wildcard:" + a.pattern;
        } else {
          return "any:";
        }
      },
      entry.authority
  );
  key += '|';
  key += std::visit(
      [](const auto& p) {
        using P = std::decay_t<decltype(p)>;
        return std::string(std::is_same_v<P, ME::ExactPath> ? "exact:" : "prefix:") + p.value;
      },
      entry.path
  );
  return key;
}

} // namespace openmoq::moqx::config
