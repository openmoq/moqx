#pragma once

#include <string>

#include <folly/Expected.h>

#include "o_rly/config/loader/parsed_config.h"
#include "o_rly/config/resolved_config.h"

namespace openmoq::o_rly::tls {
class TlsProviderRegistry;
} // namespace openmoq::o_rly::tls

namespace openmoq::o_rly::config {

/// Validate and resolve a ParsedConfig into concrete Config types.
/// Returns warnings alongside the config on success.
/// On validation failure, returns a combined error string.
folly::Expected<ResolvedConfig, std::string>
resolveConfig(const ParsedConfig& config, const tls::TlsProviderRegistry& registry);

} // namespace openmoq::o_rly::config
