/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "tls/FizzContextBuilder.h"

#include <array>
#include <variant>

#include <fizz/server/AeadTicketCipher.h>
#include <fizz/server/DefaultCertManager.h>
#include <fizz/server/ReplayCache.h>
#include <fizz/server/TicketCodec.h>
#include <folly/Random.h>
#include <proxygen/httpserver/samples/hq/FizzContext.h>

#include "tls/CertLoader.h"

namespace openmoq::moqx::tls {

std::shared_ptr<fizz::server::CertManager> makeCertManager(const config::TlsConfig& cfg) {
  auto certManager = std::make_shared<fizz::server::DefaultCertManager>();
  if (cfg.material.has_value()) {
    // PKCS#12 source: in-memory PEM buffers, the key never touches disk.
    certManager->addCertAndSetDefault(makeSelfCertFromPems(
        cfg.material->certChainPem,
        cfg.material->keyPem,
        "(in-memory PKCS#12 material)"
    ));
    return certManager;
  }
  certManager->addCertAndSetDefault(loadCertPair(cfg.certFile, cfg.keyFile));
  return certManager;
}

// Mirrors quic::samples::createFizzServerContextImpl to keep the proxygen
// sample's TLS behavior.
// TODO(#482): replace with a helper in the moxygen fork.
std::shared_ptr<const fizz::server::FizzServerContext> buildFizzServerContext(
    std::shared_ptr<fizz::server::CertManager> certManager,
    FizzContextOptions options
) {
  auto ctx = std::make_shared<fizz::server::FizzServerContext>();
  ctx->setCertManager(certManager);
  auto ticketCipher = std::make_shared<fizz::server::Aead128GCMTicketCipher<
      fizz::server::TicketCodec<fizz::server::CertificateStorage::X509>>>(
      ctx->getFactoryPtr(),
      std::move(certManager)
  );
  std::array<uint8_t, 32> ticketSeed;
  folly::Random::secureRandom(ticketSeed.data(), ticketSeed.size());
  ticketCipher->setTicketSecrets({{folly::range(ticketSeed)}});
  ctx->setTicketCipher(ticketCipher);
  ctx->setClientAuthMode(fizz::server::ClientAuthMode::Optional);
  ctx->setSupportedAlpns(options.alpns);
  ctx->setAlpnMode(fizz::server::AlpnMode::Required);
  ctx->setSendNewSessionTicket(true);
  ctx->setEarlyDataFbOnly(false);
  ctx->setVersionFallbackEnabled(false);

  fizz::server::ClockSkewTolerance tolerance;
  tolerance.before = std::chrono::minutes(-5);
  tolerance.after = std::chrono::minutes(5);
  std::shared_ptr<fizz::server::ReplayCache> replayCache =
      std::make_shared<fizz::server::AllowAllReplayReplayCache>();
  ctx->setEarlyDataSettings(true, tolerance, std::move(replayCache));
  return ctx;
}

std::shared_ptr<const fizz::server::FizzServerContext>
buildFizzServerContext(const config::TlsMode& mode, FizzContextOptions options) {
  return std::visit(
      [&options](const auto& tls) -> std::shared_ptr<const fizz::server::FizzServerContext> {
        using T = std::decay_t<decltype(tls)>;
        if constexpr (std::is_same_v<T, config::Insecure>) {
          return quic::samples::createFizzServerContextWithInsecureDefault(
              options.alpns,
              fizz::server::ClientAuthMode::None,
              "",
              ""
          );
        } else {
          return buildFizzServerContext(makeCertManager(tls), std::move(options));
        }
      },
      mode
  );
}

} // namespace openmoq::moqx::tls
