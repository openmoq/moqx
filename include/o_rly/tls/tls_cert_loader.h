#pragma once

#include <memory>
#include <string>
#include <vector>

#include <fizz/protocol/Certificate.h>
#include <fizz/server/FizzServerContext.h>
#include <folly/Expected.h>

#include "o_rly/tls/fizz_context_factory.h"

namespace openmoq::o_rly::tls {

struct LoadedCerts {
  struct Entry {
    std::string identity;
    std::shared_ptr<fizz::SelfCert> cert;
  };
  std::vector<Entry> certs;
  std::string defaultIdentity;
};

/// Base interface for TLS credential plugins.
///
/// Providers implement createContext() to produce a fully configured
/// FizzServerContext. On-demand providers (remote HSM/KMS) can supply a custom
/// CertManager that handles SNI lookup and cert retrieval at handshake time.
class TlsCertProvider {
public:
  virtual ~TlsCertProvider() = default;
  virtual folly::Expected<std::shared_ptr<const fizz::server::FizzServerContext>, std::string>
  createContext(
      const std::vector<std::string>& alpns,
      const std::vector<TicketSeed>& ticketSeeds = {}
  ) const = 0;
};

/// Convenience base for providers that load all certs upfront.
/// Subclasses implement load(); createContext() builds a
/// DefaultCertManager from the loaded certs and wraps it in a
/// standard FizzServerContext automatically.
class TlsCertLoader : public TlsCertProvider {
public:
  virtual folly::Expected<LoadedCerts, std::string> load() const = 0;
  folly::Expected<std::shared_ptr<const fizz::server::FizzServerContext>, std::string>
  createContext(
      const std::vector<std::string>& alpns,
      const std::vector<TicketSeed>& ticketSeeds = {}
  ) const override;
};

} // namespace openmoq::o_rly::tls
