#pragma once

#include "o_rly/tls/tls_cert_loader.h"

namespace openmoq::o_rly::tls {

/// TLS provider using compiled-in self-signed certs for development.
class InsecureCertProvider : public TlsCertProvider {
public:
  folly::Expected<std::shared_ptr<const fizz::server::FizzServerContext>, std::string>
  createContext(
      const std::vector<std::string>& alpns,
      const std::vector<TicketSeed>& ticketSeeds = {}
  ) const override;
};

} // namespace openmoq::o_rly::tls
