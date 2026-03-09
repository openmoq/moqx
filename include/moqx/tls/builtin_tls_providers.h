#pragma once

#include <functional>
#include <memory>
#include <string>

#include "moqx/tls/tls_provider_registry.h"

namespace openmoq::moqx::tls {

class TlsProviderRegistry;

void registerBuiltinTlsProviders(TlsProviderRegistry& registry);

/// Build a TlsProviderFactory for the "insecure" type.
/// Optional creator overrides the default InsecureCertProvider.
TlsProviderFactory makeInsecureFactory(
    std::function<std::shared_ptr<TlsCertProvider>()> creator = nullptr);

/// Build a TlsProviderFactory for the "file" type.
/// Validates cert_file/key_file non-empty, then delegates to creator.
TlsProviderFactory makeFileFactory(
    std::function<std::shared_ptr<TlsCertProvider>(std::string, std::string)>
        creator = nullptr);

/// Build a TlsProviderFactory for the "directory" type.
/// Validates cert_dir non-empty, then delegates to creator.
TlsProviderFactory makeDirectoryFactory(
    std::function<std::shared_ptr<TlsCertProvider>(std::string, std::string)>
        creator = nullptr);

} // namespace openmoq::moqx::tls
