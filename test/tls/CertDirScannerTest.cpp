/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "tls/CertDirScanner.h"

#include "CertTestUtils.h"
#include "tls/CertLoader.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

namespace openmoq::moqx::tls {
namespace {

using ::testing::Contains;
using ::testing::HasSubstr;
using ::testing::IsEmpty;
using ::testing::UnorderedElementsAre;

using moqx::test::makeSelfSignedCertPem;
using moqx::test::TempDir;

std::vector<CertDirEntry> scanStrict(const TempDir& dir) {
  std::vector<std::string> warnings;
  auto entries = scanCertDir(dir.path(), /*strict=*/true, warnings);
  EXPECT_THAT(warnings, IsEmpty());
  return entries;
}

TEST(CertDirScanner, EmptyDir) {
  TempDir dir;
  EXPECT_THAT(scanStrict(dir), IsEmpty());
}

TEST(CertDirScanner, PairsWithSanIdentities) {
  TempDir dir;
  writePair(dir, "a", makeSelfSignedCertPem("a.example.com", {"a.example.com", "alt.example.com"}));
  writePair(dir, "b", makeSelfSignedCertPem("ignored-cn", {"b.example.com"}));

  auto entries = scanStrict(dir);
  ASSERT_EQ(entries.size(), 2u);
  // pemBases is sorted, so "a" precedes "b".
  EXPECT_THAT(entries[0].identities, UnorderedElementsAre("a.example.com", "alt.example.com"));
  // SANs are authoritative: the CN must not appear when SANs exist.
  EXPECT_THAT(entries[1].identities, UnorderedElementsAre("b.example.com"));
  // The CN is still recorded as the primary identity (ticket resumption key).
  EXPECT_EQ(entries[1].primaryIdentity, "ignored-cn");
  EXPECT_THAT(entries[0].certPath, HasSubstr("a.pem"));
  EXPECT_THAT(entries[0].keyPath, HasSubstr("a.key"));
}

TEST(CertDirScanner, SanOnlyCertWithoutCnGetsSubjectPrimaryIdentity) {
  TempDir dir;
  auto pair = makeSelfSignedCertPem("", {"a.example.com"});
  writePair(dir, "a", pair);

  auto entries = scanStrict(dir);
  ASSERT_EQ(entries.size(), 1u);
  // The resumption key must equal what fizz stores in the ticket:
  // SelfCert::getIdentity(), which is the subject DN when there is no CN.
  auto cert = makeSelfCertFromPems(pair.certPem, pair.keyPem, "(test)");
  EXPECT_FALSE(entries[0].primaryIdentity.empty());
  EXPECT_EQ(entries[0].primaryIdentity, normalizeLookupKey(cert->getIdentity()));
}

TEST(CertDirScanner, CnFallbackWhenNoSans) {
  TempDir dir;
  writePair(dir, "only-cn", makeSelfSignedCertPem("cn.example.com"));

  auto entries = scanStrict(dir);
  ASSERT_EQ(entries.size(), 1u);
  EXPECT_THAT(entries[0].identities, UnorderedElementsAre("cn.example.com"));
}

TEST(CertDirScanner, IdentitiesLowercased) {
  TempDir dir;
  writePair(dir, "upper", makeSelfSignedCertPem("MiXeD.ExAmPlE.CoM"));

  auto entries = scanStrict(dir);
  ASSERT_EQ(entries.size(), 1u);
  EXPECT_THAT(entries[0].identities, UnorderedElementsAre("mixed.example.com"));
}

TEST(CertDirScanner, WildcardNormalized) {
  TempDir dir;
  writePair(dir, "wild", makeSelfSignedCertPem("*.example.com", {"*.example.com"}));

  auto entries = scanStrict(dir);
  ASSERT_EQ(entries.size(), 1u);
  EXPECT_THAT(entries[0].identities, UnorderedElementsAre(".example.com"));
}

TEST(CertDirScanner, NestedWildcardRejected) {
  TempDir dir;
  writePair(dir, "bad", makeSelfSignedCertPem("*.a.example.com", {"*.*.example.com"}));

  std::vector<std::string> warnings;
  EXPECT_THROW(scanCertDir(dir.path(), true, warnings), std::runtime_error);

  warnings.clear();
  auto entries = scanCertDir(dir.path(), false, warnings);
  EXPECT_THAT(entries, IsEmpty());
  EXPECT_THAT(warnings, Contains(HasSubstr("invalid identity")));
}

TEST(CertDirScanner, OrphanPemStrictThrows) {
  TempDir dir;
  dir.writeFile("lonely.pem", makeSelfSignedCertPem("x.example.com").certPem);

  std::vector<std::string> warnings;
  EXPECT_THROW(scanCertDir(dir.path(), true, warnings), std::runtime_error);

  warnings.clear();
  auto entries = scanCertDir(dir.path(), false, warnings);
  EXPECT_THAT(entries, IsEmpty());
  EXPECT_THAT(warnings, Contains(HasSubstr("orphan cert")));
}

TEST(CertDirScanner, OrphanKeyStrictThrows) {
  TempDir dir;
  dir.writeFile("lonely.key", makeSelfSignedCertPem("x.example.com").keyPem);

  std::vector<std::string> warnings;
  EXPECT_THROW(scanCertDir(dir.path(), true, warnings), std::runtime_error);

  warnings.clear();
  auto entries = scanCertDir(dir.path(), false, warnings);
  EXPECT_THAT(entries, IsEmpty());
  EXPECT_THAT(warnings, Contains(HasSubstr("orphan key")));
}

TEST(CertDirScanner, GarbageCertRejected) {
  TempDir dir;
  auto good = makeSelfSignedCertPem("good.example.com");
  writePair(dir, "good", good);
  dir.writeFile("bad.pem", "not a certificate");
  dir.writeFile("bad.key", good.keyPem);

  std::vector<std::string> warnings;
  EXPECT_THROW(scanCertDir(dir.path(), true, warnings), std::runtime_error);

  warnings.clear();
  auto entries = scanCertDir(dir.path(), false, warnings);
  ASSERT_EQ(entries.size(), 1u);
  EXPECT_THAT(entries[0].identities, UnorderedElementsAre("good.example.com"));
  EXPECT_THAT(warnings, Contains(HasSubstr("bad.pem")));
}

TEST(CertDirScanner, DuplicateIdentityNamesBothFiles) {
  TempDir dir;
  writePair(dir, "first", makeSelfSignedCertPem("dup.example.com"));
  writePair(dir, "second", makeSelfSignedCertPem("dup.example.com"));

  std::vector<std::string> warnings;
  try {
    scanCertDir(dir.path(), true, warnings);
    FAIL() << "expected duplicate identity to throw";
  } catch (const std::runtime_error& e) {
    EXPECT_THAT(e.what(), HasSubstr("first.pem"));
    EXPECT_THAT(e.what(), HasSubstr("second.pem"));
    EXPECT_THAT(e.what(), HasSubstr("dup.example.com"));
  }

  // Non-strict: first claimant (sorted order) wins.
  auto entries = scanCertDir(dir.path(), false, warnings);
  ASSERT_EQ(entries.size(), 1u);
  EXPECT_THAT(entries[0].certPath, HasSubstr("first.pem"));
}

TEST(CertDirScanner, IgnoresOtherExtensions) {
  TempDir dir;
  writePair(dir, "a", makeSelfSignedCertPem("a.example.com"));
  dir.writeFile("README.md", "docs");
  dir.writeFile("bundle.p12", "binary");

  auto entries = scanStrict(dir);
  EXPECT_EQ(entries.size(), 1u);
}

// --- previous-scan reuse ---

// Guarantee a strictly newer mtime than a previous write of the same path.
void bumpMtime(const TempDir& dir, const std::string& filename) {
  namespace fs = std::filesystem;
  auto p = std::filesystem::path(dir.path()) / filename;
  fs::last_write_time(p, fs::last_write_time(p) + std::chrono::seconds(2));
}

TEST(CertDirScanner, PreviousEntryReusedWhenMtimesUnchanged) {
  namespace fs = std::filesystem;
  TempDir dir;
  writePair(dir, "a", makeSelfSignedCertPem("a.example.com"));
  auto previous = scanStrict(dir);
  ASSERT_EQ(previous.size(), 1u);

  // Overwrite with garbage but restore the mtime: a scan passing `previous`
  // must copy the entry forward without reading the file.
  auto pemPath = fs::path(dir.path()) / "a.pem";
  auto mtime = fs::last_write_time(pemPath);
  dir.writeFile("a.pem", "not a certificate");
  fs::last_write_time(pemPath, mtime);

  std::vector<std::string> warnings;
  auto entries = scanCertDir(dir.path(), /*strict=*/false, warnings, &previous);
  EXPECT_THAT(warnings, IsEmpty());
  ASSERT_EQ(entries.size(), 1u);
  EXPECT_EQ(entries[0].identities, previous[0].identities);
  EXPECT_EQ(entries[0].primaryIdentity, previous[0].primaryIdentity);
}

TEST(CertDirScanner, PreviousEntryReparsedWhenMtimeChanges) {
  TempDir dir;
  writePair(dir, "a", makeSelfSignedCertPem("a.example.com"));
  auto previous = scanStrict(dir);

  dir.writeFile("a.pem", "not a certificate");
  bumpMtime(dir, "a.pem");

  std::vector<std::string> warnings;
  auto entries = scanCertDir(dir.path(), /*strict=*/false, warnings, &previous);
  EXPECT_THAT(entries, IsEmpty());
  EXPECT_THAT(warnings, Contains(HasSubstr("a.pem")));
}

TEST(CertDirScanner, PreviousEntriesStillTripDuplicateDetection) {
  TempDir dir;
  writePair(dir, "second", makeSelfSignedCertPem("dup.example.com"));
  auto previous = scanStrict(dir);

  // "first" sorts before "second", but the incumbent claims its identity
  // ahead of any newcomer; the arrival is dropped with a duplicate warning.
  writePair(dir, "first", makeSelfSignedCertPem("dup.example.com"));

  std::vector<std::string> warnings;
  auto entries = scanCertDir(dir.path(), /*strict=*/false, warnings, &previous);
  ASSERT_EQ(entries.size(), 1u);
  EXPECT_THAT(entries[0].certPath, HasSubstr("second.pem"));
  EXPECT_THAT(warnings, Contains(HasSubstr("duplicate identity")));
}

TEST(CertDirScanner, PreviousEntryForRemovedFileIsIgnored) {
  TempDir dir;
  writePair(dir, "a", makeSelfSignedCertPem("a.example.com"));
  writePair(dir, "b", makeSelfSignedCertPem("b.example.com"));
  auto previous = scanStrict(dir);
  ASSERT_EQ(previous.size(), 2u);

  dir.removeFile("b.pem");
  dir.removeFile("b.key");

  std::vector<std::string> warnings;
  auto entries = scanCertDir(dir.path(), /*strict=*/false, warnings, &previous);
  EXPECT_THAT(warnings, IsEmpty());
  ASSERT_EQ(entries.size(), 1u);
  EXPECT_THAT(entries[0].identities, UnorderedElementsAre("a.example.com"));
}

TEST(CertDirScanner, UnreadableDir) {
  std::vector<std::string> warnings;
  EXPECT_THROW(scanCertDir("/nonexistent/moqx-scanner-test", true, warnings), std::runtime_error);
  // Non-strict too: an unopenable dir is a scan-level failure (indistinguishable
  // from an empty dir), so the caller must keep its previous state.
  EXPECT_THROW(scanCertDir("/nonexistent/moqx-scanner-test", false, warnings), std::runtime_error);
}

} // namespace
} // namespace openmoq::moqx::tls
