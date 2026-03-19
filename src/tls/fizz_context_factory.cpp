#include "o_rly/tls/fizz_context_factory.h"

#include <array>

#include <fizz/server/AeadTicketCipher.h>
#include <fizz/server/TicketCodec.h>
#include <folly/Random.h>
#include <moxygen/MoQVersions.h>

namespace openmoq::o_rly::tls {

std::vector<std::string> buildMoqtAlpns(const std::string& moqtVersions) {
  std::vector<std::string> alpns;
  auto moqt = moxygen::getMoqtProtocols(moqtVersions, true);
  alpns.insert(alpns.end(), moqt.begin(), moqt.end());
  return alpns;
}

folly::Expected<std::shared_ptr<const fizz::server::FizzServerContext>, std::string>
buildStandardFizzContext(
    std::shared_ptr<fizz::server::CertManager> certManager,
    const std::vector<std::string>& alpns,
    const std::vector<TicketSeed>& ticketSeeds
) {
  auto serverCtx = std::make_shared<fizz::server::FizzServerContext>();
  serverCtx->setCertManager(certManager);

  auto ticketCipher = std::make_shared<fizz::server::Aead128GCMTicketCipher<
      fizz::server::TicketCodec<fizz::server::CertificateStorage::X509>>>(
      serverCtx->getFactoryPtr(),
      std::move(certManager)
  );
  // First seed encrypts new tickets; all seeds can decrypt (key rotation).
  if (!ticketSeeds.empty()) {
    std::vector<folly::ByteRange> secrets;
    secrets.reserve(ticketSeeds.size());
    for (const auto& seed : ticketSeeds) {
      secrets.emplace_back(folly::range(seed));
    }
    ticketCipher->setTicketSecrets(std::move(secrets));
  } else {
    std::array<uint8_t, 32> randomSeed;
    folly::Random::secureRandom(randomSeed.data(), randomSeed.size());
    ticketCipher->setTicketSecrets({{folly::range(randomSeed)}});
  }
  serverCtx->setTicketCipher(ticketCipher);

  serverCtx->setClientAuthMode(fizz::server::ClientAuthMode::None);
  serverCtx->setSupportedAlpns(alpns);
  serverCtx->setAlpnMode(fizz::server::AlpnMode::Required);
  serverCtx->setSendNewSessionTicket(false);
  serverCtx->setEarlyDataFbOnly(false);
  serverCtx->setVersionFallbackEnabled(false);

  fizz::server::ClockSkewTolerance tolerance;
  tolerance.before = std::chrono::minutes(-5);
  tolerance.after = std::chrono::minutes(5);

  std::shared_ptr<fizz::server::ReplayCache> replayCache =
      std::make_shared<fizz::server::AllowAllReplayReplayCache>();

  serverCtx->setEarlyDataSettings(true, tolerance, std::move(replayCache));

  return serverCtx;
}

} // namespace openmoq::o_rly::tls
