/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "tls/DirectoryCertLoader.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "TestCertUtils.h"

namespace openmoq::moqx::tls {
namespace {

using test::kTestCert2Pem;
using test::kTestCertPem;
using test::kTestKey2Pem;
using test::kTestKeyPem;
using test::TempCertDir;
using ::testing::HasSubstr;

TEST(DirectoryCertLoader, LoadsSingleCert) {
  TempCertDir dir;
  dir.writeCert("test", kTestCertPem, kTestKeyPem);

  DirectoryCertLoader loader(dir.path(), "");
  auto result = loader.load();
  ASSERT_TRUE(result.hasValue()) << result.error();

  EXPECT_EQ(result.value().certs.size(), 1);
  EXPECT_FALSE(result.value().defaultIdentity.empty());
}

TEST(DirectoryCertLoader, LoadsMultipleCerts) {
  TempCertDir dir;
  dir.writeCert("alpha", kTestCertPem, kTestKeyPem);
  dir.writeCert("beta", kTestCert2Pem, kTestKey2Pem);

  DirectoryCertLoader loader(dir.path(), "");
  auto result = loader.load();
  ASSERT_TRUE(result.hasValue()) << result.error();

  EXPECT_EQ(result.value().certs.size(), 2);
  // Default should be the first cert (alphabetically sorted: alpha before beta)
  EXPECT_FALSE(result.value().defaultIdentity.empty());
}

TEST(DirectoryCertLoader, EmptyDirectory) {
  TempCertDir dir;

  DirectoryCertLoader loader(dir.path(), "");
  auto result = loader.load();
  ASSERT_TRUE(result.hasError());
  EXPECT_THAT(result.error(), HasSubstr("No certificate pairs found"));
}

TEST(DirectoryCertLoader, NonexistentDirectory) {
  DirectoryCertLoader loader("/nonexistent/dir", "");
  auto result = loader.load();
  ASSERT_TRUE(result.hasError());
  EXPECT_THAT(result.error(), HasSubstr("Failed to read directory"));
}

TEST(DirectoryCertLoader, CrtWithoutMatchingKey) {
  TempCertDir dir;
  dir.writeCertOnly("orphan", kTestCertPem);

  DirectoryCertLoader loader(dir.path(), "");
  auto result = loader.load();
  ASSERT_TRUE(result.hasError());
  EXPECT_THAT(result.error(), HasSubstr("Failed to read key file"));
}

TEST(DirectoryCertLoader, InvalidDefaultCertIdentity) {
  TempCertDir dir;
  dir.writeCert("test", kTestCertPem, kTestKeyPem);

  DirectoryCertLoader loader(dir.path(), "nonexistent.example.com");
  auto result = loader.load();
  ASSERT_TRUE(result.hasError());
  EXPECT_THAT(result.error(), HasSubstr("not found"));
}

TEST(DirectoryCertLoader, InvalidPem) {
  TempCertDir dir;
  dir.writeCert("bad", "not a cert", "not a key");

  DirectoryCertLoader loader(dir.path(), "");
  auto result = loader.load();
  ASSERT_TRUE(result.hasError());
  EXPECT_THAT(result.error(), HasSubstr("Failed to parse certificate"));
}

} // namespace
} // namespace openmoq::moqx::tls
