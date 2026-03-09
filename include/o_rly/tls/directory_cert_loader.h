#pragma once

#include <string>

#include "o_rly/tls/tls_cert_loader.h"

namespace openmoq::o_rly::tls {

class DirectoryCertLoader : public TlsCertLoader {
public:
  DirectoryCertLoader(std::string certDir, std::string defaultCertIdentity);
  folly::Expected<LoadedCerts, std::string> load() const override;

private:
  std::string certDir_;
  std::string defaultCertIdentity_;
};

} // namespace openmoq::o_rly::tls
