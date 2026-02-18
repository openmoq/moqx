/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the MIT license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <o_rly/ORelay.h>
#include <moxygen/MoQServer.h>

namespace openmoq::o_rly {

class ORelayServer : public moxygen::MoQServer {
 public:
  // Used when the insecure flag is false
  ORelayServer(
      const std::string& cert,
      const std::string& key,
      const std::string& endpoint,
      const std::string& versions,
      size_t maxCachedTracks,
      size_t maxCachedGroupsPerTrack);

  // Used when the insecure flag is true
  ORelayServer(
      const std::string& endpoint,
      const std::string& versions,
      size_t maxCachedTracks,
      size_t maxCachedGroupsPerTrack);

  void onNewSession(
      std::shared_ptr<moxygen::MoQSession> clientSession) override;

 protected:
  std::shared_ptr<moxygen::MoQSession> createSession(
      folly::MaybeManagedPtr<proxygen::WebTransport> wt,
      std::shared_ptr<moxygen::MoQExecutor> executor) override;

 private:
  std::shared_ptr<ORelay> relay_;
};

} // namespace openmoq::o_rly
