#include <moqx/tls/file_cert_loader.h>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include "test_cert_utils.h"

namespace openmoq::moqx::tls {
namespace {

using test::kTestCertPem;
using test::kTestKeyPem;
using test::TempCertDir;
using ::testing::HasSubstr;

TEST(FileCertLoader, LoadsValidCertKeyPair) {
  TempCertDir dir;
  dir.writeCert("test", kTestCertPem, kTestKeyPem);

  FileCertLoader loader(dir.filePath("test.crt"), dir.filePath("test.key"));
  auto result = loader.load();
  ASSERT_TRUE(result.hasValue()) << result.error();

  EXPECT_EQ(result.value().certs.size(), 1);
  EXPECT_FALSE(result.value().defaultIdentity.empty());
  EXPECT_EQ(result.value().certs[0].identity, result.value().defaultIdentity);
  EXPECT_NE(result.value().certs[0].cert, nullptr);
}

TEST(FileCertLoader, MissingCertFile) {
  FileCertLoader loader("/nonexistent/cert.pem", "/nonexistent/key.pem");
  auto result = loader.load();
  ASSERT_TRUE(result.hasError());
  EXPECT_THAT(result.error(), HasSubstr("cert"));
}

TEST(FileCertLoader, MissingKeyFile) {
  TempCertDir dir;
  dir.writeCertOnly("test", kTestCertPem);

  FileCertLoader loader(dir.filePath("test.crt"), "/nonexistent/key.pem");
  auto result = loader.load();
  ASSERT_TRUE(result.hasError());
  EXPECT_THAT(result.error(), HasSubstr("key"));
}

TEST(FileCertLoader, InvalidPem) {
  TempCertDir dir;
  dir.writeCert("test", "not a cert", "not a key");

  FileCertLoader loader(dir.filePath("test.crt"), dir.filePath("test.key"));
  auto result = loader.load();
  ASSERT_TRUE(result.hasError());
  EXPECT_THAT(result.error(), HasSubstr("Failed to parse certificate"));
}

} // namespace
} // namespace openmoq::moqx::tls
