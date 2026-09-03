/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <filesystem>
#include <memory>
#include <optional>
#include <string>

#include <fizz/server/CertManager.h>
#include <folly/Synchronized.h>
#include <folly/container/F14Map.h>
#include <folly/executors/FunctionScheduler.h>

#include "config/Config.h"
#include "tls/CertDirScanner.h"

namespace openmoq::moqx::tls {

// fizz CertManager serving certs by SNI from a cert_dir (see CertDirScanner
// for the directory contract).
//  - Matching follows fizz DefaultCertManager semantics: lowercase, exact
//    then first-dot wildcard, fallback as MatchType::Default.
//  - Cert and key material is loaded up front, on whichever thread runs the
//    scan: the constructor at startup, the rescan thread afterwards. getCert
//    is then a map lookup, so no handshake ever waits on disk.
//  - Loading is not deferred to first use. fizz::server::CertManager::getCert
//    is synchronous (fizz::Status is Fail/Success, the cert comes back through
//    a CertMatch& out-param), so a deferred load would have to read from the
//    IO thread mid-handshake, where a slow or hung cert_dir mount stalls every
//    other connection on that thread.
//  - The directory can be rescanned at runtime without dropping in-flight
//    handshakes.
//
// The session-ticket cipher must share this instance:
//  - Resumption resolves certs through getCert(identity), keyed by the cert's
//    primary identity as fizz stores it in the ticket.
//  - A removed identity resolves to nullptr; fizz records that as an absent
//    server cert on the resumed session.
class SniCertManager : public fizz::server::CertManager {
public:
  struct Options {
    config::CertDirConfig certDir;
    // Fallback cert for absent/unmatched SNI. At most one source; with
    // neither, unmatched SNI fails the handshake.
    // File pair: refreshed on rescan when the files change.
    std::string fallbackCertFile;
    std::string fallbackKeyFile;
    // In-memory PEM material: never refreshed.
    std::optional<config::TlsMaterial> fallbackMaterial;
  };

  // Strict initial scan and load; throws std::runtime_error on any problem.
  // Starts the background rescan when reloadInterval > 0.
  explicit SniCertManager(Options options);
  ~SniCertManager() override;

  fizz::Status getCert(
      fizz::CertMatch& ret,
      fizz::Error& err,
      const folly::Optional<std::string>& sni,
      const std::vector<fizz::SignatureScheme>& supportedSigSchemes,
      const std::vector<fizz::SignatureScheme>& peerSigSchemes,
      const fizz::ClientHello& chlo
  ) const override;

  std::shared_ptr<fizz::SelfCert> getCert(const std::string& identity) const override;

  // Non-strict rescan of the dir + fallback refresh. Never throws: problems
  // are logged and the previously loaded certs keep serving.
  void rescan() noexcept;

private:
  struct Entry {
    CertDirEntry meta;
    // Loaded on the scanning thread before the entry is published, and never
    // written again; handshakes only read it. Never null: buildMap drops a
    // pair it cannot load.
    std::shared_ptr<fizz::SelfCert> cert;
  };
  using EntryMap = folly::F14FastMap<std::string, std::shared_ptr<Entry>>;
  struct State {
    // Normalized identity -> entry; entries with several identities appear
    // under each. shared_ptr so a rescan can swap the map while in-flight
    // handshakes keep their refs.
    EntryMap byIdentity;
    // Normalized primary identity (fizz getIdentity(): CN, or subject DN
    // without one) -> entry, for ticket-resumption lookups; the lowest
    // certPath wins when several certs share one.
    EntryMap byPrimary;
    std::shared_ptr<fizz::SelfCert> fallback;
    // Empty when the file could not be stat'ed: an unreadable mtime is not a
    // change, and recording a sentinel would force a reload every rescan.
    std::optional<std::filesystem::file_time_type> fallbackCertMtime;
    std::optional<std::filesystem::file_time_type> fallbackKeyMtime;
  };

  // Load every scanned pair, reusing `previous`'s entry for a pair whose
  // files are unchanged. strict=true throws on the first load failure;
  // strict=false logs it and keeps the pair's previous version, dropping the
  // pair when there is none. A kept previous version claims its identities
  // only where no currently loading pair claims them.
  static EntryMap
  buildMap(std::vector<CertDirEntry> scanned, const EntryMap* previous, bool strict);
  static EntryMap buildPrimaryMap(const EntryMap& byIdentity);

  Options options_;
  folly::Synchronized<State> state_;
  // Declared last so its thread stops before the members rescan() touches are
  // destroyed.
  folly::FunctionScheduler scheduler_;
};

} // namespace openmoq::moqx::tls
