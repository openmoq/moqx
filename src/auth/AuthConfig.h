/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "auth/Action.h"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace openmoq::moqx::auth {

// What the verifier checks tokens against; built from a cat4moq-auth entry.
struct AuthConfig {
  struct HmacKey {
    std::string id;
    std::string secret;
  };

  // A statically-granted scope applied to every request, regardless of
  // token.
  struct AnonymousScope {
    std::vector<Action> actions;
    // nullopt matches any namespace. An empty vector matches only the zero-segment namespace.
    std::optional<std::vector<std::string>> namespaceSegments;
    MatchRuleType namespaceMatchMode{MatchRuleType::Exact};
    std::optional<std::string> trackName; // nullopt = match any track
    MatchRuleType trackMatchMode{MatchRuleType::Exact};
  };

  uint64_t tokenType{0};
  std::vector<HmacKey> hmacKeys;
  bool requireSetupToken{true};
  bool allowRequestTokenOverride{true};
  bool strictClaims{false};
  std::vector<AnonymousScope> anonymousClaim; // empty = no anonymous access (default)
  uint32_t maxTokensPerMessage{0};
};

} // namespace openmoq::moqx::auth
