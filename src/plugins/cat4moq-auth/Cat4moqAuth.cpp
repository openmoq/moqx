/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "plugins/cat4moq-auth/Cat4moqAuth.h"

#include "auth/Auth.h"
#include "plugin/Registry.h"

#include <folly/String.h>
#include <folly/logging/xlog.h>

#include <unordered_set>

namespace openmoq::moqx::plugins::cat4moq {

namespace {

// MOQT carries token_type as a QUIC variable-length integer (RFC 9000 Section
// 16), whose maximum encodable value is 2^62 - 1. This bound is a property of
// the wire encoding, not a moqx policy, so it is stable across MOQT drafts.
constexpr uint64_t kQuicVarintExclusiveUpperBound = uint64_t{1} << 62;

auth::AuthConfig::AnonymousScope resolveAnonymousScope(const Config::AnonymousScope& scope) {
  using Scope = Config::AnonymousScope;
  using MatchMode = auth::MatchRuleType;

  auth::AuthConfig::AnonymousScope out;
  // Resolution runs only on validated input; every name canonicalizes.
  for (const auto& name : scope.actions) {
    out.actions.push_back(*auth::canonicalAction(name));
  }

  if (scope.namespace_match) {
    scope.namespace_match->visit([&](const auto& alt) {
      using N = std::decay_t<decltype(alt)>;
      if constexpr (std::is_same_v<N, Scope::ExactNamespace>) {
        out.namespaceSegments = alt.exact;
        out.namespaceMatchMode = MatchMode::Exact;
      } else if constexpr (std::is_same_v<N, Scope::PrefixNamespace>) {
        out.namespaceSegments = alt.prefix;
        out.namespaceMatchMode = MatchMode::Prefix;
      } else if constexpr (std::is_same_v<N, Scope::SuffixNamespace>) {
        out.namespaceSegments = alt.suffix;
        out.namespaceMatchMode = MatchMode::Suffix;
      } else { // ContainsNamespace
        out.namespaceSegments = alt.contains;
        out.namespaceMatchMode = MatchMode::Contains;
      }
    });
  }

  if (scope.track_match) {
    scope.track_match->visit([&](const auto& alt) {
      using T = std::decay_t<decltype(alt)>;
      if constexpr (std::is_same_v<T, Scope::ExactTrack>) {
        out.trackName = alt.exact;
        out.trackMatchMode = MatchMode::Exact;
      } else if constexpr (std::is_same_v<T, Scope::PrefixTrack>) {
        out.trackName = alt.prefix;
        out.trackMatchMode = MatchMode::Prefix;
      } else if constexpr (std::is_same_v<T, Scope::SuffixTrack>) {
        out.trackName = alt.suffix;
        out.trackMatchMode = MatchMode::Suffix;
      } else { // ContainsTrack
        out.trackName = alt.contains;
        out.trackMatchMode = MatchMode::Contains;
      }
    });
  }
  return out;
}

void validate(const Config& config, std::vector<std::string>& errors) {
  if (!config.hmac_keys || config.hmac_keys->empty()) {
    errors.emplace_back("hmac_keys is required");
  } else {
    std::unordered_set<std::string> keyIDs;
    for (size_t i = 0; i < config.hmac_keys->size(); ++i) {
      const auto& key = (*config.hmac_keys)[i];
      const auto prefix = "hmac_keys[" + std::to_string(i) + "]";
      if (key.id.empty()) {
        errors.push_back(prefix + ".id must be non-empty");
      } else if (!keyIDs.insert(key.id).second) {
        errors.push_back(prefix + ".id duplicates another key");
      }
      if (key.secret.empty()) {
        errors.push_back(prefix + ".secret must be non-empty");
      }
    }
  }
  // token_type 0 is a valid MOQT AUTHORIZATION_TOKEN type, accepted as-is and
  // not treated as "unset": omitting it and setting token_type: 0 are identical.
  if (config.token_type.value_or(0) >= kQuicVarintExclusiveUpperBound) {
    errors.emplace_back("token_type must fit in a QUIC varint");
  }
  if (!config.max_tokens_per_message) {
    errors.emplace_back("max_tokens_per_message is required (set it here or in "
                        "service_defaults.plugins.cat4moq-auth)");
  } else if (*config.max_tokens_per_message == 0) {
    errors.emplace_back("max_tokens_per_message must be >= 1");
  }

  if (!config.anonymous_claim) {
    return;
  }
  for (size_t i = 0; i < config.anonymous_claim->size(); ++i) {
    const auto& scope = (*config.anonymous_claim)[i];
    const auto prefix = "anonymous_claim[" + std::to_string(i) + "]";
    if (scope.actions.empty()) {
      errors.push_back(prefix + ".actions must be non-empty");
      continue;
    }
    for (const auto& name : scope.actions) {
      auto action = auth::canonicalAction(name);
      if (!action) {
        errors.push_back(prefix + ".actions: '" + name + "' is not a recognized CAT4MOQ action");
      } else if (*action == auth::Action::ClientSetup || *action == auth::Action::ServerSetup) {
        errors.push_back(
            prefix + ".actions: '" + name +
            "' is not allowed here -- the anonymous claim never authorizes session setup"
        );
      }
    }
  }
}

plugin::Error toError(auth::AuthError error, bool atSetup) {
  // Setup keeps distinct close codes for an expired or malformed token;
  // request rejections stay UNAUTHORIZED.
  auto kind = plugin::Error::Kind::Unauthorized;
  if (atSetup && error == auth::AuthError::Expired) {
    kind = plugin::Error::Kind::ExpiredToken;
  } else if (atSetup && error == auth::AuthError::Malformed) {
    kind = plugin::Error::Kind::MalformedToken;
  }
  return plugin::Error{kind, auth::toString(error)};
}

auth::TokenValues tokenValues(const plugin::ParamsView& params, uint64_t tokenType) {
  auth::TokenValues out;
  for (auto& token : params.authTokens(tokenType)) {
    out.push_back(std::move(token.value));
  }
  return out;
}

auth::Action toAction(plugin::RequestKind kind) {
  switch (kind) {
  case plugin::RequestKind::Subscribe:
    return auth::Action::Subscribe;
  case plugin::RequestKind::Fetch:
    return auth::Action::Fetch;
  case plugin::RequestKind::TrackStatus:
    return auth::Action::TrackStatus;
  case plugin::RequestKind::Publish:
    return auth::Action::Publish;
  case plugin::RequestKind::PublishNamespace:
    return auth::Action::PublishNamespace;
  case plugin::RequestKind::SubscribeNamespace:
  // SUBSCRIBE_TRACKS is namespace-level, like SUBSCRIBE_NAMESPACE.
  case plugin::RequestKind::SubscribeTracks:
    return auth::Action::SubscribeNamespace;
  }
  return auth::Action::Subscribe;
}

struct SessionGrants : plugin::SessionState {
  explicit SessionGrants(std::vector<auth::Grants> grants) : grants(std::move(grants)) {}
  std::vector<auth::Grants> grants;
};

class Service : public plugin::ServiceInstance {
public:
  explicit Service(std::shared_ptr<const auth::AuthTokenVerifier> verifier)
      : verifier_(std::move(verifier)) {}

  plugin::HookSet hooks() const override { return plugin::Hook::Admit | plugin::Hook::Authorize; }

  folly::coro::Task<plugin::Result<std::shared_ptr<plugin::SessionState>>>
  admit(const plugin::SetupView& setup) override {
    auto grants =
        auth::authenticateSetup(*verifier_, tokenValues(setup.params, verifier_->tokenType()));
    if (grants.hasError()) {
      co_return folly::makeUnexpected(toError(grants.error(), /*atSetup=*/true));
    }
    co_return std::make_shared<SessionGrants>(std::move(grants.value()));
  }

  folly::coro::Task<plugin::Result<folly::Unit>>
  authorize(const plugin::RequestView& request, const plugin::SessionState* state) override {
    // Peer relays authenticate with the relay token; only namespace interest
    // skips grant checks for them.
    if (request.peer && (request.kind == plugin::RequestKind::SubscribeNamespace ||
                         request.kind == plugin::RequestKind::SubscribeTracks)) {
      co_return folly::unit;
    }
    static const std::vector<auth::Grants> kNoGrants;
    const auto* session = static_cast<const SessionGrants*>(state);
    auto ok = auth::authorize(
        *verifier_,
        toAction(request.kind),
        tokenValues(request.params, verifier_->tokenType()),
        request.ns,
        session ? session->grants : kNoGrants,
        request.track
    );
    if (ok.hasError()) {
      co_return folly::makeUnexpected(toError(ok.error(), /*atSetup=*/false));
    }
    co_return folly::unit;
  }

private:
  std::shared_ptr<const auth::AuthTokenVerifier> verifier_;
};

class Root : public plugin::Root {
public:
  explicit Root(auth::AuthConfig config)
      : verifier_(std::make_shared<const auth::AuthTokenVerifier>(std::move(config))) {}

  plugin::Result<std::unique_ptr<plugin::ServiceInstance>>
  createService(const plugin::ServiceContext&) override {
    return std::make_unique<Service>(verifier_);
  }

private:
  std::shared_ptr<const auth::AuthTokenVerifier> verifier_;
};

} // namespace

folly::Expected<auth::AuthConfig, std::string> resolve(const Config& config) {
  std::vector<std::string> errors;
  validate(config, errors);
  if (!errors.empty()) {
    return folly::makeUnexpected(folly::join("; ", errors));
  }
  auth::AuthConfig out;
  out.tokenType = config.token_type.value_or(0);
  out.requireSetupToken = config.require_setup_token.value_or(true);
  out.allowRequestTokenOverride = config.allow_request_token_override.value_or(true);
  out.strictClaims = config.strict_claims.value_or(false);
  out.maxTokensPerMessage = *config.max_tokens_per_message;
  for (const auto& key : *config.hmac_keys) {
    out.hmacKeys.push_back(auth::AuthConfig::HmacKey{.id = key.id, .secret = key.secret});
  }
  if (config.anonymous_claim) {
    for (const auto& scope : *config.anonymous_claim) {
      out.anonymousClaim.push_back(resolveAnonymousScope(scope));
    }
  }
  return out;
}

folly::Expected<auth::AuthConfig, std::string>
serviceAuthConfig(const config::ServiceConfig& service, std::string_view pluginId) {
  const config::PluginEntryConfig* found = nullptr;
  for (const auto& entry : service.plugins) {
    if (entry.type != kType || (!pluginId.empty() && entry.id != pluginId)) {
      continue;
    }
    if (found) {
      return folly::makeUnexpected(std::string("several cat4moq-auth entries; pick one by id"));
    }
    found = &entry;
  }
  if (!found) {
    return folly::makeUnexpected(std::string("no matching cat4moq-auth entry"));
  }
  return resolve(*static_cast<const Config*>(found->config.get()));
}

std::shared_ptr<const plugin::PluginType> type() {
  return plugin::makePluginType<Config>(
      std::string(kType),
      plugin::Scopes{.service = true},
      [](const Config& config) -> plugin::Result<std::unique_ptr<plugin::Root>> {
        auto resolved = resolve(config);
        if (resolved.hasError()) {
          return plugin::fail(plugin::Error::Kind::Internal, resolved.error());
        }
        return std::make_unique<Root>(std::move(resolved.value()));
      },
      [](Config& config) {
        // Signing secrets never leave the process, e.g. over /config.
        if (config.hmac_keys) {
          for (auto& key : *config.hmac_keys) {
            key.secret = "<redacted>";
          }
        }
      }
  );
}

} // namespace openmoq::moqx::plugins::cat4moq
