/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <proxygen/httpserver/samples/hq/FizzContext.h>

#include "InsecureCertProvider.h"

namespace openmoq::moqx::tls {

folly::Expected<std::shared_ptr<const fizz::server::FizzServerContext>, std::string>
InsecureCertProvider::createContext(const std::vector<std::string>& alpns) const {
  return quic::samples::createFizzServerContextWithInsecureDefault(
      alpns,
      fizz::server::ClientAuthMode::None,
      "",
      ""
  );
}

} // namespace openmoq::moqx::tls
