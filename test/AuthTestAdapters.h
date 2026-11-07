/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

// moxygen-typed calls into the auth core, so the auth tests can build their
// inputs as real messages.

#include "auth/Auth.h"

#include <moxygen/MoQTypes.h>

#include <memory>

namespace openmoq::moqx::auth {

// The values of `params`' AUTHORIZATION_TOKENs of `tokenType`, as the plugin
// host hands them over.
inline TokenValues findAuthTokens(const moxygen::Parameters& params, uint64_t tokenType) {
  TokenValues out;
  for (const auto& param : params) {
    if (param.key == folly::to_underlying(moxygen::TrackRequestParamKey::AUTHORIZATION_TOKEN) &&
        param.asAuthToken.tokenType == tokenType) {
      out.push_back(param.asAuthToken.tokenValue);
    }
  }
  return out;
}

inline folly::Expected<Grants, AuthError>
verify(const AuthTokenVerifier& verifier, const moxygen::AuthToken& token) {
  return verifier.verify(token.tokenValue);
}

inline folly::Expected<std::shared_ptr<const std::vector<Grants>>, AuthError>
authenticateSetup(const AuthTokenVerifier& verifier, const moxygen::Parameters& params) {
  auto grants = authenticateSetup(verifier, findAuthTokens(params, verifier.tokenType()));
  if (grants.hasError()) {
    return folly::makeUnexpected(grants.error());
  }
  return std::make_shared<const std::vector<Grants>>(std::move(grants.value()));
}

inline folly::Expected<folly::Unit, AuthError> authorize(
    const AuthTokenVerifier& verifier,
    Action action,
    const moxygen::Parameters& params,
    const moxygen::TrackNamespace& ns,
    const std::vector<Grants>& sessionGrants,
    std::optional<std::string_view> trackName = std::nullopt
) {
  return authorize(
      verifier,
      action,
      findAuthTokens(params, verifier.tokenType()),
      Namespace(ns.trackNamespace),
      sessionGrants,
      trackName
  );
}

inline bool allows(
    const Grants& grants,
    Action action,
    const moxygen::TrackNamespace& ns,
    std::chrono::system_clock::time_point now = std::chrono::system_clock::now()
) {
  return allows(grants, action, Namespace(ns.trackNamespace), now);
}

inline bool allows(
    const Grants& grants,
    Action action,
    const moxygen::FullTrackName& ftn,
    std::chrono::system_clock::time_point now = std::chrono::system_clock::now()
) {
  return allows(grants, action, Namespace(ftn.trackNamespace.trackNamespace), ftn.trackName, now);
}

inline bool allowsAny(
    const std::vector<Grants>& grantsList,
    Action action,
    const moxygen::TrackNamespace& ns,
    std::chrono::system_clock::time_point now = std::chrono::system_clock::now()
) {
  return allowsAny(grantsList, action, Namespace(ns.trackNamespace), now);
}

inline bool allowsAny(
    const std::vector<Grants>& grantsList,
    Action action,
    const moxygen::FullTrackName& ftn,
    std::chrono::system_clock::time_point now = std::chrono::system_clock::now()
) {
  return allowsAny(
      grantsList,
      action,
      Namespace(ftn.trackNamespace.trackNamespace),
      ftn.trackName,
      now
  );
}

inline std::string canonicalNamespace(const moxygen::TrackNamespace& ns) {
  return canonicalNamespace(Namespace(ns.trackNamespace));
}

} // namespace openmoq::moqx::auth
