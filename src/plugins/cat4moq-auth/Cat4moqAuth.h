/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

// cat4moq-auth: CAT4MOQ token auth as a service-scope plugin.
// - admit verifies the setup tokens; their grants become the session state.
// - authorize checks each request against request tokens, session grants and
//   the anonymous claim.
// - Config reference: docs/config.md#cat4moq-auth.

#include "auth/AuthConfig.h"
#include "config/Config.h"
#include "plugin/PluginType.h"

#include <folly/Expected.h>
#include <rfl.hpp>

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace openmoq::moqx::plugins::cat4moq {

inline constexpr std::string_view kType = "cat4moq-auth";

struct Config {
  struct HmacKey {
    std::string id;
    std::string secret;
  };

  // A statically-granted scope applied to every request, regardless of token.
  struct AnonymousScope {
    struct ExactNamespace {
      std::vector<std::string> exact;
    };
    struct PrefixNamespace {
      std::vector<std::string> prefix;
    };
    struct SuffixNamespace {
      std::vector<std::string> suffix;
    };
    struct ContainsNamespace {
      std::vector<std::string> contains;
    };
    using NamespaceMatch =
        rfl::Variant<ExactNamespace, PrefixNamespace, SuffixNamespace, ContainsNamespace>;

    struct ExactTrack {
      std::string exact;
    };
    struct PrefixTrack {
      std::string prefix;
    };
    struct SuffixTrack {
      std::string suffix;
    };
    struct ContainsTrack {
      std::string contains;
    };
    using TrackMatch = rfl::Variant<ExactTrack, PrefixTrack, SuffixTrack, ContainsTrack>;

    // CAT4MOQ action names; client_setup and server_setup are rejected.
    std::vector<std::string> actions;
    // Omitted matches any namespace or track.
    std::optional<NamespaceMatch> namespace_match;
    std::optional<TrackMatch> track_match;
  };

  // MOQT AUTHORIZATION_TOKEN type (default 0).
  std::optional<uint64_t> token_type;
  std::optional<std::vector<HmacKey>> hmac_keys;
  // Default true.
  std::optional<bool> require_setup_token;
  // Default true.
  std::optional<bool> allow_request_token_override;
  // Reject unsupported token claims instead of ignoring them (default false).
  std::optional<bool> strict_claims;
  std::optional<std::vector<AnonymousScope>> anonymous_claim;
  // Required: bounds per-message verification cost.
  std::optional<uint32_t> max_tokens_per_message;
};

// Checks `config` and builds what the verifier needs. Every problem found is
// in the error, separated by "; ".
folly::Expected<auth::AuthConfig, std::string> resolve(const Config& config);

std::shared_ptr<const plugin::PluginType> type();

// The verifier config of `service`'s cat4moq-auth entry: the one with id
// `pluginId`, or the only one when `pluginId` is empty.
folly::Expected<auth::AuthConfig, std::string>
serviceAuthConfig(const config::ServiceConfig& service, std::string_view pluginId);

} // namespace openmoq::moqx::plugins::cat4moq
