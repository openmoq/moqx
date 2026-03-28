#pragma once

#include <moqx/config/config.h>

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <folly/container/F14Map.h>

namespace openmoq::moqx {

class ServiceMatcher {
public:
  explicit ServiceMatcher(const folly::F14FastMap<std::string, config::ServiceConfig>& services);

  // Returns service name for (authority, path) pair, or nullopt.
  // Priority: exact authority > wildcard authority > any authority.
  // Within each authority tier: exact path > longest prefix > any path.
  std::optional<std::string_view> match(std::string_view authority, std::string_view path) const;

private:
  struct PathRuleSet {
    folly::F14FastMap<std::string, std::string> exactPaths; // path → service name

    struct PrefixRule {
      std::string prefix;
      std::string serviceName;
    };
    std::vector<PrefixRule> prefixPaths; // sorted descending by length

    void addRule(
        const config::ServiceConfig::MatchEntry::PathMatcher& path,
        const std::string& serviceName
    );
    void finalize(); // sorts prefixPaths
    std::optional<std::string_view> match(std::string_view path) const;
  };

  folly::F14FastMap<std::string, PathRuleSet> exactAuthorityRules_;
  folly::F14FastMap<std::string, PathRuleSet> wildcardAuthorityRules_;
  PathRuleSet anyAuthorityRules_;
};

} // namespace openmoq::moqx
