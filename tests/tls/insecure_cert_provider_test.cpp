#include <o_rly/tls/insecure_cert_provider.h>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

namespace openmoq::o_rly::tls {
namespace {

TEST(InsecureCertProvider, CreateContextReturnsValidContext) {
  InsecureCertProvider provider;
  auto result = provider.createContext({"h3"});
  ASSERT_TRUE(result.hasValue()) << result.error();
  EXPECT_NE(result.value(), nullptr);

  const auto& alpns = result.value()->getSupportedAlpns();
  ASSERT_FALSE(alpns.empty());
  EXPECT_EQ(alpns[0], "h3");
}

TEST(InsecureCertProvider, CreateContextWithMoqtAlpns) {
  InsecureCertProvider provider;
  auto result = provider.createContext({"h3", "moq-00"});
  ASSERT_TRUE(result.hasValue()) << result.error();
  EXPECT_NE(result.value(), nullptr);

  const auto& alpns = result.value()->getSupportedAlpns();
  EXPECT_GE(alpns.size(), 2u);
}

} // namespace
} // namespace openmoq::o_rly::tls
