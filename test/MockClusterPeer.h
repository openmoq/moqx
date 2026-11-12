/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <moxygen/openmoq/extensions/MoQClusterExtension.h>
#include <moxygen/test/MockMoQSession.h>

#include <optional>

namespace openmoq::moqx::test {

// Makes `session` report MoQ Cluster as negotiated with a peer that declared
// `peerHop` and, if set, `peerCost`, as a real SETUP exchange would.
inline void negotiateClusterPeer(
    moxygen::test::MockMoQSession& session,
    uint64_t peerHop,
    std::optional<uint64_t> peerCost = std::nullopt
) {
  using moxygen::MoQClusterExtension;
  moxygen::SetupParameters peer(moxygen::FrameType::SERVER_SETUP);
  for (auto& option :
       MoQClusterExtension(peerHop, peerCost).getSetupOptions(moxygen::kVersionDraft18)) {
    peer.insertParam(std::move(option));
  }
  moxygen::SetupParameters local(moxygen::FrameType::CLIENT_SETUP);
  for (auto& option : MoQClusterExtension(1).getSetupOptions(moxygen::kVersionDraft18)) {
    local.insertParam(std::move(option));
  }
  auto rules = MoQClusterExtension(1).negotiate(moxygen::kVersionDraft18, local, peer);
  XCHECK(rules.hasValue());
  session.messageParameterExtensions = std::move(rules.value());
  session.peerSetupParams = std::move(peer);
}

} // namespace openmoq::moqx::test
