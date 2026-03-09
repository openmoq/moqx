#pragma once

#include <string>

#include "moqx/tls/tls_cert_loader.h"

namespace openmoq::moqx::tls {

/// Loads certificates from a specified directory. This directory is searched for pairs of files
/// with `.crt` and `.key` extensions; the `.crt` file is expected to be the certificate, and the
/// `.key` file the corresponding key file.
class DirectoryCertLoader : public TlsCertLoader {
public:
  DirectoryCertLoader(std::string certDir, std::string defaultCertIdentity);
  folly::Expected<LoadedCerts, std::string> load() const override;

private:
  std::string certDir_;
  std::string defaultCertIdentity_;
};

} // namespace openmoq::moqx::tls
