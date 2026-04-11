/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <tls/InsecureCertProvider.h>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

namespace openmoq::moqx::tls {
namespace {

TEST(InsecureCertProvider, CreateContextReturnsValidContext) {
  InsecureCertProvider provider;
  auto result = provider.createContext({"h3"}, {});
  ASSERT_TRUE(result.hasValue()) << result.error();
  EXPECT_NE(result.value(), nullptr);

  const auto& alpns = result.value()->getSupportedAlpns();
  ASSERT_FALSE(alpns.empty());
  EXPECT_EQ(alpns[0], "h3");
}

TEST(InsecureCertProvider, CreateContextWithMoqtAlpns) {
  InsecureCertProvider provider;
  auto result = provider.createContext({"h3", "moq-00"}, {});
  ASSERT_TRUE(result.hasValue()) << result.error();
  EXPECT_NE(result.value(), nullptr);

  const auto& alpns = result.value()->getSupportedAlpns();
  EXPECT_GE(alpns.size(), 2u);
}

} // namespace
} // namespace openmoq::moqx::tls
