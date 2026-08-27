/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "tls/SniCertManager.h"

#include "CertTestUtils.h"
#include "tls/CertLoader.h"

#include <fizz/protocol/Certificate.h>
#include <gmock/gmock.h>
#include <gtest/gtest.h>

namespace openmoq::moqx::tls {
namespace {

using ::testing::HasSubstr;

using moqx::test::makeSelfSignedCertPem;
using moqx::test::TempDir;
using moqx::test::TestKeyType;

const std::vector<fizz::SignatureScheme> kAllSchemes = {
    fizz::SignatureScheme::ecdsa_secp256r1_sha256,
    fizz::SignatureScheme::rsa_pss_sha256,
};

SniCertManager::Options optionsFor(const TempDir& dir, std::chrono::seconds interval = {}) {
  SniCertManager::Options options;
  options.certDir = config::CertDirConfig{dir.path(), interval};
  return options;
}

// SNI lookup with every scheme supported on both sides; returns folly::none on
// a miss.
fizz::CertMatch match(const SniCertManager& manager, folly::Optional<std::string> sni) {
  fizz::CertMatch ret;
  fizz::Error err;
  fizz::ClientHello chlo;
  EXPECT_EQ(manager.getCert(ret, err, sni, kAllSchemes, kAllSchemes, chlo), fizz::Status::Success);
  return ret;
}

std::string identityOf(const fizz::CertMatch& result) {
  if (!result) {
    return "";
  }
  return result->cert->getIdentity();
}

// Guarantee a strictly newer mtime than a previous write of the same path.
void bumpMtime(const TempDir& dir, const std::string& filename) {
  namespace fs = std::filesystem;
  auto p = fs::path(dir.path()) / filename;
  fs::last_write_time(p, fs::last_write_time(p) + std::chrono::seconds(2));
}

TEST(SniCertManager, ExactMatch) {
  TempDir dir;
  writePair(dir, "a", makeSelfSignedCertPem("a.example.com"));
  writePair(dir, "b", makeSelfSignedCertPem("b.example.com"));
  SniCertManager manager(optionsFor(dir));

  auto result = match(manager, std::string("a.example.com"));
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(identityOf(result), "a.example.com");
  EXPECT_EQ(result->type, fizz::MatchType::Direct);
  EXPECT_EQ(identityOf(match(manager, std::string("b.example.com"))), "b.example.com");
}

TEST(SniCertManager, CaseInsensitive) {
  TempDir dir;
  writePair(dir, "a", makeSelfSignedCertPem("a.example.com"));
  SniCertManager manager(optionsFor(dir));

  EXPECT_EQ(identityOf(match(manager, std::string("A.EXAMPLE.com"))), "a.example.com");
}

TEST(SniCertManager, WildcardMatchesOneLabel) {
  TempDir dir;
  writePair(dir, "wild", makeSelfSignedCertPem("*.example.com", {"*.example.com"}));
  SniCertManager manager(optionsFor(dir));

  EXPECT_EQ(identityOf(match(manager, std::string("a.example.com"))), "*.example.com");
  // Two labels deep: ".a.example.com" is not a stored key.
  EXPECT_FALSE(match(manager, std::string("b.a.example.com")).has_value());
  // The bare zone doesn't match "*.example.com" either.
  EXPECT_FALSE(match(manager, std::string("example.com")).has_value());
}

TEST(SniCertManager, SniKeepsALeadingStar) {
  TempDir dir;
  writePair(dir, "foo", makeSelfSignedCertPem("foo.example.com"));
  writePair(dir, "wild", makeSelfSignedCertPem("*.example.com", {"*.example.com"}));
  SniCertManager manager(optionsFor(dir));

  // fizz lowercases the SNI and nothing else; stripping a leading '*' is the
  // insertion-side rule. So this misses the exact key and falls through to the
  // first-dot wildcard instead of hitting "foo.example.com".
  EXPECT_EQ(identityOf(match(manager, std::string("*foo.example.com"))), "*.example.com");
}

TEST(SniCertManager, NoMatchNoFallbackReturnsNone) {
  TempDir dir;
  writePair(dir, "a", makeSelfSignedCertPem("a.example.com"));
  SniCertManager manager(optionsFor(dir));

  EXPECT_FALSE(match(manager, std::string("unknown.example.com")).has_value());
  EXPECT_FALSE(match(manager, folly::none).has_value());
}

TEST(SniCertManager, FallbackServesUnmatchedAndAbsentSni) {
  TempDir dir;
  writePair(dir, "a", makeSelfSignedCertPem("a.example.com"));
  auto fallbackPem = makeSelfSignedCertPem("fallback.example.com");
  TempDir fallbackDir;
  writePair(fallbackDir, "fb", fallbackPem);

  auto options = optionsFor(dir);
  options.fallbackCertFile = fallbackDir.path() + "/fb.pem";
  options.fallbackKeyFile = fallbackDir.path() + "/fb.key";
  SniCertManager manager(std::move(options));

  auto unmatched = match(manager, std::string("unknown.example.com"));
  ASSERT_TRUE(unmatched.has_value());
  EXPECT_EQ(identityOf(unmatched), "fallback.example.com");
  EXPECT_EQ(unmatched->type, fizz::MatchType::Default);

  auto absent = match(manager, folly::none);
  ASSERT_TRUE(absent.has_value());
  EXPECT_EQ(identityOf(absent), "fallback.example.com");

  // SNI hits still take precedence over the fallback.
  EXPECT_EQ(identityOf(match(manager, std::string("a.example.com"))), "a.example.com");
}

TEST(SniCertManager, MaterialFallback) {
  TempDir dir;
  writePair(dir, "a", makeSelfSignedCertPem("a.example.com"));
  auto fallbackPem = makeSelfSignedCertPem("mat.example.com");

  auto options = optionsFor(dir);
  options.fallbackMaterial = config::TlsMaterial{fallbackPem.certPem, fallbackPem.keyPem};
  SniCertManager manager(std::move(options));

  EXPECT_EQ(identityOf(match(manager, folly::none)), "mat.example.com");
}

TEST(SniCertManager, EmptyDirNoFallbackThrows) {
  TempDir dir;
  try {
    SniCertManager manager(optionsFor(dir));
    FAIL() << "expected empty cert_dir with no fallback to throw";
  } catch (const std::runtime_error& e) {
    EXPECT_THAT(e.what(), HasSubstr("no fallback"));
  }
}

TEST(SniCertManager, EmptyDirWithFallbackServes) {
  TempDir dir;
  TempDir fallbackDir;
  writePair(fallbackDir, "fb", makeSelfSignedCertPem("fallback.example.com"));

  auto options = optionsFor(dir);
  options.fallbackCertFile = fallbackDir.path() + "/fb.pem";
  options.fallbackKeyFile = fallbackDir.path() + "/fb.key";
  SniCertManager manager(std::move(options));

  EXPECT_EQ(identityOf(match(manager, std::string("anything"))), "fallback.example.com");
}

TEST(SniCertManager, StrictStartupScanThrows) {
  TempDir dir;
  dir.writeFile("orphan.pem", makeSelfSignedCertPem("x.example.com").certPem);
  EXPECT_THROW(SniCertManager{optionsFor(dir)}, std::runtime_error);
}

TEST(SniCertManager, ServingSurvivesFileDeletion) {
  TempDir dir;
  writePair(dir, "a", makeSelfSignedCertPem("a.example.com"));
  SniCertManager manager(optionsFor(dir));

  // The constructor loaded the pair, so handshakes never touch the files.
  dir.removeFile("a.pem");
  dir.removeFile("a.key");
  EXPECT_EQ(identityOf(match(manager, std::string("a.example.com"))), "a.example.com");
}

TEST(SniCertManager, BrokenKeyIsFatalAtStartup) {
  TempDir dir;
  auto pairA = makeSelfSignedCertPem("a.example.com");
  auto pairB = makeSelfSignedCertPem("b.example.com");
  // a.pem paired with b's key: the scan passes (the cert parses), the load
  // does not. Startup loads every pair, so this cannot reach a handshake.
  dir.writeFile("a.pem", pairA.certPem);
  dir.writeFile("a.key", pairB.keyPem);
  TempDir fallbackDir;
  writePair(fallbackDir, "fb", makeSelfSignedCertPem("fallback.example.com"));

  auto options = optionsFor(dir);
  options.fallbackCertFile = fallbackDir.path() + "/fb.pem";
  options.fallbackKeyFile = fallbackDir.path() + "/fb.key";
  EXPECT_THROW(SniCertManager{std::move(options)}, std::runtime_error);
}

TEST(SniCertManager, SchemeSelectionPrefersPeerIntersection) {
  TempDir dir;
  writePair(dir, "ec", makeSelfSignedCertPem("ec.example.com", {}, TestKeyType::EC256));
  writePair(dir, "rsa", makeSelfSignedCertPem("rsa.example.com", {}, TestKeyType::RSA2048));
  SniCertManager manager(optionsFor(dir));

  fizz::CertMatch ret;
  fizz::Error err;
  fizz::ClientHello chlo;

  // Peer speaks only ECDSA: the EC cert negotiates that scheme.
  std::vector<fizz::SignatureScheme> ecOnly = {fizz::SignatureScheme::ecdsa_secp256r1_sha256};
  ASSERT_EQ(
      manager.getCert(ret, err, std::string("ec.example.com"), kAllSchemes, ecOnly, chlo),
      fizz::Status::Success
  );
  ASSERT_TRUE(ret.has_value());
  EXPECT_EQ(ret->scheme, fizz::SignatureScheme::ecdsa_secp256r1_sha256);

  // Peer speaks only ECDSA but SNI selects the RSA cert: per the CertManager
  // contract peer schemes are ignored rather than failing the match.
  ASSERT_EQ(
      manager.getCert(ret, err, std::string("rsa.example.com"), kAllSchemes, ecOnly, chlo),
      fizz::Status::Success
  );
  ASSERT_TRUE(ret.has_value());
  EXPECT_EQ(ret->scheme, fizz::SignatureScheme::rsa_pss_sha256);

  // Server itself doesn't support RSA schemes: the RSA cert is unusable.
  std::vector<fizz::SignatureScheme> serverEcOnly = {fizz::SignatureScheme::ecdsa_secp256r1_sha256};
  ASSERT_EQ(
      manager.getCert(ret, err, std::string("rsa.example.com"), serverEcOnly, kAllSchemes, chlo),
      fizz::Status::Success
  );
  EXPECT_FALSE(ret.has_value());
}

TEST(SniCertManager, SchemeSelectionFollowsAReloadedKeyType) {
  TempDir dir;
  writePair(dir, "a", makeSelfSignedCertPem("a.example.com", {}, TestKeyType::EC256));
  SniCertManager manager(optionsFor(dir));
  EXPECT_EQ(
      match(manager, std::string("a.example.com"))->scheme,
      fizz::SignatureScheme::ecdsa_secp256r1_sha256
  );

  // Rotate the pair EC -> RSA. The negotiated scheme must come from the
  // reloaded cert: a scheme the served key cannot produce would fail every
  // handshake for the identity.
  writePair(dir, "a", makeSelfSignedCertPem("a.example.com", {}, TestKeyType::RSA2048));
  bumpMtime(dir, "a.pem");
  manager.rescan();

  auto result = match(manager, std::string("a.example.com"));
  ASSERT_TRUE(result.has_value());
  EXPECT_EQ(result->scheme, fizz::SignatureScheme::rsa_pss_sha256);
}

TEST(SniCertManager, ResumptionLooksUpPrimaryIdentityCn) {
  TempDir dir;
  writePair(dir, "a", makeSelfSignedCertPem("relay-cn-a", {"a.example.com"}));
  SniCertManager manager(optionsFor(dir));

  // SNI matches via the SAN; the served identity is the CN.
  EXPECT_EQ(identityOf(match(manager, std::string("a.example.com"))), "relay-cn-a");
  // Ticket resumption resolves the cert's primary identity (its CN).
  // With SANs present, the CN is not among the SNI identities.
  auto cert = manager.getCert(std::string("relay-cn-a"));
  ASSERT_NE(cert, nullptr);
  EXPECT_EQ(cert->getIdentity(), "relay-cn-a");
}

TEST(SniCertManager, SharedCnAcrossPairsIsNotADuplicate) {
  TempDir dir;
  writePair(dir, "a", makeSelfSignedCertPem("shared-cn", {"a.example.com"}));
  writePair(dir, "b", makeSelfSignedCertPem("shared-cn", {"b.example.com"}));
  // A CN shared by several pairs must not trip duplicate-identity detection.
  SniCertManager manager(optionsFor(dir));

  EXPECT_EQ(identityOf(match(manager, std::string("a.example.com"))), "shared-cn");
  EXPECT_EQ(identityOf(match(manager, std::string("b.example.com"))), "shared-cn");
  EXPECT_NE(manager.getCert(std::string("shared-cn")), nullptr);
}

TEST(SniCertManager, PeerSchemeMismatchPrefersUsableFallback) {
  TempDir dir;
  writePair(dir, "rsa", makeSelfSignedCertPem("rsa.example.com", {}, TestKeyType::RSA2048));
  TempDir fallbackDir;
  writePair(fallbackDir, "fb", makeSelfSignedCertPem("fallback.example.com")); // EC256

  auto options = optionsFor(dir);
  options.fallbackCertFile = fallbackDir.path() + "/fb.pem";
  options.fallbackKeyFile = fallbackDir.path() + "/fb.key";
  SniCertManager manager(std::move(options));

  // Peer only speaks ECDSA and the SNI-matched cert is RSA: the EC fallback
  // (which the peer can verify) beats serving the RSA cert with a scheme the
  // peer never advertised.
  fizz::CertMatch ret;
  fizz::Error err;
  fizz::ClientHello chlo;
  std::vector<fizz::SignatureScheme> ecOnly = {fizz::SignatureScheme::ecdsa_secp256r1_sha256};
  ASSERT_EQ(
      manager.getCert(ret, err, std::string("rsa.example.com"), kAllSchemes, ecOnly, chlo),
      fizz::Status::Success
  );
  ASSERT_TRUE(ret.has_value());
  EXPECT_EQ(identityOf(ret), "fallback.example.com");
  EXPECT_EQ(ret->scheme, fizz::SignatureScheme::ecdsa_secp256r1_sha256);
}

TEST(SniCertManager, GetCertByIdentityForResumption) {
  TempDir dir;
  writePair(dir, "a", makeSelfSignedCertPem("a.example.com"));
  TempDir fallbackDir;
  writePair(fallbackDir, "fb", makeSelfSignedCertPem("fallback.example.com"));

  auto options = optionsFor(dir);
  options.fallbackCertFile = fallbackDir.path() + "/fb.pem";
  options.fallbackKeyFile = fallbackDir.path() + "/fb.key";
  SniCertManager manager(std::move(options));

  auto cert = manager.getCert(std::string("a.example.com"));
  ASSERT_NE(cert, nullptr);
  EXPECT_EQ(cert->getIdentity(), "a.example.com");

  EXPECT_NE(manager.getCert(std::string("fallback.example.com")), nullptr);
  EXPECT_EQ(manager.getCert(std::string("removed.example.com")), nullptr);
}

TEST(SniCertManager, GetCertByIdentityFallsBackWhenRescanDropsThePair) {
  TempDir dir;
  writePair(dir, "a", makeSelfSignedCertPem("a.example.com"));
  TempDir fallbackDir;
  writePair(fallbackDir, "fb", makeSelfSignedCertPem("fb.example.com"));

  auto options = optionsFor(dir);
  options.fallbackCertFile = fallbackDir.path() + "/fb.pem";
  options.fallbackKeyFile = fallbackDir.path() + "/fb.key";
  SniCertManager manager(std::move(options));

  // A pair that appears already broken has no previous version to keep, so
  // the rescan drops it. Resumption must still reach the fallback, matching
  // what a fresh handshake for the same SNI gets.
  auto pair = makeSelfSignedCertPem("fb.example.com");
  auto other = makeSelfSignedCertPem("other.example.com");
  dir.writeFile("fb.pem", pair.certPem);
  dir.writeFile("fb.key", other.keyPem);
  manager.rescan();

  auto cert = manager.getCert(std::string("fb.example.com"));
  ASSERT_NE(cert, nullptr);
  EXPECT_EQ(cert->getIdentity(), "fb.example.com");
  EXPECT_EQ(identityOf(match(manager, std::string("fb.example.com"))), "fb.example.com");
}

// --- rescan (driven directly; no timer involved) ---

TEST(SniCertManager, RescanPicksUpAddedPair) {
  TempDir dir;
  writePair(dir, "a", makeSelfSignedCertPem("a.example.com"));
  SniCertManager manager(optionsFor(dir));

  EXPECT_FALSE(match(manager, std::string("new.example.com")).has_value());
  writePair(dir, "new", makeSelfSignedCertPem("new.example.com"));
  manager.rescan();
  EXPECT_EQ(identityOf(match(manager, std::string("new.example.com"))), "new.example.com");
}

TEST(SniCertManager, RescanDropsRemovedPair) {
  TempDir dir;
  writePair(dir, "a", makeSelfSignedCertPem("a.example.com"));
  writePair(dir, "b", makeSelfSignedCertPem("b.example.com"));
  SniCertManager manager(optionsFor(dir));

  dir.removeFile("b.pem");
  dir.removeFile("b.key");
  manager.rescan();
  EXPECT_FALSE(match(manager, std::string("b.example.com")).has_value());
  EXPECT_EQ(identityOf(match(manager, std::string("a.example.com"))), "a.example.com");
}

TEST(SniCertManager, RescanDropsPairWhenOnlyTheKeyIsRemoved) {
  TempDir dir;
  writePair(dir, "a", makeSelfSignedCertPem("a.example.com"));
  writePair(dir, "b", makeSelfSignedCertPem("b.example.com"));
  SniCertManager manager(optionsFor(dir));

  // The scan rejects the orphan .pem on every pass, so retaining it would
  // serve b forever: deleting the key must retire the pair.
  dir.removeFile("b.key");
  manager.rescan();
  EXPECT_FALSE(match(manager, std::string("b.example.com")).has_value());
  manager.rescan();
  EXPECT_FALSE(match(manager, std::string("b.example.com")).has_value());
  EXPECT_EQ(identityOf(match(manager, std::string("a.example.com"))), "a.example.com");
}

TEST(SniCertManager, RescanReloadsRewrittenPair) {
  TempDir dir;
  writePair(dir, "a", makeSelfSignedCertPem("a.example.com"));
  SniCertManager manager(optionsFor(dir));
  EXPECT_EQ(identityOf(match(manager, std::string("a.example.com"))), "a.example.com");

  // Same identity, fresh key material; SANs move to a second name to make the
  // reload observable.
  writePair(dir, "a", makeSelfSignedCertPem("a.example.com", {"a.example.com", "a2.example.com"}));
  bumpMtime(dir, "a.pem");
  manager.rescan();

  EXPECT_EQ(identityOf(match(manager, std::string("a2.example.com"))), "a.example.com");
}

TEST(SniCertManager, RescanKeepsLoadedCertForUnchangedPair) {
  TempDir dir;
  writePair(dir, "a", makeSelfSignedCertPem("a.example.com"));
  SniCertManager manager(optionsFor(dir));

  auto before = match(manager, std::string("a.example.com"));
  manager.rescan();
  auto after = match(manager, std::string("a.example.com"));
  ASSERT_TRUE(before.has_value());
  ASSERT_TRUE(after.has_value());
  // Same SelfCert instance: the loaded cert was carried across the rescan.
  EXPECT_EQ(before->cert.get(), after->cert.get());
}

TEST(SniCertManager, RescanBrokenRewriteKeepsServingLastGoodCert) {
  TempDir dir;
  writePair(dir, "a", makeSelfSignedCertPem("a.example.com"));
  TempDir fallbackDir;
  writePair(fallbackDir, "fb", makeSelfSignedCertPem("fallback.example.com"));

  auto options = optionsFor(dir);
  options.fallbackCertFile = fallbackDir.path() + "/fb.pem";
  options.fallbackKeyFile = fallbackDir.path() + "/fb.key";
  SniCertManager manager(std::move(options));
  EXPECT_EQ(identityOf(match(manager, std::string("a.example.com"))), "a.example.com");

  // Rewrite with a mismatched key: the scan still indexes the pair (the cert
  // parses) and the rescan's load fails — the previously loaded cert keeps
  // serving, not the wrong-name fallback.
  auto other = makeSelfSignedCertPem("other.example.com");
  dir.writeFile("a.key", other.keyPem);
  bumpMtime(dir, "a.key");
  manager.rescan();

  EXPECT_EQ(identityOf(match(manager, std::string("a.example.com"))), "a.example.com");
}

TEST(SniCertManager, RescanKeepsPairWhenRewriteIsUnparsable) {
  TempDir dir;
  writePair(dir, "a", makeSelfSignedCertPem("a.example.com"));
  SniCertManager manager(optionsFor(dir));

  // Non-atomic rewrite caught mid-write: unparsable cert, file still exists.
  // The rescan treats this as a scan error, not a removal; the loaded cert
  // keeps serving.
  dir.writeFile("a.pem", "not a certificate");
  bumpMtime(dir, "a.pem");
  manager.rescan();
  EXPECT_EQ(identityOf(match(manager, std::string("a.example.com"))), "a.example.com");
}

TEST(SniCertManager, RescanDropsNewPairThatFailsToLoad) {
  TempDir dir;
  writePair(dir, "a", makeSelfSignedCertPem("a.example.com"));
  TempDir fallbackDir;
  writePair(fallbackDir, "fb", makeSelfSignedCertPem("fallback.example.com"));

  auto options = optionsFor(dir);
  options.fallbackCertFile = fallbackDir.path() + "/fb.pem";
  options.fallbackKeyFile = fallbackDir.path() + "/fb.key";
  SniCertManager manager(std::move(options));

  // New pair, mismatched key: the cert parses so the scan indexes it, but the
  // load fails and there is no previous version to keep.
  auto good = makeSelfSignedCertPem("new.example.com");
  auto other = makeSelfSignedCertPem("other.example.com");
  dir.writeFile("new.pem", good.certPem);
  dir.writeFile("new.key", other.keyPem);
  manager.rescan();
  EXPECT_EQ(identityOf(match(manager, std::string("new.example.com"))), "fallback.example.com");

  // Repairing it hands the identity over on the next rescan.
  dir.writeFile("new.key", good.keyPem);
  manager.rescan();
  EXPECT_EQ(identityOf(match(manager, std::string("new.example.com"))), "new.example.com");
}

TEST(SniCertManager, RescanIncumbentKeepsIdentityAgainstNewDuplicate) {
  TempDir dir;
  writePair(dir, "z", makeSelfSignedCertPem("z-cn", {"a.example.com"}));
  SniCertManager manager(optionsFor(dir));
  EXPECT_EQ(identityOf(match(manager, std::string("a.example.com"))), "z-cn");

  // A newcomer claiming a served identity must not steal it even though "a"
  // sorts before "z"; the arrival is dropped with a duplicate warning.
  writePair(dir, "a", makeSelfSignedCertPem("a-cn", {"a.example.com"}));
  manager.rescan();
  EXPECT_EQ(identityOf(match(manager, std::string("a.example.com"))), "z-cn");

  // Removing the incumbent hands the identity over on the next rescan.
  dir.removeFile("z.pem");
  dir.removeFile("z.key");
  manager.rescan();
  EXPECT_EQ(identityOf(match(manager, std::string("a.example.com"))), "a-cn");
}

TEST(SniCertManager, RetainedStaleEntryDoesNotShadowALoadedPair) {
  TempDir dir;
  writePair(dir, "a", makeSelfSignedCertPem("a-cn", {"x.example.com"}));
  writePair(dir, "b", makeSelfSignedCertPem("b-cn", {"y.example.com"}));
  SniCertManager manager(optionsFor(dir));
  EXPECT_EQ(identityOf(match(manager, std::string("x.example.com"))), "a-cn");

  // The pairs swap names in one rescan window, and "a" is caught mid-write
  // with a key that no longer matches its cert. Its retained entry still
  // carries "x.example.com", which "b" now legitimately serves.
  dir.writeFile("a.pem", makeSelfSignedCertPem("a2-cn", {"y.example.com"}).certPem);
  bumpMtime(dir, "a.pem");
  writePair(dir, "b", makeSelfSignedCertPem("b2-cn", {"x.example.com"}));
  bumpMtime(dir, "b.pem");
  bumpMtime(dir, "b.key");
  manager.rescan();

  EXPECT_EQ(identityOf(match(manager, std::string("x.example.com"))), "b2-cn");
  // "a" never loaded, so the name it was rewritten to claim is unserved.
  EXPECT_FALSE(match(manager, std::string("y.example.com")).has_value());
}

TEST(SniCertManager, ResumptionResolvesSanOnlyCertWithoutCn) {
  TempDir dir;
  auto pair = makeSelfSignedCertPem("", {"a.example.com"});
  writePair(dir, "a", pair);
  SniCertManager manager(optionsFor(dir));

  // fizz stores getIdentity() (the subject DN for a CN-less cert) in the
  // ticket; resumption must resolve it though it is no SNI identity.
  auto expected = makeSelfCertFromPems(pair.certPem, pair.keyPem, "(test)")->getIdentity();
  auto cert = manager.getCert(expected);
  ASSERT_NE(cert, nullptr);
  EXPECT_EQ(cert->getIdentity(), expected);
}

TEST(SniCertManager, SharedCnResumptionWinnerIsLowestCertPath) {
  TempDir dir;
  writePair(dir, "a", makeSelfSignedCertPem("shared-cn", {"a.example.com"}, TestKeyType::EC256));
  writePair(dir, "b", makeSelfSignedCertPem("shared-cn", {"b.example.com"}, TestKeyType::RSA2048));
  SniCertManager manager(optionsFor(dir));

  auto cert = manager.getCert(std::string("shared-cn"));
  ASSERT_NE(cert, nullptr);
  // Deterministic winner: lowest certPath ("a.pem"), whose key is EC.
  EXPECT_EQ(cert->getSigSchemes().front(), fizz::SignatureScheme::ecdsa_secp256r1_sha256);
}

TEST(SniCertManager, ResumptionPrefersPrimaryIdentityOverAnotherCertsSan) {
  TempDir dir;
  // Disjoint identity sets, so both pairs load: only b's CN collides with a
  // SAN of a, and a CN with SANs present is no SNI identity of its own.
  writePair(dir, "a", makeSelfSignedCertPem("a-cn", {"a.example.com", "b.example.com"}));
  writePair(dir, "b", makeSelfSignedCertPem("b.example.com", {"c.example.com"}));
  SniCertManager manager(optionsFor(dir));

  // A ticket naming "b.example.com" was issued for b, whose getIdentity() it
  // stores; a merely carries that name as a SAN.
  auto cert = manager.getCert(std::string("b.example.com"));
  ASSERT_NE(cert, nullptr);
  EXPECT_EQ(cert->getIdentity(), "b.example.com");

  // SNI for the same name still resolves to a, which serves it.
  EXPECT_EQ(identityOf(match(manager, std::string("b.example.com"))), "a-cn");
}

TEST(SniCertManager, RescanRetriesLoadAfterRetainingStaleEntry) {
  namespace fs = std::filesystem;
  TempDir dir;
  writePair(dir, "a", makeSelfSignedCertPem("v1", {"a.example.com"}));
  SniCertManager manager(optionsFor(dir));
  EXPECT_EQ(identityOf(match(manager, std::string("a.example.com"))), "v1");

  // Break the key so the rescan retains the previous entry, which keeps the
  // pre-break mtimes.
  dir.writeFile("a.key", makeSelfSignedCertPem("other.example.com").keyPem);
  bumpMtime(dir, "a.key");
  auto brokenMtime = fs::last_write_time(fs::path(dir.path()) / "a.key");
  manager.rescan();
  EXPECT_EQ(identityOf(match(manager, std::string("a.example.com"))), "v1");

  // Repair with a new pair but leave the key's mtime at the broken value. The
  // retained entry's stale mtimes must still register this as a change,
  // otherwise the identity stays pinned to v1 forever.
  writePair(dir, "a", makeSelfSignedCertPem("v2", {"a.example.com"}));
  fs::last_write_time(fs::path(dir.path()) / "a.key", brokenMtime);
  manager.rescan();
  EXPECT_EQ(identityOf(match(manager, std::string("a.example.com"))), "v2");
}

TEST(SniCertManager, RescanRefreshesFallbackPair) {
  TempDir dir;
  writePair(dir, "a", makeSelfSignedCertPem("a.example.com"));
  TempDir fallbackDir;
  writePair(fallbackDir, "fb", makeSelfSignedCertPem("fallback-v1.example.com"));

  auto options = optionsFor(dir);
  options.fallbackCertFile = fallbackDir.path() + "/fb.pem";
  options.fallbackKeyFile = fallbackDir.path() + "/fb.key";
  SniCertManager manager(std::move(options));
  EXPECT_EQ(identityOf(match(manager, folly::none)), "fallback-v1.example.com");

  writePair(fallbackDir, "fb", makeSelfSignedCertPem("fallback-v2.example.com"));
  bumpMtime(fallbackDir, "fb.pem");
  manager.rescan();
  EXPECT_EQ(identityOf(match(manager, folly::none)), "fallback-v2.example.com");
}

TEST(SniCertManager, RescanEmptiedDirServesFallbackOnly) {
  TempDir dir;
  writePair(dir, "a", makeSelfSignedCertPem("a.example.com"));
  TempDir fallbackDir;
  writePair(fallbackDir, "fb", makeSelfSignedCertPem("fallback.example.com"));

  auto options = optionsFor(dir);
  options.fallbackCertFile = fallbackDir.path() + "/fb.pem";
  options.fallbackKeyFile = fallbackDir.path() + "/fb.key";
  SniCertManager manager(std::move(options));

  dir.removeFile("a.pem");
  dir.removeFile("a.key");
  manager.rescan();
  EXPECT_EQ(identityOf(match(manager, std::string("a.example.com"))), "fallback.example.com");
}

} // namespace
} // namespace openmoq::moqx::tls
