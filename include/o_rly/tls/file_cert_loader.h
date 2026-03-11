#pragma once

#include <string>

#include "o_rly/tls/tls_cert_loader.h"

namespace openmoq::o_rly::tls {

class FileCertLoader : public TlsCertLoader {
public:
  FileCertLoader(std::string certFile, std::string keyFile);
  folly::Expected<LoadedCerts, std::string> load() const override;

private:
  std::string certFile_;
  std::string keyFile_;
};

} // namespace openmoq::o_rly::tls
