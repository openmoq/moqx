/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "auth/Auth.h"

#include <folly/portability/GTest.h>

using namespace openmoq::moqx;
using namespace openmoq::moqx::auth;
using namespace moxygen;

namespace {

void appendTypeLen(std::string& out, uint8_t major, uint64_t len) {
  if (len < 24) {
    out.push_back(static_cast<char>((major << 5) | len));
  } else if (len <= 0xff) {
    out.push_back(static_cast<char>((major << 5) | 24));
    out.push_back(static_cast<char>(len));
  } else if (len <= 0xffff) {
    out.push_back(static_cast<char>((major << 5) | 25));
    out.push_back(static_cast<char>((len >> 8) & 0xff));
    out.push_back(static_cast<char>(len & 0xff));
  } else {
    out.push_back(static_cast<char>((major << 5) | 26));
    out.push_back(static_cast<char>((len >> 24) & 0xff));
    out.push_back(static_cast<char>((len >> 16) & 0xff));
    out.push_back(static_cast<char>((len >> 8) & 0xff));
    out.push_back(static_cast<char>(len & 0xff));
  }
}

std::string cborUInt(uint64_t value) {
  std::string out;
  appendTypeLen(out, 0, value);
  return out;
}

std::string cborText(std::string_view value) {
  std::string out;
  appendTypeLen(out, 3, value.size());
  out.append(value);
  return out;
}

std::string cborBytes(std::string_view value) {
  std::string out;
  appendTypeLen(out, 2, value.size());
  out.append(value);
  return out;
}

std::string cborArray(std::initializer_list<std::string_view> values) {
  std::string out;
  appendTypeLen(out, 4, values.size());
  for (auto value : values) {
    out.append(value);
  }
  return out;
}

std::string cborMap(std::initializer_list<std::pair<std::string_view, std::string_view>> entries) {
  std::string out;
  appendTypeLen(out, 5, entries.size());
  for (const auto& [key, value] : entries) {
    out.append(key);
    out.append(value);
  }
  return out;
}

std::string namespaceBytes(const TrackNamespace& ns) {
  std::string out;
  for (const auto& field : ns.trackNamespace) {
    out.push_back(0);
    out.push_back(0);
    out.push_back(0);
    out.push_back(static_cast<char>(field.size()));
    out.append(field);
  }
  return out;
}

AuthToken
makeToken(std::string claims, std::string_view secret = "secret", std::string_view keyID = "k1") {
  return AuthToken{
      .tokenType = 77,
      .tokenValue = AuthTokenVerifier::signForTest(keyID, secret, claims),
      .alias = AuthToken::DontRegister,
  };
}

config::AuthConfig makeConfig() {
  return config::AuthConfig{
      .enabled = true,
      .tokenType = 77,
      .hmacKeys = {config::AuthConfig::HmacKey{.id = "k1", .secret = "secret"}},
      .requireSetupToken = true,
      .allowRequestTokenOverride = true,
  };
}

std::string claimsFor(Action action, const TrackNamespace& ns, std::string_view track) {
  auto actions = cborArray({cborUInt(static_cast<uint64_t>(action))});
  auto nsMatch = cborMap({{cborUInt(0), cborBytes(namespaceBytes(ns))}});
  auto trackMatch = cborMap({{cborUInt(0), cborBytes(track)}});
  auto scope = cborArray({actions, nsMatch, trackMatch});
  auto moqt = cborArray({scope});
  auto exp = cborUInt(4'102'444'800ULL); // 2100-01-01
  return cborMap({{cborUInt(4), exp}, {cborText("moqt"), moqt}});
}

} // namespace

TEST(AuthTest, VerifiesSignedTokenAndAllowsMatchingAction) {
  TrackNamespace ns{{"live", "event"}};
  AuthTokenVerifier verifier(makeConfig());
  auto grants = verifier.verify(makeToken(claimsFor(Action::Subscribe, ns, "video")));
  ASSERT_TRUE(grants.hasValue());
  EXPECT_TRUE(allows(grants.value(), Action::Subscribe, ns, "video"));
  EXPECT_FALSE(allows(grants.value(), Action::Publish, ns, "video"));
  EXPECT_FALSE(allows(grants.value(), Action::Subscribe, ns, "audio"));
}

TEST(AuthTest, RejectsBadSignature) {
  auto token = makeToken(claimsFor(Action::ClientSetup, TrackNamespace{}, ""));
  token.tokenValue.back() ^= 0x01;
  AuthTokenVerifier verifier(makeConfig());
  auto grants = verifier.verify(token);
  ASSERT_TRUE(grants.hasError());
  EXPECT_EQ(grants.error(), AuthError::BadSignature);
}

TEST(AuthTest, RejectsExpiredToken) {
  auto scope = cborArray(
      {cborArray({cborUInt(static_cast<uint64_t>(Action::ClientSetup))}), cborMap({}), cborMap({})}
  );
  auto claims = cborMap({{cborUInt(4), cborUInt(1)}, {cborText("moqt"), cborArray({scope})}});
  AuthTokenVerifier verifier(makeConfig());
  auto grants = verifier.verify(makeToken(claims));
  ASSERT_TRUE(grants.hasError());
  EXPECT_EQ(grants.error(), AuthError::Expired);
}
