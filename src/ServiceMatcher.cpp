#include "ServiceMatcher.h"

#include <algorithm>
#include <unordered_set>

#include <folly/logging/xlog.h>

namespace openmoq::moqx {

void ServiceMatcher::PathRuleSet::addRule(
    const config::ServiceConfig::MatchEntry::PathMatcher& path,
    const std::string& serviceName
) {
  if (std::holds_alternative<config::ServiceConfig::MatchEntry::ExactPath>(path)) {
    const auto& val = std::get<config::ServiceConfig::MatchEntry::ExactPath>(path).value;
    exactPaths.try_emplace(val, serviceName);
  } else {
    const auto& val = std::get<config::ServiceConfig::MatchEntry::PrefixPath>(path).value;
    prefixPaths.push_back({val, serviceName});
  }
}

void ServiceMatcher::PathRuleSet::finalize() {
  std::sort(prefixPaths.begin(), prefixPaths.end(), [](const auto& a, const auto& b) {
    return a.prefix.size() > b.prefix.size();
  });
}

std::optional<std::string_view> ServiceMatcher::PathRuleSet::match(std::string_view path) const {
  // O(1) exact path lookup
  auto it = exactPaths.find(path);
  if (it != exactPaths.end()) {
    return it->second;
  }

  // Linear scan prefix paths (sorted longest first)
  for (const auto& rule : prefixPaths) {
    if (path.size() >= rule.prefix.size() && path.substr(0, rule.prefix.size()) == rule.prefix) {
      return rule.serviceName;
    }
  }

  return std::nullopt;
}

ServiceMatcher::ServiceMatcher(const folly::F14FastMap<std::string, config::ServiceConfig>& services
) {
  for (const auto& [name, svc] : services) {
    for (const auto& entry : svc.match) {
      std::visit(
          [&](const auto& auth) {
            using A = std::decay_t<decltype(auth)>;
            if constexpr (std::is_same_v<A, config::ServiceConfig::MatchEntry::ExactAuthority>) {
              exactAuthorityRules_[auth.value].addRule(entry.path, name);
            } else if constexpr (std::is_same_v<
                                     A,
                                     config::ServiceConfig::MatchEntry::WildcardAuthority>) {
              // "*.x.com" → ".x.com"
              auto suffix = auth.pattern.substr(1);
              wildcardAuthorityRules_[suffix].addRule(entry.path, name);
            } else {
              // AnyAuthority
              anyAuthorityRules_.addRule(entry.path, name);
            }
          },
          entry.authority
      );
    }
  }

  // Finalize all PathRuleSets
  for (auto& [_, rules] : exactAuthorityRules_) {
    rules.finalize();
  }
  for (auto& [_, rules] : wildcardAuthorityRules_) {
    rules.finalize();
  }
  anyAuthorityRules_.finalize();
}

std::optional<std::string_view>
ServiceMatcher::match(std::string_view authority, std::string_view path) const {
  // MOQT spec allows empty path in CLIENT_SETUP; normalize to "/" so
  // PrefixPath{"/"} always matches.
  std::string_view effectivePath = path;
  if (path.empty()) {
    XLOG(WARN) << "Empty path in CLIENT_SETUP for authority '" << authority
               << "', normalizing to '/'";
    effectivePath = "/";
  }

  // 1. Exact authority match (O(1) hash lookup)
  auto it = exactAuthorityRules_.find(authority);
  if (it != exactAuthorityRules_.end()) {
    auto result = it->second.match(effectivePath);
    if (result.has_value()) {
      return result;
    }
  }

  // 2. Wildcard match (O(1) hash lookup on suffix)
  // For "foo.example.com", find first '.' → extract ".example.com" → hash lookup.
  // Single-label constraint is automatically satisfied by this approach.
  auto dot = authority.find('.');
  if (dot != std::string_view::npos) {
    auto suffix = authority.substr(dot); // ".example.com"
    auto wit = wildcardAuthorityRules_.find(suffix);
    if (wit != wildcardAuthorityRules_.end()) {
      auto result = wit->second.match(effectivePath);
      if (result.has_value()) {
        return result;
      }
    }
  }

  // 3. Any-authority fallback (path matching still applies)
  return anyAuthorityRules_.match(effectivePath);
}

std::vector<std::string> ServiceMatcher::allExactPaths() const {
  std::unordered_set<std::string> seen;
  std::vector<std::string> result;
  auto collect = [&](const PathRuleSet& rs) {
    for (const auto& [path, _] : rs.exactPaths) {
      if (seen.insert(path).second) {
        result.push_back(path);
      }
    }
  };
  for (const auto& [_, rs] : exactAuthorityRules_) {
    collect(rs);
  }
  for (const auto& [_, rs] : wildcardAuthorityRules_) {
    collect(rs);
  }
  collect(anyAuthorityRules_);
  return result;
}

} // namespace openmoq::moqx
