/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <tls/DirectoryCertLoader.h>
#include <tls/FileCertLoader.h>
#include <tls/FizzContextFactory.h>

#include <fizz/server/DefaultCertManager.h>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "TestCertUtils.h"
#include "util/TempDir.h"

namespace openmoq::moqx::tls {
namespace {

using openmoq::moqx::test::util::TempDir;
using test::kTestCert2Pem;
using test::kTestCertPem;
using test::kTestKey2Pem;
using test::kTestKeyPem;
using ::testing::HasSubstr;

TEST(BuildAlpns, IncludesH3) {
  auto alpns = buildAlpns("");
  ASSERT_FALSE(alpns.empty());
  EXPECT_EQ(alpns[0], "h3");
}

TEST(BuildAlpns, IncludesMoqtVersions) {
  auto alpns = buildAlpns("14,16");
  EXPECT_GE(alpns.size(), 2u);
  EXPECT_EQ(alpns[0], "h3");
}

TEST(BuildStandardFizzContext, ValidCertManager) {
  TempDir dir;
  dir.writeCert("test", kTestCertPem, kTestKeyPem);

  FileCertLoader loader(dir.filePath("test.crt"), dir.filePath("test.key"));
  auto loaded = loader.load();
  ASSERT_TRUE(loaded.hasValue()) << loaded.error();

  auto certManager = std::make_shared<fizz::server::DefaultCertManager>();
  for (auto& entry : loaded.value().certs) {
    certManager->addCertAndSetDefault(std::move(entry.cert));
  }

  auto result = buildStandardFizzContext(std::move(certManager), {"h3"});
  ASSERT_TRUE(result.hasValue()) << result.error();
  EXPECT_NE(result.value(), nullptr);

  const auto& alpns = result.value()->getSupportedAlpns();
  ASSERT_FALSE(alpns.empty());
  EXPECT_EQ(alpns[0], "h3");
}

TEST(FileCertLoader, CreateContextEndToEnd) {
  TempDir dir;
  dir.writeCert("test", kTestCertPem, kTestKeyPem);

  FileCertLoader loader(dir.filePath("test.crt"), dir.filePath("test.key"));
  auto result = loader.createContext({"h3"});
  ASSERT_TRUE(result.hasValue()) << result.error();
  EXPECT_NE(result.value(), nullptr);
  EXPECT_FALSE(result.value()->getSupportedAlpns().empty());
}

TEST(DirectoryCertLoader, CreateContextEndToEnd) {
  TempDir dir;
  dir.writeCert("alpha", kTestCertPem, kTestKeyPem);
  dir.writeCert("beta", kTestCert2Pem, kTestKey2Pem);

  DirectoryCertLoader loader(dir.path(), "");
  auto result = loader.createContext({"h3"});
  ASSERT_TRUE(result.hasValue()) << result.error();
  EXPECT_NE(result.value(), nullptr);
}

TEST(FileCertLoader, CreateContextInvalidCert) {
  FileCertLoader loader("/nonexistent/cert.pem", "/nonexistent/key.pem");
  auto result = loader.createContext({"h3"});
  ASSERT_TRUE(result.hasError());
  EXPECT_THAT(result.error(), HasSubstr("cert"));
}

} // namespace
} // namespace openmoq::moqx::tls
