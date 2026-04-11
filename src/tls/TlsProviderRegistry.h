/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include <folly/Expected.h>
#include <folly/container/F14Map.h>

#include "config/loader/ParsedConfig.h"

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
