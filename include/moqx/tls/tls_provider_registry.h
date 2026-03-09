#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <folly/Expected.h>
#include <folly/container/F14Map.h>

#include "moqx/config/loader/parsed_config.h"

namespace openmoq::moqx::tls {

class TlsCertProvider;

using TlsProviderFactory =
    std::function<folly::Expected<std::shared_ptr<TlsCertProvider>, std::string>(
        const config::ParsedTlsMode& tls
    )>;

class TlsProviderRegistry {
public:
  void registerProvider(std::string type, TlsProviderFactory factory);
  const TlsProviderFactory* getFactory(const std::string& type) const;
  std::vector<std::string> registeredTypes() const;

private:
  folly::F14FastMap<std::string, TlsProviderFactory> factories_;
};

} // namespace openmoq::moqx::tls
