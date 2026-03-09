#pragma once

#include <string>

#include <folly/Expected.h>

#include "moqx/config/loader/parsed_config.h"
#include "moqx/config/resolved_config.h"

namespace openmoq::moqx::tls {
class TlsProviderRegistry;
} // namespace openmoq::moqx::tls

namespace openmoq::moqx::config {

/// Validate and resolve a ParsedConfig into concrete Config types.
/// Returns warnings alongside the config on success.
/// On validation failure, returns a combined error string.
folly::Expected<ResolvedConfig, std::string>
resolveConfig(const ParsedConfig& config, const tls::TlsProviderRegistry& registry);

} // namespace openmoq::moqx::config
