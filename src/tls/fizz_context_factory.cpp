#include "moqx/tls/fizz_context_factory.h"

#include <array>

#include <fizz/server/AeadTicketCipher.h>
#include <fizz/server/TicketCodec.h>
#include <folly/Random.h>
#include <moxygen/MoQVersions.h>

namespace openmoq::moqx::tls {

std::vector<std::string> buildAlpns(const std::string& moqtVersions) {
  std::vector<std::string> alpns = {"h3"};
  auto moqt = moxygen::getMoqtProtocols(moqtVersions, true);
  alpns.insert(alpns.end(), moqt.begin(), moqt.end());
  return alpns;
}

folly::Expected<std::shared_ptr<const fizz::server::FizzServerContext>, std::string>
buildStandardFizzContext(
    std::shared_ptr<fizz::server::CertManager> certManager,
    const std::vector<std::string>& alpns
) {
  auto serverCtx = std::make_shared<fizz::server::FizzServerContext>();
  serverCtx->setCertManager(certManager);

  auto ticketCipher = std::make_shared<fizz::server::Aead128GCMTicketCipher<
      fizz::server::TicketCodec<fizz::server::CertificateStorage::X509>>>(
      serverCtx->getFactoryPtr(),
      std::move(certManager)
  );
  std::array<uint8_t, 32> ticketSeed;
  folly::Random::secureRandom(ticketSeed.data(), ticketSeed.size());
  ticketCipher->setTicketSecrets({{folly::range(ticketSeed)}});
  serverCtx->setTicketCipher(ticketCipher);

  serverCtx->setClientAuthMode(fizz::server::ClientAuthMode::Optional);
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

} // namespace openmoq::moqx::tls
