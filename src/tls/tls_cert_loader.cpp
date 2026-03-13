#include "moqx/tls/tls_cert_loader.h"

#include <fizz/server/DefaultCertManager.h>

#include "moqx/tls/fizz_context_factory.h"

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
