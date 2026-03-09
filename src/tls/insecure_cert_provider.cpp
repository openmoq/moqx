#include "moqx/tls/insecure_cert_provider.h"

#include <proxygen/httpserver/samples/hq/FizzContext.h>

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
