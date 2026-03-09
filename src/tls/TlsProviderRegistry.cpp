/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "TlsProviderRegistry.h"

namespace openmoq::moqx::tls {

void TlsProviderRegistry::registerProvider(std::string type, TlsProviderFactory factory) {
  factories_.emplace(std::move(type), std::move(factory));
}

const TlsProviderFactory* TlsProviderRegistry::getFactory(const std::string& type) const {
  auto it = factories_.find(type);
  if (it == factories_.end()) {
    return nullptr;
  }
  return &it->second;
}

std::vector<std::string> TlsProviderRegistry::registeredTypes() const {
  std::vector<std::string> types;
  types.reserve(factories_.size());
  for (const auto& [type, _] : factories_) {
    types.push_back(type);
  }
  return types;
}

} // namespace openmoq::moqx::tls
