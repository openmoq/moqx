/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <filesystem>
#include <string>
#include <vector>

namespace openmoq::moqx::tls {

// One <base>.pem/<base>.key pair as discovered by scanCertDir. Key material is
// NOT read at scan time.
struct CertDirEntry {
  std::string certPath;
  std::string keyPath;
  // Lowercased CN/DNS-SAN identities, wildcard "*.x" normalized to ".x"
  // (fizz DefaultCertManager key form: exact lookup, then first-dot suffix).
  std::vector<std::string> identities;
  // normalizeLookupKey of fizz SelfCert::getIdentity(): the CN, or the
  // subject DN when the cert has no CN.
  std::string primaryIdentity;
  std::filesystem::file_time_type certMtime;
  std::filesystem::file_time_type keyMtime;
};

// Normalize an identity or SNI value to the map-key form the entries above
// use: lowercase, leading "*" stripped ("*.x" -> ".x"). Writers and readers of
// CertDirEntry::identities must both go through this so lookups can't drift.
std::string normalizeLookupKey(std::string s);

// Scan a directory (non-recursive) for <base>.pem + <base>.key pairs and
// extract identities from each certificate (DNS SANs; CN when there are none).
//
// `previous` (optional) holds an earlier scan's entries, matched by certPath:
// a pair whose cert and key mtimes both equal its previous entry's is copied
// forward without reading or parsing the files. Orphan and duplicate-identity
// detection apply to copied entries too; previous entries whose files are
// gone from the directory are ignored.
//
// Failure handling:
//  - An unopenable directory throws std::runtime_error regardless of `strict`.
//  - A per-pair problem is an orphan .pem/.key, an unstattable file, an
//    unparsable cert, an invalid identity, or an identity claimed by two pairs
//    (the incumbent, else the first in sorted order, wins).
//     - strict=true: it throws.
//     - strict=false: it is appended to `warnings` and the pair is skipped.
std::vector<CertDirEntry> scanCertDir(
    const std::string& dir,
    bool strict,
    std::vector<std::string>& warnings,
    const std::vector<CertDirEntry>* previous = nullptr
);

} // namespace openmoq::moqx::tls
