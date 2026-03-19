#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include <fizz/server/CertManager.h>
#include <fizz/server/FizzServerContext.h>
#include <folly/Expected.h>

namespace openmoq::o_rly::tls {

/// A single TLS ticket encryption seed (typically 32 bytes).
using TicketSeed = std::vector<uint8_t>;

/// Build a standard FizzServerContext from a CertManager and ALPN list.
/// Configures ticket cipher, client auth, early data, etc.
///
/// @param ticketSeeds  Optional ticket encryption seeds. The first seed
///   encrypts new tickets; all seeds can decrypt (supports key rotation).
///   When empty, a random seed is generated (tickets won't survive restart).
folly::Expected<std::shared_ptr<const fizz::server::FizzServerContext>, std::string>
buildStandardFizzContext(
    std::shared_ptr<fizz::server::CertManager> certManager,
    const std::vector<std::string>& alpns,
    const std::vector<TicketSeed>& ticketSeeds = {}
);

/// Build MOQT ALPN list from a comma-separated versions string.
/// Does not include "h3" — callers add it when WebTransport is needed.
std::vector<std::string> buildMoqtAlpns(const std::string& moqtVersions);

} // namespace openmoq::o_rly::tls
