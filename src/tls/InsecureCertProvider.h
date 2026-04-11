/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include "TlsCertLoader.h"

namespace openmoq::moqx::tls {

/// TLS provider using compiled-in self-signed certs for development.
class InsecureCertProvider : public TlsCertProvider {
public:
  folly::Expected<std::shared_ptr<const fizz::server::FizzServerContext>, std::string>
  createContext(const std::vector<std::string>& alpns, const std::vector<TicketSeed>& ticketSeeds)
      const override;

  folly::Expected<std::string, std::string> getKeyPath() const override {
    return folly::makeExpected<std::string>("");
  }

  folly::Expected<std::string, std::string> getCertPath() const override {
    return folly::makeExpected<std::string>("");
  }
};

} // namespace openmoq::moqx::tls
