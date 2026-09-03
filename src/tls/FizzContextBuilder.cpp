/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "tls/FizzContextBuilder.h"

#include <array>
#include <map>
#include <stdexcept>
#include <variant>

#include <folly/String.h>
#include <folly/Synchronized.h>
#include <folly/ssl/OpenSSLHash.h>

#include <fizz/server/AeadTicketCipher.h>
#include <fizz/server/DefaultCertManager.h>
#include <fizz/server/ReplayCache.h>
#include <fizz/server/TicketCodec.h>
#include <folly/Random.h>
#include <proxygen/httpserver/samples/hq/FizzContext.h>

#include "tls/CertLoader.h"
#include "tls/SniCertManager.h"

namespace openmoq::moqx::tls {

namespace {

// One SniCertManager per distinct option set: listeners sharing a cert_dir
// share the scan, the rescan thread, and the loaded keys. Safe to share
// (Synchronized state); dead cache slots are pruned on the next call.
std::shared_ptr<SniCertManager> sharedSniCertManager(SniCertManager::Options options) {
  static folly::Synchronized<std::map<std::string, std::weak_ptr<SniCertManager>>> cache;

  std::string key = options.certDir.dir;
  key += '\0' + std::to_string(options.certDir.reloadInterval.count());
  key += '\0' + options.fallbackCertFile;
  key += '\0' + options.fallbackKeyFile;
  if (options.fallbackMaterial.has_value()) {
    // Digest, not the PEM itself: this key lives in a process-lifetime static,
    // which is no place for a plaintext private key.
    const auto& material = *options.fallbackMaterial;
    std::array<unsigned char, 32> digest{};
    folly::ssl::OpenSSLHash::Digest hasher;
    hasher.hash_init(EVP_sha256());
    hasher.hash_update(folly::ByteRange(folly::StringPiece(material.certChainPem)));
    hasher.hash_update(folly::ByteRange(folly::StringPiece(material.keyPem)));
    hasher.hash_final(folly::MutableByteRange(digest.data(), digest.size()));
    key += '\0' + folly::hexlify(folly::ByteRange(digest.data(), digest.size()));
  }

  auto locked = cache.wlock();
  for (auto it = locked->begin(); it != locked->end();) {
    it = it->second.expired() ? locked->erase(it) : std::next(it);
  }
  if (auto it = locked->find(key); it != locked->end()) {
    if (auto existing = it->second.lock()) {
      return existing;
    }
  }
  auto manager = std::make_shared<SniCertManager>(std::move(options));
  (*locked)[key] = manager;
  return manager;
}

} // namespace

std::shared_ptr<fizz::server::CertManager> makeCertManager(const config::ListenerTlsConfig& cfg) {
  if (cfg.certDir.has_value()) {
    SniCertManager::Options options;
    options.certDir = *cfg.certDir;
    options.fallbackCertFile = cfg.tls.certFile;
    options.fallbackKeyFile = cfg.tls.keyFile;
    options.fallbackMaterial = cfg.tls.material;
    return sharedSniCertManager(std::move(options));
  }

  auto certManager = std::make_shared<fizz::server::DefaultCertManager>();
  if (cfg.tls.material.has_value()) {
    // PKCS#12 source: in-memory PEM buffers, the key never touches disk.
    certManager->addCertAndSetDefault(makeSelfCertFromPems(
        cfg.tls.material->certChainPem,
        cfg.tls.material->keyPem,
        "(in-memory PKCS#12 material)"
    ));
    return certManager;
  }
  certManager->addCertAndSetDefault(loadCertPair(cfg.tls.certFile, cfg.tls.keyFile));
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
  if (!options.ticketSeeds.empty()) {
    std::vector<folly::ByteRange> secrets;
    secrets.reserve(options.ticketSeeds.size());
    for (const auto& seed : options.ticketSeeds) {
      secrets.emplace_back(reinterpret_cast<const uint8_t*>(seed.data()), seed.size());
    }
    // A false return would leave the cipher secretless: tickets silently stop
    // being issued and resumption dies. Config validation enforces the 32-byte
    // minimum; this guards programmatic callers of the extension seam.
    if (!ticketCipher->setTicketSecrets(secrets)) {
      throw std::runtime_error(
          "fizz rejected the TLS ticket seeds: each seed must be at least 32 bytes"
      );
    }
  } else {
    std::array<uint8_t, 32> ticketSeed;
    folly::Random::secureRandom(ticketSeed.data(), ticketSeed.size());
    ticketCipher->setTicketSecrets({{folly::range(ticketSeed)}});
  }
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
          options.ticketSeeds = tls.ticketSeeds;
          return buildFizzServerContext(makeCertManager(tls), std::move(options));
        }
      },
      mode
  );
}

} // namespace openmoq::moqx::tls
