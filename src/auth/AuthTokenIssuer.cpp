/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "auth/AuthTokenIssuer.h"

#include "auth/Action.h"
#include "auth/Dpop.h"
#include "auth/Es256Key.h"
#include "auth/HmacKey.h"

#include <openssl/rand.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

namespace openmoq::moqx::auth {
namespace {

std::string trim(std::string_view value) {
  auto begin = value.begin();
  auto end = value.end();
  while (begin != end && std::isspace(static_cast<unsigned char>(*begin))) {
    ++begin;
  }
  while (begin != end && std::isspace(static_cast<unsigned char>(*(end - 1)))) {
    --end;
  }
  return std::string(begin, end);
}

Action parseAction(std::string_view value) {
  if (auto action = canonicalAction(value)) {
    return *action;
  }
  throw std::invalid_argument("unknown C4M action: " + std::string(value));
}

bool containsAction(const std::vector<Action>& actions, Action action) {
  return std::find(actions.begin(), actions.end(), action) != actions.end();
}

bool isNamespaceAction(Action action) {
  switch (action) {
  case Action::SubscribeTracks:
  case Action::PublishNamespace:
  case Action::SubscribeNamespace:
    return true;
  case Action::ClientSetup:
  case Action::Subscribe:
  case Action::RequestUpdate:
  case Action::Publish:
  case Action::Fetch:
  case Action::TrackStatus:
    return false;
  }
  return false;
}

} // namespace

std::vector<Action> parseActions(std::string_view value) {
  std::vector<Action> actions;
  std::size_t start = 0;
  while (start <= value.size()) {
    const auto comma = value.find(',', start);
    const auto end = comma == std::string_view::npos ? value.size() : comma;
    auto token = trim(value.substr(start, end - start));
    if (!token.empty()) {
      actions.push_back(parseAction(token));
    }
    if (comma == std::string_view::npos) {
      break;
    }
    start = comma + 1;
  }
  if (actions.empty()) {
    throw std::invalid_argument("at least one C4M action is required");
  }
  return actions;
}

moxygen::TrackNamespace parseTrackNamespace(std::string_view value) {
  moxygen::TrackNamespace ns;
  std::size_t start = 0;
  while (start <= value.size()) {
    const auto slash = value.find('/', start);
    const auto end = slash == std::string_view::npos ? value.size() : slash;
    if (end > start) {
      ns.trackNamespace.emplace_back(value.substr(start, end - start));
    }
    if (slash == std::string_view::npos) {
      break;
    }
    start = slash + 1;
  }
  if (ns.trackNamespace.empty()) {
    ns.trackNamespace.emplace_back(value);
  }
  return ns;
}

IssuedToken issueToken(const IssueTokenOptions& options) {
  if (options.keyID.empty()) {
    throw std::invalid_argument("C4M key id must be non-empty");
  }
  if (options.secret.empty() == options.privateKeyFile.empty()) {
    throw std::invalid_argument("supply exactly one C4M HMAC secret or ES256 private key file");
  }
  if (options.actions.empty()) {
    throw std::invalid_argument("at least one C4M action is required");
  }
  if (options.ttl <= std::chrono::seconds::zero()) {
    throw std::invalid_argument("C4M ttl must be greater than zero");
  }

  Grants grants;
  grants.expiresAt = options.now + options.ttl;
  grants.issuer = options.issuer;
  grants.audiences = options.audiences;
  if (!options.dpopKeyFile.empty()) {
    const auto [x, y] = es256PublicCoordinates(options.dpopKeyFile);
    grants.dpop = DpopRequirement{
        DpopBinding{
            options.dpopJkt ? DpopBinding::Kind::Jkt : DpopBinding::Kind::Ckt,
            options.dpopJkt ? jwkThumbprint(x, y) : coseKeyThumbprint(x, y)
        },
        options.dpopWindow,
        true,
    };
  }
  if (options.replayProtected) {
    std::array<unsigned char, 16> id{};
    if (RAND_bytes(id.data(), static_cast<int>(id.size())) != 1) {
      throw std::runtime_error("could not generate cti");
    }
    grants.cti = std::string(reinterpret_cast<const char*>(id.data()), id.size());
    grants.replayProtected = true;
  }

  if (containsAction(options.actions, Action::ClientSetup)) {
    grants.scopes.push_back(Scope{
        .actions = {Action::ClientSetup},
        .namespaceMatches = {},
        .trackMatches = {},
    });
  }

  std::vector<MatchRule> namespaceMatches;
  if (!options.trackNamespace.trackNamespace.empty()) {
    for (const auto& field : options.trackNamespace.trackNamespace) {
      namespaceMatches.push_back(MatchRule{.value = field});
    }
  }

  std::vector<Action> namespaceActions;
  namespaceActions.reserve(options.actions.size());
  std::vector<Action> trackActions;
  trackActions.reserve(options.actions.size());
  for (auto action : options.actions) {
    if (action == Action::ClientSetup) {
      continue;
    }
    if (isNamespaceAction(action)) {
      namespaceActions.push_back(action);
    } else {
      trackActions.push_back(action);
    }
  }

  if (!namespaceActions.empty()) {
    grants.scopes.push_back(Scope{
        .actions = std::move(namespaceActions),
        .namespaceMatches = namespaceMatches,
        .trackMatches = {},
        .exactNamespace = !options.trackNamespace.trackNamespace.empty(),
    });
  }

  if (!trackActions.empty()) {
    std::vector<MatchRule> trackMatches;
    if (options.trackName.has_value()) {
      trackMatches.push_back(MatchRule{
          .type = MatchRule::Type::Exact,
          .value = *options.trackName,
      });
    }

    grants.scopes.push_back(Scope{
        .actions = std::move(trackActions),
        .namespaceMatches = std::move(namespaceMatches),
        .trackMatches = std::move(trackMatches),
        .exactNamespace =
            !options.trackNamespace.trackNamespace.empty() || options.trackName.has_value(),
    });
  }

  if (!options.privateKeyFile.empty()) {
    auto signer = loadEs256PrivateKeyFile(options.privateKeyFile);
    return IssuedToken{.tokenValue = signC4mToken(options.keyID, *signer, grants)};
  }
  return IssuedToken{.tokenValue = signGrants(options.keyID, options.secret, grants)};
}

std::string signGrants(std::string_view keyID, std::string_view secret, const Grants& grants) {
  return signC4mToken(keyID, deriveHmacKey(secret), grants);
}

config::AuthConfig::HmacKey selectHmacKey(const config::AuthConfig& auth, std::string_view keyID) {
  if (!auth.enabled) {
    throw std::invalid_argument("service auth is not enabled");
  }
  if (auth.hmacKeys.empty()) {
    throw std::invalid_argument("service auth has no HMAC keys");
  }
  if (keyID.empty()) {
    return auth.hmacKeys.front();
  }
  for (const auto& key : auth.hmacKeys) {
    if (key.id == keyID) {
      return key;
    }
  }
  throw std::invalid_argument("service auth has no HMAC key with id: " + std::string(keyID));
}

} // namespace openmoq::moqx::auth
