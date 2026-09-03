/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "tls/SniCertManager.h"

#include <algorithm>
#include <chrono>
#include <set>
#include <stdexcept>

#include <folly/String.h>
#include <folly/logging/xlog.h>

#include "tls/CertLoader.h"

namespace openmoq::moqx::tls {

namespace {

namespace fs = std::filesystem;

// Mtime of `path`, or nullopt when it cannot be stat'ed.
std::optional<fs::file_time_type> mtimeOf(const std::string& path) {
  std::error_code ec;
  auto mtime = fs::last_write_time(path, ec);
  return ec ? std::nullopt : std::optional<fs::file_time_type>(mtime);
}

// First scheme in `supported` the cert can produce and the peer advertised
// (DefaultCertManager::findCert semantics).
folly::Optional<fizz::SignatureScheme> selectSchemeStrict(
    const std::vector<fizz::SignatureScheme>& certSchemes,
    const std::vector<fizz::SignatureScheme>& supported,
    const std::vector<fizz::SignatureScheme>& peer
) {
  for (auto scheme : supported) {
    if (std::find(certSchemes.begin(), certSchemes.end(), scheme) != certSchemes.end() &&
        std::find(peer.begin(), peer.end(), scheme) != peer.end()) {
      return scheme;
    }
  }
  return folly::none;
}

// Client-supplied SNI, lowercased. The leading-'*' strip in
// normalizeLookupKey is the insertion-side rule (fizz getKeyFromIdent) and
// must not be applied to a name the peer chose: '*foo.example.com' would then
// hit the entry for 'foo.example.com'.
std::string normalizeSni(std::string s) {
  folly::toLowerAscii(s);
  return s;
}

// CertManager-contract last resort: ignore peerSigSchemes entirely.
folly::Optional<fizz::SignatureScheme> selectSchemeRelaxed(
    const std::vector<fizz::SignatureScheme>& certSchemes,
    const std::vector<fizz::SignatureScheme>& supported
) {
  for (auto scheme : supported) {
    if (std::find(certSchemes.begin(), certSchemes.end(), scheme) != certSchemes.end()) {
      return scheme;
    }
  }
  return folly::none;
}

} // namespace

folly::F14FastMap<std::string, std::shared_ptr<SniCertManager::Entry>>
SniCertManager::buildMap(std::vector<CertDirEntry> scanned, const EntryMap* previous, bool strict) {
  // Carry loaded certs across a rescan for pairs whose files are unchanged.
  EntryMap previousByCertPath;
  if (previous) {
    for (const auto& [ident, entry] : *previous) {
      previousByCertPath.emplace(entry->meta.certPath, entry);
    }
  }

  // Entries whose identities match the directory as it is now, and entries
  // kept from the previous scan because their pair no longer loads. Kept
  // separate so the claim order below can prefer the current ones.
  std::vector<std::shared_ptr<Entry>> current;
  std::vector<std::shared_ptr<Entry>> retained;
  for (auto& meta : scanned) {
    auto prev = previousByCertPath.find(meta.certPath);
    const bool unchanged = prev != previousByCertPath.end() &&
                           prev->second->meta.certMtime == meta.certMtime &&
                           prev->second->meta.keyMtime == meta.keyMtime;

    if (unchanged) {
      current.push_back(prev->second);
      continue;
    }
    auto fresh = std::make_shared<Entry>();
    fresh->meta = std::move(meta);
    try {
      fresh->cert = loadCertPair(fresh->meta.certPath, fresh->meta.keyPath);
      current.push_back(std::move(fresh));
    } catch (const std::exception& e) {
      if (strict) {
        throw;
      }
      if (prev == previousByCertPath.end()) {
        XLOG(ERR) << "cert_dir: dropping '" << fresh->meta.certPath << "': " << e.what();
        continue;
      }
      // Keep the previous version whole rather than pairing the new
      // identities with the old cert. Its stale mtimes make the next rescan
      // retry the load.
      XLOG(ERR) << "cert_dir: '" << fresh->meta.certPath
                << "' failed to load, keeping the previously loaded certificate: " << e.what();
      retained.push_back(prev->second);
    }
  }

  // Retained entries claim last: their identity set is the one the pair had
  // before it stopped loading, so it must only fill names no current pair
  // serves. Two pairs swapping names in one rescan window, with one of them
  // failing to load, is the case that needs it.
  EntryMap map;
  for (const auto* group : {&current, &retained}) {
    for (const auto& entry : *group) {
      for (const auto& ident : entry->meta.identities) {
        auto [it, inserted] = map.emplace(ident, entry);
        if (!inserted) {
          XLOG(ERR) << "cert_dir: '" << entry->meta.certPath << "' does not serve '" << ident
                    << "': already claimed by '" << it->second->meta.certPath << "'";
        }
      }
    }
  }
  return map;
}

folly::F14FastMap<std::string, std::shared_ptr<SniCertManager::Entry>>
SniCertManager::buildPrimaryMap(const EntryMap& byIdentity) {
  // A CN shared by several pairs resolves to the lowest certPath: byIdentity
  // iterates in hash order, which would make the winner arbitrary and able to
  // flip between rescans.
  std::map<std::string, std::shared_ptr<Entry>> byCertPath;
  for (const auto& [ident, entry] : byIdentity) {
    byCertPath.emplace(entry->meta.certPath, entry);
  }
  EntryMap map;
  for (const auto& [path, entry] : byCertPath) {
    if (!entry->meta.primaryIdentity.empty()) {
      map.emplace(entry->meta.primaryIdentity, entry);
    }
  }
  return map;
}

SniCertManager::SniCertManager(Options options) : options_(std::move(options)) {
  const bool hasFileFallback = !options_.fallbackCertFile.empty();
  const bool hasMaterialFallback = options_.fallbackMaterial.has_value();

  State initial;
  std::vector<std::string> warnings; // unused: strict scan throws instead
  initial.byIdentity = buildMap(
      scanCertDir(options_.certDir.dir, /*strict=*/true, warnings),
      nullptr,
      /*strict=*/true
  );
  initial.byPrimary = buildPrimaryMap(initial.byIdentity);

  if (hasFileFallback) {
    // Mtimes are recorded before the content is read; see the parsePair
    // comment in CertDirScanner.cpp for the race this order avoids.
    initial.fallbackCertMtime = mtimeOf(options_.fallbackCertFile);
    initial.fallbackKeyMtime = mtimeOf(options_.fallbackKeyFile);
    initial.fallback = loadCertPair(options_.fallbackCertFile, options_.fallbackKeyFile);
  } else if (hasMaterialFallback) {
    initial.fallback = makeSelfCertFromPems(
        options_.fallbackMaterial->certChainPem,
        options_.fallbackMaterial->keyPem,
        "(in-memory PKCS#12 material)"
    );
  }

  if (initial.byIdentity.empty()) {
    if (!initial.fallback) {
      throw std::runtime_error(
          "cert_dir '" + options_.certDir.dir +
          "' contains no certificate pairs and no fallback cert is configured"
      );
    }
    XLOG(WARN) << "cert_dir '" << options_.certDir.dir
               << "' contains no certificate pairs; serving the fallback cert only";
  }

  *state_.wlock() = std::move(initial);

  if (options_.certDir.reloadInterval > std::chrono::seconds(0)) {
    scheduler_.setThreadName("moqx-cert-rescan");
    scheduler_.addFunction(
        [this] { rescan(); },
        options_.certDir.reloadInterval,
        "cert-dir-rescan",
        options_.certDir.reloadInterval // initial delay: the ctor just scanned
    );
    scheduler_.start();
  }
}

SniCertManager::~SniCertManager() {
  scheduler_.shutdown();
}

fizz::Status SniCertManager::getCert(
    fizz::CertMatch& ret,
    fizz::Error& /* err */,
    const folly::Optional<std::string>& sni,
    const std::vector<fizz::SignatureScheme>& supportedSigSchemes,
    const std::vector<fizz::SignatureScheme>& peerSigSchemes,
    const fizz::ClientHello& /* chlo */
) const {
  std::shared_ptr<Entry> entry;
  std::shared_ptr<fizz::SelfCert> fallback;
  {
    auto state = state_.rlock();
    fallback = state->fallback;
    if (sni) {
      auto key = normalizeSni(*sni);
      auto it = state->byIdentity.find(key);
      if (it == state->byIdentity.end()) {
        // Wildcard form: the suffix from the first dot (".example.com").
        auto dot = key.find_first_of('.');
        if (dot != std::string::npos) {
          it = state->byIdentity.find(key.substr(dot));
        }
      }
      if (it != state->byIdentity.end()) {
        entry = it->second;
      }
    }
  }

  std::shared_ptr<fizz::SelfCert> entryCert = entry ? entry->cert : nullptr;

  // The loaded cert is the authority on which schemes it can sign: offering
  // one its key cannot produce would fail every handshake for the identity.
  std::vector<fizz::SignatureScheme> entrySchemes;
  if (entryCert) {
    entrySchemes = entryCert->getSigSchemes();
  }

  // A usable fallback must beat an entry the client cannot verify: both certs
  // get the strict pass (scheme in the peer's signature_algorithms) before
  // either gets the relaxed CertManager-contract pass.
  if (entryCert) {
    if (auto scheme = selectSchemeStrict(entrySchemes, supportedSigSchemes, peerSigSchemes)) {
      ret = fizz::CertMatchStruct{std::move(entryCert), *scheme, fizz::MatchType::Direct};
      return fizz::Status::Success;
    }
  }
  if (fallback) {
    if (auto scheme =
            selectSchemeStrict(fallback->getSigSchemes(), supportedSigSchemes, peerSigSchemes)) {
      ret = fizz::CertMatchStruct{std::move(fallback), *scheme, fizz::MatchType::Default};
      return fizz::Status::Success;
    }
  }
  if (entryCert) {
    if (auto scheme = selectSchemeRelaxed(entrySchemes, supportedSigSchemes)) {
      ret = fizz::CertMatchStruct{std::move(entryCert), *scheme, fizz::MatchType::Direct};
      return fizz::Status::Success;
    }
  }
  if (fallback) {
    if (auto scheme = selectSchemeRelaxed(fallback->getSigSchemes(), supportedSigSchemes)) {
      ret = fizz::CertMatchStruct{std::move(fallback), *scheme, fizz::MatchType::Default};
      return fizz::Status::Success;
    }
  }

  // DefaultCertManager miss behavior: empty match, Success, no error.
  ret = folly::none;
  return fizz::Status::Success;
}

std::shared_ptr<fizz::SelfCert> SniCertManager::getCert(const std::string& identity) const {
  auto key = normalizeLookupKey(identity);

  std::shared_ptr<Entry> entry;
  std::shared_ptr<fizz::SelfCert> fallback;
  {
    auto state = state_.rlock();
    fallback = state->fallback;
    // The ticket stores the cert's primary identity, so byPrimary is the
    // authority: another cert can carry the same name as a SAN without being
    // the one the ticket names. byIdentity then covers certs whose primary
    // identity is empty.
    if (auto pit = state->byPrimary.find(key); pit != state->byPrimary.end()) {
      entry = pit->second;
    } else if (auto it = state->byIdentity.find(key); it != state->byIdentity.end()) {
      entry = it->second;
    }
  }
  if (entry) {
    return entry->cert;
  }
  if (fallback && normalizeLookupKey(fallback->getIdentity()) == key) {
    return fallback;
  }
  return nullptr;
}

void SniCertManager::rescan() noexcept {
  try {
    std::error_code ec;
    if (!fs::is_directory(options_.certDir.dir, ec)) {
      XLOG(WARN) << "cert_dir '" << options_.certDir.dir
                 << "' is not accessible; keeping previously loaded certificates";
      return;
    }

    // Snapshot the previous state, then scan and build the new maps with no
    // lock held so in-flight handshakes never wait on the rebuild.
    EntryMap previousByIdentity;
    std::optional<fs::file_time_type> prevCertMtime;
    std::optional<fs::file_time_type> prevKeyMtime;
    bool hasFallback = false;
    {
      auto state = state_.rlock();
      previousByIdentity = state->byIdentity;
      prevCertMtime = state->fallbackCertMtime;
      prevKeyMtime = state->fallbackKeyMtime;
      hasFallback = state->fallback != nullptr;
    }

    // Previous metas let the scan skip re-parsing unchanged pairs. Entries
    // retained past a scan error hold their old mtimes while the changed file
    // holds new ones, so they never suppress a needed re-parse.
    std::vector<CertDirEntry> prevMetas;
    {
      std::set<std::string> seenPaths;
      for (const auto& [ident, entry] : previousByIdentity) {
        if (seenPaths.insert(entry->meta.certPath).second) {
          prevMetas.push_back(entry->meta);
        }
      }
    }

    std::vector<std::string> warnings;
    auto scanned = scanCertDir(options_.certDir.dir, /*strict=*/false, warnings, &prevMetas);
    for (const auto& warning : warnings) {
      XLOG(WARN) << "cert_dir rescan: " << warning;
    }
    std::set<std::string> scannedCertPaths;
    for (const auto& meta : scanned) {
      scannedCertPaths.insert(meta.certPath);
    }

    auto newMap = buildMap(std::move(scanned), &previousByIdentity, /*strict=*/false);

    // A scan-dropped pair whose files both still exist is a scan error, not a
    // removal: keep the previous entry, which already carries a loaded cert.
    // Either file missing is a removal — an orphan is a stable state the scan
    // rejects on every pass, so retaining it would serve the cert forever.
    for (const auto& [ident, entry] : previousByIdentity) {
      if (!scannedCertPaths.count(entry->meta.certPath) && fs::exists(entry->meta.certPath, ec) &&
          fs::exists(entry->meta.keyPath, ec) && newMap.emplace(ident, entry).second) {
        XLOG(WARN) << "cert_dir rescan: keeping previously scanned '" << entry->meta.certPath
                   << "' for identity '" << ident << "'";
      }
    }
    auto newPrimary = buildPrimaryMap(newMap);

    // Fallback file pair: refresh eagerly on mtime change so the fallback is
    // always servable; keep the old cert when the new files don't load.
    std::shared_ptr<fizz::SelfCert> newFallback;
    std::optional<fs::file_time_type> newCertMtime;
    std::optional<fs::file_time_type> newKeyMtime;
    if (!options_.fallbackCertFile.empty()) {
      newCertMtime = mtimeOf(options_.fallbackCertFile);
      newKeyMtime = mtimeOf(options_.fallbackKeyFile);
      if (newCertMtime != prevCertMtime || newKeyMtime != prevKeyMtime) {
        try {
          newFallback = loadCertPair(options_.fallbackCertFile, options_.fallbackKeyFile);
          XLOG(INFO) << "reloaded fallback cert '" << options_.fallbackCertFile << "'";
        } catch (const std::exception& e) {
          XLOG(WARN) << "fallback cert reload failed, keeping the previous cert: " << e.what();
        }
      }
    }

    if (newMap.empty() && !previousByIdentity.empty()) {
      if (hasFallback) {
        XLOG(WARN) << "cert_dir '" << options_.certDir.dir
                   << "' no longer contains certificate pairs; serving the fallback cert only";
      } else {
        // Nothing left to serve: every handshake from here on fails until the
        // directory is repaired.
        XLOG(ERR) << "cert_dir '" << options_.certDir.dir
                  << "' no longer contains certificate pairs and no fallback cert is configured; "
                     "handshakes will fail";
      }
    }

    auto state = state_.wlock();
    state->byIdentity = std::move(newMap);
    state->byPrimary = std::move(newPrimary);
    if (newFallback) {
      state->fallback = std::move(newFallback);
      state->fallbackCertMtime = newCertMtime;
      state->fallbackKeyMtime = newKeyMtime;
    }
  } catch (const std::exception& e) {
    XLOG(ERR) << "cert_dir rescan failed; keeping previous state: " << e.what();
  }
}

} // namespace openmoq::moqx::tls
