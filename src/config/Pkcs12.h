/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <string>

#include <folly/Expected.h>
#include <folly/Range.h>

#include "config/Config.h"

namespace openmoq::moqx::config {

// Transcode a PKCS#12 bundle (DER bytes) into in-memory PEM material: the
// certificate chain (leaf + intermediates) and the unencrypted private key.
// Performed entirely in memory; nothing is written to disk.
//
// `password` may be empty for password-less bundles. A NUL-terminated copy is
// made internally (StringPiece is not guaranteed NUL-terminated) and wiped
// before returning. The caller still owns and should wipe its own copy of the
// password, and must never log the returned key material.
//
// Returns a descriptive error string on failure (empty input, invalid
// structure, wrong password / corrupt bundle, or a bundle missing the key or
// certificate).
folly::Expected<TlsMaterial, std::string>
transcodePkcs12(folly::StringPiece der, folly::StringPiece password);

// Read a PKCS#12 file into memory, then transcode it (see transcodePkcs12).
// Returns an error string if the file cannot be read or fails to transcode.
folly::Expected<TlsMaterial, std::string>
loadPkcs12File(const std::string& path, folly::StringPiece password);

} // namespace openmoq::moqx::config
