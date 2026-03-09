#pragma once

namespace openmoq::o_rly::tls {

class TlsProviderRegistry;

void registerBuiltinTlsProviders(TlsProviderRegistry& registry);

} // namespace openmoq::o_rly::tls
