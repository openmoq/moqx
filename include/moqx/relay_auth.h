/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <moxygen/MoQTypes.h>
#include <moxygen/Publisher.h>
#include <optional>
#include <string>

namespace openmoq::moqx {

// Randomly chosen token type identifying a relay-to-relay peering subNs.
// When a relay receives a SubscribeNamespace carrying this token type it knows
// the sender is a relay peer and should reciprocate with its own peer subNs.
static constexpr uint64_t kRelayAuthTokenType = 0xA3F7'C291'5B84'E60DULL;

// Returns true if subNs carries a relay peer auth token (kRelayAuthTokenType).
bool isPeerSubNs(const moxygen::SubscribeNamespace& subNs);

// Builds a wildcard SubscribeNamespace(prefix={}, BOTH).
// If relayID is provided, includes the relay auth token (initiating peer).
// Without relayID the subNs has no token (reciprocal response — prevents loops).
moxygen::SubscribeNamespace makePeerSubNs(
    std::optional<std::string> relayID = std::nullopt);

// No-op NamespacePublishHandle used for outgoing peer subNs where we don't
// expect namespace announcements from the peer (strict relay hierarchy).
// Draft 16+ NAMESPACE messages on the bidi stream are silently dropped.
class NullNamespacePublishHandle : public moxygen::Publisher::NamespacePublishHandle {
 public:
  void namespaceMsg(const moxygen::TrackNamespace&) override {}
  void namespaceDoneMsg(const moxygen::TrackNamespace&) override {}
};

} // namespace openmoq::moqx
