/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "tls/CertDirScanner.h"

#include <algorithm>
#include <map>
#include <set>
#include <stdexcept>

#include <fizz/backend/openssl/certificate/CertUtils.h>
#include <fizz/util/Status.h>
#include <folly/FileUtil.h>
#include <folly/String.h>
#include <folly/ssl/OpenSSLCertUtils.h>

namespace openmoq::moqx::tls {

namespace {

namespace fs = std::filesystem;

// strict: the problem aborts the whole scan; otherwise it is recorded and the
// offending pair is skipped.
void problem(bool strict, std::vector<std::string>& warnings, std::string msg) {
  if (strict) {
    throw std::runtime_error(std::move(msg));
  }
  warnings.push_back(std::move(msg));
}

// normalizeLookupKey plus validation. Throws on identities fizz would reject
// (empty, bare "*", any remaining "*" e.g. nested wildcards). Mirrors fizz
// DefaultCertManager::getKeyFromIdent + addCertIdentity's validity check.
std::string normalizeIdentity(const std::string& ident) {
  if (ident.empty()) {
    throw std::runtime_error("empty identity");
  }
  std::string key = normalizeLookupKey(ident);
  if (key.empty() || key == "." || key.find('*') != std::string::npos) {
    throw std::runtime_error("invalid identity '" + ident + "'");
  }
  return key;
}

// Parse the pair's certificate (leaf only, no key material) into a
// CertDirEntry stamped with the given mtimes. Throws std::runtime_error with
// a path-annotated message.
CertDirEntry parsePair(
    const std::string& certPath,
    const std::string& keyPath,
    fs::file_time_type certMtime,
    fs::file_time_type keyMtime
) {
  std::string certPem;
  if (!folly::readFile(certPath.c_str(), certPem)) {
    throw std::runtime_error("failed to read '" + certPath + "'");
  }

  std::vector<folly::ssl::X509UniquePtr> certs;
  try {
    certs = folly::ssl::OpenSSLCertUtils::readCertsFromBuffer(folly::StringPiece(certPem));
  } catch (const std::exception& e) {
    throw std::runtime_error("failed to parse certificate '" + certPath + "': " + e.what());
  }
  if (certs.empty()) {
    throw std::runtime_error("no certificate found in '" + certPath + "'");
  }
  X509& leaf = *certs.front();

  auto cn = folly::ssl::OpenSSLCertUtils::getCommonName(leaf);
  std::vector<std::string> rawIdents = folly::ssl::OpenSSLCertUtils::getSubjectAltNames(leaf);
  if (rawIdents.empty() && cn && !cn->empty()) {
    rawIdents.push_back(*cn);
  }
  if (rawIdents.empty()) {
    throw std::runtime_error("certificate '" + certPath + "' has no DNS SANs and no CN");
  }

  CertDirEntry entry;
  entry.certPath = certPath;
  entry.keyPath = keyPath;
  if (cn && !cn->empty()) {
    entry.primaryIdentity = normalizeLookupKey(*cn);
  } else if (auto subject = folly::ssl::OpenSSLCertUtils::getSubject(leaf);
             subject && !subject->empty()) {
    // fizz SelfCert::getIdentity() falls back to the subject DN when there is
    // no CN; mirror it so resumption lookups for a CN-less cert resolve.
    entry.primaryIdentity = normalizeLookupKey(*subject);
  }
  for (const auto& ident : rawIdents) {
    std::string key;
    try {
      key = normalizeIdentity(ident);
    } catch (const std::exception& e) {
      throw std::runtime_error(std::string("certificate '") + certPath + "': " + e.what());
    }
    if (std::find(entry.identities.begin(), entry.identities.end(), key) ==
        entry.identities.end()) {
      entry.identities.push_back(std::move(key));
    }
  }

  folly::ssl::EvpPkeyUniquePtr pubKey(X509_get_pubkey(&leaf));
  if (!pubKey) {
    throw std::runtime_error("failed to read public key from '" + certPath + "'");
  }
  fizz::openssl::KeyType keyType;
  fizz::Error err;
  if (fizz::openssl::CertUtils::getKeyType(keyType, err, pubKey) != fizz::Status::Success) {
    throw std::runtime_error(
        "unsupported key type in '" + certPath + "': " + (err.msg() ? err.msg() : "unknown")
    );
  }

  entry.certMtime = certMtime;
  entry.keyMtime = keyMtime;
  return entry;
}

} // namespace

std::string normalizeLookupKey(std::string s) {
  if (!s.empty() && s.front() == '*') {
    s.erase(0, 1);
  }
  folly::toLowerAscii(s);
  return s;
}

std::vector<CertDirEntry> scanCertDir(
    const std::string& dir,
    bool strict,
    std::vector<std::string>& warnings,
    const std::vector<CertDirEntry>* previous
) {
  std::error_code ec;
  fs::directory_iterator it(dir, ec);
  if (ec) {
    // Always fatal, even non-strict: an unopenable dir is indistinguishable
    // from an empty one, and returning {} would wipe every served identity.
    throw std::runtime_error("cert_dir '" + dir + "' is not readable: " + ec.message());
  }

  // Bases are collected first so orphan detection is order-independent.
  std::set<std::string> pemBases;
  std::set<std::string> keyBases;
  for (const auto& de : it) {
    if (!de.is_regular_file(ec)) {
      continue;
    }
    const auto& path = de.path();
    if (path.extension() == ".pem") {
      pemBases.insert(path.stem().string());
    } else if (path.extension() == ".key") {
      keyBases.insert(path.stem().string());
    }
  }

  for (const auto& base : keyBases) {
    if (!pemBases.count(base)) {
      problem(
          strict,
          warnings,
          "cert_dir '" + dir + "': orphan key '" + base + ".key' has no matching " + base + ".pem"
      );
    }
  }

  std::map<std::string, const CertDirEntry*> previousByCertPath;
  if (previous) {
    for (const auto& entry : *previous) {
      previousByCertPath.emplace(entry.certPath, &entry);
    }
  }

  // Incumbents (pairs present in `previous`) claim identities before new
  // arrivals: a duplicate dropped into a live directory must not steal an
  // identity from the pair serving it. Sorted order breaks ties within each.
  std::vector<std::string> orderedBases;
  orderedBases.reserve(pemBases.size());
  for (bool incumbentPass : {true, false}) {
    for (const auto& base : pemBases) {
      auto certPath = (fs::path(dir) / (base + ".pem")).string();
      if ((previousByCertPath.count(certPath) != 0) == incumbentPass) {
        orderedBases.push_back(base);
      }
    }
  }

  std::vector<CertDirEntry> entries;
  std::map<std::string, std::string> identityToCert; // duplicate detection
  for (const auto& base : orderedBases) {
    auto certPath = (fs::path(dir) / (base + ".pem")).string();
    if (!keyBases.count(base)) {
      problem(
          strict,
          warnings,
          "cert_dir '" + dir + "': orphan cert '" + base + ".pem' has no matching " + base + ".key"
      );
      continue;
    }
    auto keyPath = (fs::path(dir) / (base + ".key")).string();

    // Mtimes are recorded before the content is read: a write racing the scan
    // then leaves an older mtime with the newer bytes, so the next rescan
    // re-parses, instead of stamping stale bytes with the new mtime forever.
    std::error_code certEc;
    std::error_code keyEc;
    auto certMtime = fs::last_write_time(certPath, certEc);
    auto keyMtime = fs::last_write_time(keyPath, keyEc);
    if (certEc || keyEc) {
      problem(
          strict,
          warnings,
          "failed to stat '" + (certEc ? certPath : keyPath) +
              "': " + (certEc ? certEc : keyEc).message()
      );
      continue;
    }

    CertDirEntry entry;
    auto prev = previousByCertPath.find(certPath);
    if (prev != previousByCertPath.end() && prev->second->certMtime == certMtime &&
        prev->second->keyMtime == keyMtime) {
      entry = *prev->second;
    } else {
      try {
        entry = parsePair(certPath, keyPath, certMtime, keyMtime);
      } catch (const std::exception& e) {
        problem(strict, warnings, e.what());
        continue;
      }
    }

    // Check every identity before claiming any, so a dropped pair leaves no
    // trace in the duplicate map.
    bool duplicate = false;
    for (const auto& ident : entry.identities) {
      auto existing = identityToCert.find(ident);
      if (existing != identityToCert.end()) {
        problem(
            strict,
            warnings,
            "duplicate identity '" + ident + "' claimed by both '" + existing->second + "' and '" +
                certPath + "'"
        );
        duplicate = true;
      }
    }
    if (duplicate) {
      continue; // first claimant wins; drop the whole later pair
    }
    for (const auto& ident : entry.identities) {
      identityToCert.emplace(ident, certPath);
    }
    entries.push_back(std::move(entry));
  }

  return entries;
}

} // namespace openmoq::moqx::tls
