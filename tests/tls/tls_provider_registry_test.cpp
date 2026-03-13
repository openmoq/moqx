#include <moqx/tls/tls_provider_registry.h>

#include <moqx/config/loader/parsed_config.h>
#include <moqx/tls/tls_cert_loader.h>

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
