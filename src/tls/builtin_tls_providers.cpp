#include "o_rly/tls/builtin_tls_providers.h"

#include "o_rly/config/loader/parsed_config.h"
#include "o_rly/tls/directory_cert_loader.h"
#include "o_rly/tls/file_cert_loader.h"
#include "o_rly/tls/insecure_cert_provider.h"
#include "o_rly/tls/tls_provider_registry.h"

namespace openmoq::o_rly::tls {

void registerBuiltinTlsProviders(TlsProviderRegistry& registry) {
  registry.registerProvider(
      config::ParsedTlsInsecure::Tag{}.name(),
      [](const config::ParsedTlsMode&)
          -> folly::Expected<std::shared_ptr<TlsCertProvider>, std::string> {
        return std::make_shared<InsecureCertProvider>();
      }
  );

  registry.registerProvider(
      config::ParsedTlsFile::Tag{}.name(),
      [](const config::ParsedTlsMode& tls
      ) -> folly::Expected<std::shared_ptr<TlsCertProvider>, std::string> {
        return tls.visit([](const auto& variant)
                             -> folly::Expected<std::shared_ptr<TlsCertProvider>, std::string> {
          using T = std::decay_t<decltype(variant)>;
          if constexpr (std::is_same_v<T, config::ParsedTlsFile>) {
            if (variant.cert_file.value().empty()) {
              return folly::makeUnexpected(std::string("cert_file is required"));
            }
            if (variant.key_file.value().empty()) {
              return folly::makeUnexpected(std::string("key_file is required"));
            }
            return std::make_shared<FileCertLoader>(
                variant.cert_file.value(), variant.key_file.value()
            );
          } else {
            return folly::makeUnexpected(
                std::string("'file' factory called with wrong TLS variant")
            );
          }
        });
      }
  );

  registry.registerProvider(
      config::ParsedTlsDirectory::Tag{}.name(),
      [](const config::ParsedTlsMode& tls
      ) -> folly::Expected<std::shared_ptr<TlsCertProvider>, std::string> {
        return tls.visit([](const auto& variant)
                             -> folly::Expected<std::shared_ptr<TlsCertProvider>, std::string> {
          using T = std::decay_t<decltype(variant)>;
          if constexpr (std::is_same_v<T, config::ParsedTlsDirectory>) {
            if (variant.cert_dir.value().empty()) {
              return folly::makeUnexpected(std::string("cert_dir is required"));
            }
            return std::make_shared<DirectoryCertLoader>(
                variant.cert_dir.value(), variant.default_cert.value().value_or("")
            );
          } else {
            return folly::makeUnexpected(
                std::string("'directory' factory called with wrong TLS variant")
            );
          }
        });
      }
  );
}

} // namespace openmoq::o_rly::tls
