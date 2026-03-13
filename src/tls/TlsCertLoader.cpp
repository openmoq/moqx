/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <fizz/server/DefaultCertManager.h>

#include "FizzContextFactory.h"
#include "TlsCertLoader.h"

namespace openmoq::moqx::tls {

folly::Expected<std::shared_ptr<const fizz::server::FizzServerContext>, std::string>
TlsCertLoader::createContext(
    const std::vector<std::string>& alpns,
    const std::vector<TicketSeed>& ticketSeeds
) const {
  auto loaded = load();
  if (loaded.hasError()) {
    return folly::makeUnexpected(loaded.error());
  }

  auto certManager = std::make_shared<fizz::server::DefaultCertManager>();
  for (auto& entry : loaded.value().certs) {
    if (entry.identity == loaded.value().defaultIdentity) {
      certManager->addCertAndSetDefault(std::move(entry.cert));
    } else {
      certManager->addCert(std::move(entry.cert));
    }
  }

  return buildStandardFizzContext(std::move(certManager), alpns, ticketSeeds);
}

} // namespace openmoq::moqx::tls
