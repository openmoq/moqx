/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "auth/C4m.h"
#include "config/Config.h"

#include <catapult/crypto.hpp>

#include <folly/Expected.h>
#include <folly/Unit.h>
#include <folly/lang/Bits.h>
#include <moxygen/MoQTypes.h>

#include <chrono>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace openmoq::moqx::auth {

// Encoding for local anonymous-policy matching only. C4M token scopes match
// individual fields and never use this representation.
inline std::string canonicalNamespace(const moxygen::TrackNamespace& ns) {
  std::string out;
  for (const auto& field : ns.trackNamespace) {
    const uint32_t lenBigEndian = folly::Endian::big(static_cast<uint32_t>(field.size()));
    out.append(reinterpret_cast<const char*>(&lenBigEndian), sizeof(lenBigEndian));
    out.append(field);
  }
  return out;
}

class AuthTokenVerifier {
public:
  explicit AuthTokenVerifier(config::AuthConfig config);

  bool enabled() const { return config_.enabled; }
  uint64_t tokenType() const { return config_.tokenType; }
  uint64_t dpopTokenType() const { return config_.dpopTokenType; }
  bool requireSetupToken() const { return config_.requireSetupToken; }
  bool allowRequestTokenOverride() const { return config_.allowRequestTokenOverride; }

  // Caps how many AUTHORIZATION_TOKEN params of the configured tokenType()
  // one message may carry; a message over the cap is rejected outright, not
  // truncated. Params of other token types are not counted. 0 would reject
  // every message carrying a matching token. Config validation requires >=1
  // when enabled().
  uint32_t maxTokensPerMessage() const { return config_.maxTokensPerMessage; }

  bool allowsAnonymous(
      Action action,
      const moxygen::TrackNamespace& ns,
      std::optional<std::string_view> trackName
  ) const;

  // connectionId identifies the accepting connection (see newConnectionId()). It binds tokens
  // carrying catreplay 1 to that connection; 0 means unknown, which such tokens cannot use.
  folly::Expected<Grants, AuthError>
  verify(const moxygen::AuthToken& token, uint64_t connectionId = 0) const;

private:
  // Key material is loaded once. ES256 entries contain only a public key;
  // kid routing never changes the key's configured algorithm.
  folly::Expected<Grants, AuthError> verifySignature(const moxygen::AuthToken& token) const;

  struct VerificationKey {
    std::string id;
    std::unique_ptr<const catapult::CryptographicAlgorithm> algorithm;
  };

  config::AuthConfig config_;
  ClaimPolicy claimPolicy_;
  std::vector<VerificationKey> keys_;
  std::unordered_map<std::string, std::size_t> keyIdIndex_;

  // Replay state for catreplay 1: token identity -> {connection, expiry}. Retained after the
  // connection closes until the token expires, so reuse stays blocked. Bounded; when full of
  // live entries new replay-protected tokens are refused rather than left unprotected.
  struct ReplayEntry {
    uint64_t connectionId;
    std::chrono::system_clock::time_point expiresAt;
  };
  static constexpr std::size_t kMaxReplayEntries = 1u << 17;
  bool bindReplay(const Grants& grants, uint64_t connectionId) const;

public:
  // DPoP: keeps the grants whose cnf binding is satisfied by a valid proof among `params` for this
  // exact action (draft-02 section 4.3). Grants without cnf pass through unchanged.
  std::vector<Grants> applyDpop(
      const std::vector<Grants>& grants,
      const moxygen::Parameters& params,
      const DpopContext& context
  ) const;

private:
  // True the first time a proof cti is seen within its window; false for a replay or when full.
  bool noteProofCti(
      const Grants& grants,
      const std::string& cti,
      std::chrono::system_clock::time_point expiresAt
  ) const;
  mutable std::mutex proofMutex_;
  mutable std::unordered_map<std::string, std::chrono::system_clock::time_point> proofCtis_;
  mutable std::mutex replayMutex_;
  mutable std::unordered_map<std::string, ReplayEntry> replay_;
};

// Process-unique, non-zero id for one accepted connection.
uint64_t newConnectionId();

// Returns every AUTHORIZATION_TOKEN parameter matching tokenType, not just the
// first — a message may carry more than one, and a request is authorized if
// any one of them verifies and covers the action (see authorize()).
std::vector<moxygen::AuthToken>
findAuthTokens(const moxygen::Parameters& params, uint64_t tokenType);

// Namespace-level authorization (e.g. PublishNamespace, SubscribeNamespace, setup).
bool allows(
    const Grants& grants,
    Action action,
    const moxygen::TrackNamespace& ns,
    std::chrono::system_clock::time_point now = std::chrono::system_clock::now()
);

// Track-level authorization (e.g. Subscribe, Publish, Fetch, TrackStatus).
bool allows(
    const Grants& grants,
    Action action,
    const moxygen::FullTrackName& ftn,
    std::chrono::system_clock::time_point now = std::chrono::system_clock::now()
);

// True if any element of grantsList allows the action (namespace-level).
bool allowsAny(
    const std::vector<Grants>& grantsList,
    Action action,
    const moxygen::TrackNamespace& ns,
    std::chrono::system_clock::time_point now = std::chrono::system_clock::now()
);

// True if any element of grantsList allows the action (track-level).
bool allowsAny(
    const std::vector<Grants>& grantsList,
    Action action,
    const moxygen::FullTrackName& ftn,
    std::chrono::system_clock::time_point now = std::chrono::system_clock::now()
);

const char* toString(AuthError error);

// Verifies the setup AUTHORIZATION_TOKEN(s): null when auth is disabled,
// else every successfully-verified token's grants. When requireSetupToken()
// is set, connecting is additionally gated on whether any one of them
// permits Action::ClientSetup; without it, verified grants pool into the
// session with no ClientSetup check.
folly::Expected<std::shared_ptr<const std::vector<Grants>>, AuthError> authenticateSetup(
    const AuthTokenVerifier& verifier,
    const moxygen::Parameters& setupParams,
    uint64_t connectionId = 0
);

// How long an authorized action may continue (moqt-reval, draft-02 section 2.2). Present only
// when every source that authorizes the action expires and asks to be revalidated: the action
// must end once `deadline` (the latest expiry among them) has passed, and until then the relay
// re-checks at least every `interval`. Absent when the action is authorized by anything that does
// not expire or does not ask for revalidation (anonymous_claim, a plain token).
struct Revalidation {
  std::chrono::system_clock::time_point deadline;
  std::chrono::seconds interval;
};

struct AuthDecision {
  std::optional<Revalidation> revalidation;
};

// Authorizes a request against session grants, or per-request token(s) when
// allow_request_token_override is set. Permitted if any verified request
// token, or any session grant, covers the action. Returns Unit when permitted.
folly::Expected<AuthDecision, AuthError> authorize(
    const AuthTokenVerifier& verifier,
    Action action,
    const moxygen::Parameters& params,
    const moxygen::TrackNamespace& ns,
    const std::vector<Grants>& sessionGrants,
    std::optional<std::string_view> trackName = std::nullopt,
    uint64_t connectionId = 0
);

} // namespace openmoq::moqx::auth
