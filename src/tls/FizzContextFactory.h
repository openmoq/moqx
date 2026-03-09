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
#include <folly/Expected.h>

namespace openmoq::moqx::tls {

/// Build a standard FizzServerContext from a CertManager and ALPN list.
/// Configures ticket cipher, client auth, early data, etc.
folly::Expected<std::shared_ptr<const fizz::server::FizzServerContext>, std::string>
buildStandardFizzContext(
    std::shared_ptr<fizz::server::CertManager> certManager,
    const std::vector<std::string>& alpns
);

/// Build ALPN list from a comma-separated MOQT versions string.
std::vector<std::string> buildAlpns(const std::string& moqtVersions);

} // namespace openmoq::moqx::tls
