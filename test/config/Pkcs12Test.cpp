/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "config/Pkcs12.h"
#include "Pkcs12TestUtils.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

using namespace openmoq::moqx::config;
using namespace openmoq::moqx::config::test;
using ::testing::HasSubstr;

TEST(Pkcs12, TranscodeHappyPath) {
  auto der = makeSelfSignedPkcs12Der("s3cret");
  auto result = transcodePkcs12(der, "s3cret");
  ASSERT_FALSE(result.hasError()) << result.error();
  EXPECT_THAT(result->certChainPem, HasSubstr("BEGIN CERTIFICATE"));
  EXPECT_THAT(result->keyPem, HasSubstr("PRIVATE KEY"));
  // Decrypted key must not be password-encrypted.
  EXPECT_THAT(result->keyPem, ::testing::Not(HasSubstr("ENCRYPTED")));
}

TEST(Pkcs12, TranscodePasswordLess) {
  auto der = makeSelfSignedPkcs12Der("");
  auto result = transcodePkcs12(der, "");
  ASSERT_FALSE(result.hasError()) << result.error();
  EXPECT_THAT(result->certChainPem, HasSubstr("BEGIN CERTIFICATE"));
  EXPECT_THAT(result->keyPem, HasSubstr("PRIVATE KEY"));
}

TEST(Pkcs12, TranscodeWrongPassword) {
  auto der = makeSelfSignedPkcs12Der("s3cret");
  auto result = transcodePkcs12(der, "wrong");
  ASSERT_TRUE(result.hasError());
  EXPECT_THAT(result.error(), HasSubstr("wrong password"));
}

TEST(Pkcs12, TranscodeEmptyInput) {
  auto result = transcodePkcs12("", "x");
  ASSERT_TRUE(result.hasError());
  EXPECT_THAT(result.error(), HasSubstr("empty"));
}

TEST(Pkcs12, TranscodeGarbageBytes) {
  auto result = transcodePkcs12("this is not a pkcs12 bundle", "x");
  ASSERT_TRUE(result.hasError());
  EXPECT_THAT(result.error(), HasSubstr("not a valid"));
}

TEST(Pkcs12, LoadFromFileHappyPath) {
  auto der = makeSelfSignedPkcs12Der("pw123");
  TempFile p12(der, ".p12");
  auto result = loadPkcs12File(p12.path(), "pw123");
  ASSERT_FALSE(result.hasError()) << result.error();
  EXPECT_THAT(result->certChainPem, HasSubstr("BEGIN CERTIFICATE"));
}

TEST(Pkcs12, LoadFromFileMissing) {
  auto result = loadPkcs12File("/no/such/bundle.p12", "");
  ASSERT_TRUE(result.hasError());
  EXPECT_THAT(result.error(), HasSubstr("failed to read"));
}
