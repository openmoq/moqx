/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <memory>
#include <string>
#include <vector>

#include <fizz/server/CertManager.h>
#include <fizz/server/FizzServerContext.h>

#include "config/Config.h"

namespace openmoq::moqx::tls {

struct FizzContextOptions {
  std::vector<std::string> alpns;
};

// Config-to-CertManager dispatch for a secure listener: a single-cert
// DefaultCertManager from the configured file pair or in-memory material.
//
// This dispatch is the seam for alternative cert sources (HSM, KMS, ...):
// implement fizz::server::CertManager and pass it to the CertManager overload
// of buildFizzServerContext below. getCert() is synchronous, so selection must
// run from in-process state; slow remote signing belongs in the served certs
// (fizz::AsyncSelfCert), not the manager.
//
// Throws std::runtime_error with the offending paths on any load failure.
std::shared_ptr<fizz::server::CertManager> makeCertManager(const config::TlsConfig& cfg);

// Secure fizz server context around a caller-supplied CertManager: ticket
// cipher wired to the same manager (resumption resolves certs through it),
// ALPN required, early data on, ClientAuthMode::Optional.
std::shared_ptr<const fizz::server::FizzServerContext> buildFizzServerContext(
    std::shared_ptr<fizz::server::CertManager> certManager,
    FizzContextOptions options
);

// Fizz server context for a listener. Insecure: the proxygen sample context
// with a compiled-in cert (ClientAuthMode::None). Secure: the CertManager
// overload around makeCertManager(cfg).
// Throws std::runtime_error with the offending paths on any load failure.
std::shared_ptr<const fizz::server::FizzServerContext>
buildFizzServerContext(const config::TlsMode& mode, FizzContextOptions options);

} // namespace openmoq::moqx::tls
