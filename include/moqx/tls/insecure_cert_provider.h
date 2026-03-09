#pragma once

#include "moqx/tls/tls_cert_loader.h"

namespace openmoq::moqx::tls {

/// TLS provider using compiled-in self-signed certs for development.
class InsecureCertProvider : public TlsCertProvider {
public:
  folly::Expected<std::shared_ptr<const fizz::server::FizzServerContext>, std::string>
  createContext(const std::vector<std::string>& alpns) const override;
};

} // namespace openmoq::moqx::tls
