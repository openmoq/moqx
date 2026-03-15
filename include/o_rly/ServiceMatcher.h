#pragma once

#include <o_rly/config/config.h>

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <folly/container/F14Map.h>

namespace openmoq::o_rly {

class ServiceMatcher {
public:
  explicit ServiceMatcher(const std::vector<config::ServiceConfig>& services);

  // Returns service index for (authority, path) pair, or nullopt.
  // Priority: exact authority > wildcard authority > any authority.
  // Within each authority tier: exact path > longest prefix > any path.
  std::optional<size_t> match(std::string_view authority, std::string_view path) const;

private:
  struct PathRuleSet {
    folly::F14FastMap<std::string, size_t> exactPaths; // O(1) exact path

    struct PrefixRule {
      std::string prefix;
      size_t serviceIndex;
    };
    std::vector<PrefixRule> prefixPaths; // sorted descending by length

    void addRule(const config::ServiceConfig::MatchEntry::PathMatcher& path, size_t serviceIndex);
    void finalize(); // sorts prefixPaths
    std::optional<size_t> match(std::string_view path) const;
  };

  folly::F14FastMap<std::string, PathRuleSet> exactAuthorityRules_;
  folly::F14FastMap<std::string, PathRuleSet> wildcardAuthorityRules_;
  PathRuleSet anyAuthorityRules_;
};

} // namespace openmoq::o_rly
