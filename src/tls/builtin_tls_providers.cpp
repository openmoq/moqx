#include "moqx/tls/builtin_tls_providers.h"

#include "moqx/config/loader/parsed_config.h"
#include "moqx/tls/directory_cert_loader.h"
#include "moqx/tls/file_cert_loader.h"
#include "moqx/tls/insecure_cert_provider.h"
#include "moqx/tls/tls_provider_registry.h"

namespace openmoq::moqx::tls {

void registerBuiltinTlsProviders(TlsProviderRegistry& registry) {
  registry.registerProvider(config::ParsedTlsInsecure::Tag{}.name(), makeInsecureFactory());
  registry.registerProvider(config::ParsedTlsFile::Tag{}.name(), makeFileFactory());
  registry.registerProvider(config::ParsedTlsDirectory::Tag{}.name(), makeDirectoryFactory());
}

TlsProviderFactory makeInsecureFactory(std::function<std::shared_ptr<TlsCertProvider>()> creator) {
  if (!creator) {
    creator = [] { return std::make_shared<InsecureCertProvider>(); };
  }

  return
      [creator = std::move(creator)](const config::ParsedTlsMode&)
          -> folly::Expected<std::shared_ptr<TlsCertProvider>, std::string> { return creator(); };
}

TlsProviderFactory
makeFileFactory(std::function<std::shared_ptr<TlsCertProvider>(std::string, std::string)> creator) {
  using namespace std::literals;

  if (!creator) {
    creator = [](std::string cert, std::string key) {
      return std::make_shared<FileCertLoader>(std::move(cert), std::move(key));
    };
  }

  return [creator = std::move(creator)](const config::ParsedTlsMode& tls
         ) -> folly::Expected<std::shared_ptr<TlsCertProvider>, std::string> {
    return tls.visit(
        [&creator](const auto& variant
        ) -> folly::Expected<std::shared_ptr<TlsCertProvider>, std::string> {
          using T = std::decay_t<decltype(variant)>;

          if constexpr (std::is_same_v<T, config::ParsedTlsFile>) {
            if (variant.cert_file.value().empty()) {
              return folly::makeUnexpected("cert_file is required"s);
            }

            if (variant.key_file.value().empty()) {
              return folly::makeUnexpected("key_file is required"s);
            }

            return creator(variant.cert_file.value(), variant.key_file.value());
          } else {
            return folly::makeUnexpected("'file' factory called with wrong TLS variant"s);
          }
        }
    );
  };
}

TlsProviderFactory makeDirectoryFactory(
    std::function<std::shared_ptr<TlsCertProvider>(std::string, std::string)> creator
) {
  using namespace std::literals;

  if (!creator) {
    creator = [](std::string certDir, std::string defaultCert) {
      return std::make_shared<DirectoryCertLoader>(std::move(certDir), std::move(defaultCert));
    };
  }

  return [creator = std::move(creator)](const config::ParsedTlsMode& tls
         ) -> folly::Expected<std::shared_ptr<TlsCertProvider>, std::string> {
    return tls.visit(
        [&creator](const auto& variant
        ) -> folly::Expected<std::shared_ptr<TlsCertProvider>, std::string> {
          using T = std::decay_t<decltype(variant)>;

          if constexpr (std::is_same_v<T, config::ParsedTlsDirectory>) {
            if (variant.cert_dir.value().empty()) {
              return folly::makeUnexpected("cert_dir is required"s);
            }

            return creator(variant.cert_dir.value(), variant.default_cert.value().value_or(""s));
          } else {
            return folly::makeUnexpected("'directory' factory called with wrong TLS variant"s);
          }
        }
    );
  };
}

} // namespace openmoq::moqx::tls
