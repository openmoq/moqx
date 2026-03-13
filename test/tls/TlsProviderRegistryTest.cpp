/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <tls/TlsProviderRegistry.h>

#include <config/loader/ParsedConfig.h>
#include <tls/TlsCertLoader.h>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

namespace openmoq::moqx::tls {
namespace {

using ::testing::HasSubstr;
using ::testing::UnorderedElementsAre;

// Dummy provider for testing
class DummyProvider : public TlsCertProvider {
public:
  folly::Expected<std::shared_ptr<const fizz::server::FizzServerContext>, std::string>
  createContext(const std::vector<std::string>&, const std::vector<TicketSeed>&) const override {
    return folly::makeUnexpected(std::string("dummy"));
  }

  folly::Expected<std::string, std::string> getKeyPath() const override {
    return folly::makeUnexpected(std::string("dummy"));
  }

  folly::Expected<std::string, std::string> getCertPath() const override {
    return folly::makeUnexpected(std::string("dummy"));
  }
};

TEST(TlsProviderRegistry, RegisterAndLookup) {
  TlsProviderRegistry registry;
  registry.registerProvider(
      "test",
      [](const config::ParsedTlsMode&)
          -> folly::Expected<std::shared_ptr<TlsCertProvider>, std::string> {
        return std::make_shared<DummyProvider>();
      }
  );

  auto* factory = registry.getFactory("test");
  ASSERT_NE(factory, nullptr);

  config::ParsedTlsMode tls{config::ParsedTlsInsecure{}};
  auto result = (*factory)(tls);
  ASSERT_TRUE(result.hasValue());
  EXPECT_NE(result.value(), nullptr);
}

TEST(TlsProviderRegistry, MissingTypeReturnsNull) {
  TlsProviderRegistry registry;
  EXPECT_EQ(registry.getFactory("nonexistent"), nullptr);
}

TEST(TlsProviderRegistry, RegisteredTypes) {
  TlsProviderRegistry registry;
  registry.registerProvider(
      "alpha",
      [](const config::ParsedTlsMode&)
          -> folly::Expected<std::shared_ptr<TlsCertProvider>, std::string> {
        return std::make_shared<DummyProvider>();
      }
  );
  registry.registerProvider(
      "beta",
      [](const config::ParsedTlsMode&)
          -> folly::Expected<std::shared_ptr<TlsCertProvider>, std::string> {
        return std::make_shared<DummyProvider>();
      }
  );

  auto types = registry.registeredTypes();
  EXPECT_THAT(types, UnorderedElementsAre("alpha", "beta"));
}

TEST(TlsProviderRegistry, FactoryErrorPropagation) {
  TlsProviderRegistry registry;
  registry.registerProvider(
      "failing",
      [](const config::ParsedTlsMode&)
          -> folly::Expected<std::shared_ptr<TlsCertProvider>, std::string> {
        return folly::makeUnexpected(std::string("something went wrong"));
      }
  );

  auto* factory = registry.getFactory("failing");
  ASSERT_NE(factory, nullptr);

  config::ParsedTlsMode tls{config::ParsedTlsInsecure{}};
  auto result = (*factory)(tls);
  ASSERT_TRUE(result.hasError());
  EXPECT_THAT(result.error(), HasSubstr("something went wrong"));
}

} // namespace
} // namespace openmoq::moqx::tls
