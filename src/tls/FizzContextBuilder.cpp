/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "tls/FizzContextBuilder.h"

#include <fizz/backend/openssl/certificate/CertUtils.h>
#include <fizz/server/AeadTicketCipher.h>
#include <fizz/server/DefaultCertManager.h>
#include <fizz/server/ReplayCache.h>
#include <fizz/server/TicketCodec.h>
#include <fizz/util/Status.h>
#include <folly/Random.h>
#include <proxygen/httpserver/samples/hq/FizzContext.h>

#include <array>
#include <chrono>
#include <stdexcept>
#include <type_traits>
#include <variant>

namespace openmoq::moqx::tls {

namespace {

// Builds the context from in-memory PEM so a PKCS#12-derived key never touches
// disk. Mirrors quic::samples::createFizzServerContextImpl, which the PEM-file
// path uses, so both paths get the same TLS settings.
// TODO(#482): replace with a buffer-based helper in the moxygen fork.
std::shared_ptr<const fizz::server::FizzServerContext> buildFizzContextFromMaterial(
    const std::vector<std::string>& alpns,
    fizz::server::ClientAuthMode clientAuth,
    const std::string& certChainPem,
    const std::string& keyPem
) {
  std::unique_ptr<fizz::SelfCert> cert;
  fizz::Error err;
  FIZZ_THROW_ON_ERROR(fizz::openssl::CertUtils::makeSelfCert(cert, err, certChainPem, keyPem), err);
  auto certManager = std::make_shared<fizz::server::DefaultCertManager>();
  certManager->addCertAndSetDefault(std::move(cert));

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
  ctx->setClientAuthMode(clientAuth);
  ctx->setSupportedAlpns(alpns);
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

} // namespace

std::shared_ptr<const fizz::server::FizzServerContext>
buildFizzServerContext(const config::TlsMode& tlsMode, const std::vector<std::string>& alpns) {
  return std::visit(
      [&alpns](const auto& tlsCfg) -> std::shared_ptr<const fizz::server::FizzServerContext> {
        using T = std::decay_t<decltype(tlsCfg)>;
        if constexpr (std::is_same_v<T, config::Insecure>) {
          return quic::samples::createFizzServerContextWithInsecureDefault(
              alpns,
              fizz::server::ClientAuthMode::None,
              "",
              ""
          );
        } else {
          // PKCS#12 source: certFile/keyFile are empty, the PEM is in material.
          if (tlsCfg.material.has_value()) {
            return buildFizzContextFromMaterial(
                alpns,
                fizz::server::ClientAuthMode::Optional,
                tlsCfg.material->certChainPem,
                tlsCfg.material->keyPem
            );
          }
          // fizz's own error ("no certificates read") names no file.
          try {
            return quic::samples::createFizzServerContext(
                alpns,
                fizz::server::ClientAuthMode::Optional,
                tlsCfg.certFile,
                tlsCfg.keyFile
            );
          } catch (const std::exception& e) {
            throw std::runtime_error(
                "failed to load TLS certificate/key (cert='" + tlsCfg.certFile + "', key='" +
                tlsCfg.keyFile + "'): " + e.what() +
                " - check the paths exist and are readable by this process"
            );
          }
        }
      },
      tlsMode
  );
}

} // namespace openmoq::moqx::tls
