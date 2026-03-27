/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <moqx/relay_auth.h>

using namespace moxygen;

namespace openmoq::moqx {

bool isPeerSubNs(const SubscribeNamespace& subNs) {
  const uint64_t authKey =
      static_cast<uint64_t>(TrackRequestParamKey::AUTHORIZATION_TOKEN);
  for (const auto& param : subNs.params) {
    if (param.key == authKey &&
        param.asAuthToken.tokenType == kRelayAuthTokenType) {
      return true;
    }
  }
  return false;
}

moxygen::SubscribeNamespace makePeerSubNs(std::optional<std::string> relayID) {
  SubscribeNamespace subNs;
  subNs.trackNamespacePrefix = {}; // wildcard: matches all namespaces
  subNs.options = SubscribeNamespaceOptions::BOTH;
  if (relayID) {
    subNs.params.insertParam(Parameter(
        static_cast<uint64_t>(TrackRequestParamKey::AUTHORIZATION_TOKEN),
        AuthToken{
            .tokenType = kRelayAuthTokenType,
            .tokenValue = *relayID,
            .alias = AuthToken::DontRegister,
        }));
  }
  return subNs;
}

} // namespace openmoq::moqx
