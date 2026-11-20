/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "auth/Action.h"

#include <chrono>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace openmoq::moqx::auth {

// DPoP for MOQT, draft-ietf-moq-c4m-02 section 4. The proof claim labels actx and ath are not
// assigned yet; these are provisional private-use values chosen by moqx (the draft's appendix
// defines no proof vectors).
constexpr int64_t kDpopActxClaim = -65539;
constexpr int64_t kDpopAthClaim = -65540;
// Proofs travel as an AUTHORIZATION_TOKEN parameter of this type, which is not registered by
// the draft. Configurable per service.
constexpr uint64_t kDefaultDpopTokenType = 17;
constexpr std::chrono::seconds kDefaultDpopWindow{300};

// cnf confirmation method carried by the token.
struct DpopBinding {
  enum class Kind { Ckt, Jkt };
  Kind kind{Kind::Ckt};
  std::string thumbprint; // 32 raw SHA-256 bytes
};

// What a DPoP-bound token demands of each action (cnf + catdpop).
struct DpopRequirement {
  DpopBinding binding;
  std::chrono::seconds window{kDefaultDpopWindow};
  bool detectReplay{true}; // catdpop jti: reject a proof cti seen within the window
};

// The MOQT action a proof must name.
struct DpopContext {
  Action action{Action::ClientSetup};
  std::vector<std::string> trackNamespace; // empty = no namespace field
  std::optional<std::string> trackName;
};

struct VerifiedDpopProof {
  std::string cti;
  std::chrono::system_clock::time_point issuedAt;
};

// SHA-256 over raw bytes.
std::string sha256(std::string_view data);

// RFC 9679 COSE Key Thumbprint and RFC 7638 JWK Thumbprint for a P-256 key.
std::string coseKeyThumbprint(std::string_view x, std::string_view y);
std::string jwkThumbprint(std::string_view x, std::string_view y);

// Verifies steps 2-6 and 8 of section 4.3 for one proof against one token. `tokenHash` is the
// SHA-256 of the CAT token exactly as presented (see sha256()). Throws C4mError on any failure.
// Replay detection (step 7) is left to the caller, which owns the cti cache.
VerifiedDpopProof verifyDpopProof(
    std::string_view proof,
    std::string_view tokenHash,
    const DpopRequirement& requirement,
    const DpopContext& context,
    std::chrono::system_clock::time_point now = std::chrono::system_clock::now()
);

// Client-side proof creation from an unencrypted PEM P-256 private key. Used by tests and
// operator tooling; the relay never signs.
std::string signDpopProof(
    const std::string& privateKeyPemPath,
    std::string_view token,
    const DpopContext& context,
    std::string_view cti,
    std::chrono::system_clock::time_point issuedAt = std::chrono::system_clock::now()
);

// Public x/y (32 bytes each) of a PEM P-256 private or public key, for computing cnf values.
std::pair<std::string, std::string> es256PublicCoordinates(const std::string& pemPath);

} // namespace openmoq::moqx::auth
