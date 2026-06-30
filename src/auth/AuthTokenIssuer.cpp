/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "auth/AuthTokenIssuer.h"

#include <algorithm>
#include <cctype>
#include <stdexcept>
#include <string>
#include <utility>

namespace openmoq::moqx::auth {
namespace {

std::string canonicalNamespace(const moxygen::TrackNamespace& ns) {
  std::string out;
  for (const auto& field : ns.trackNamespace) {
    out.push_back(static_cast<char>((field.size() >> 24) & 0xff));
    out.push_back(static_cast<char>((field.size() >> 16) & 0xff));
    out.push_back(static_cast<char>((field.size() >> 8) & 0xff));
    out.push_back(static_cast<char>(field.size() & 0xff));
    out.append(field);
  }
  return out;
}

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

std::string normalizeActionName(std::string_view value) {
  auto out = trim(value);
  std::replace(out.begin(), out.end(), '-', '_');
  std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  return out;
}

Action parseAction(std::string_view value) {
  const auto name = normalizeActionName(value);
  if (name == "client_setup" || name == "setup" || name == "0") {
    return Action::ClientSetup;
  }
  if (name == "server_setup" || name == "1") {
    return Action::ServerSetup;
  }
  if (name == "publish_namespace" || name == "announce" || name == "2") {
    return Action::PublishNamespace;
  }
  if (name == "subscribe_namespace" || name == "3") {
    return Action::SubscribeNamespace;
  }
  if (name == "subscribe" || name == "4") {
    return Action::Subscribe;
  }
  if (name == "request_update" || name == "subscribe_update" || name == "5") {
    return Action::RequestUpdate;
  }
  if (name == "publish" || name == "6") {
    return Action::Publish;
  }
  if (name == "fetch" || name == "7") {
    return Action::Fetch;
  }
  if (name == "track_status" || name == "8") {
    return Action::TrackStatus;
  }
  throw std::invalid_argument("unknown CAT4MOQ action: " + std::string(value));
}

bool containsAction(const std::vector<Action>& actions, Action action) {
  return std::find(actions.begin(), actions.end(), action) != actions.end();
}

bool isNamespaceAction(Action action) {
  switch (action) {
  case Action::ServerSetup:
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
    throw std::invalid_argument("at least one CAT4MOQ action is required");
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
    throw std::invalid_argument("CAT4MOQ key id must be non-empty");
  }
  if (options.secret.empty()) {
    throw std::invalid_argument("CAT4MOQ secret must be non-empty");
  }
  if (options.actions.empty()) {
    throw std::invalid_argument("at least one CAT4MOQ action is required");
  }
  if (options.ttl <= std::chrono::seconds::zero()) {
    throw std::invalid_argument("CAT4MOQ ttl must be greater than zero");
  }

  Grants grants;
  grants.expiresAt = options.now + options.ttl;

  if (containsAction(options.actions, Action::ClientSetup)) {
    grants.scopes.push_back(Scope{
        .actions = {Action::ClientSetup},
        .namespaceMatches = {},
        .trackMatches = {},
    });
  }

  std::vector<MatchRule> namespaceMatches;
  if (!options.trackNamespace.trackNamespace.empty()) {
    namespaceMatches.push_back(MatchRule{
        .type = MatchRule::Type::Exact,
        .value = canonicalNamespace(options.trackNamespace),
    });
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
    });
  }

  return IssuedToken{.tokenValue = AuthTokenVerifier::sign(options.keyID, options.secret, grants)};
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
