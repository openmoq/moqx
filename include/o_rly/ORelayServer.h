#pragma once

#include <moxygen/MoQServer.h>
#include <o_rly/ORelay.h>

namespace openmoq::o_rly {

class ORelayServer : public moxygen::MoQServer {
public:
  ORelayServer(
      std::shared_ptr<const fizz::server::FizzServerContext> fizzContext,
      const std::string& endpoint,
      size_t maxCachedTracks,
      size_t maxCachedGroupsPerTrack
  );

  void onNewSession(std::shared_ptr<moxygen::MoQSession> clientSession) override;

protected:
  std::shared_ptr<moxygen::MoQSession> createSession(
      folly::MaybeManagedPtr<proxygen::WebTransport> wt,
      std::shared_ptr<moxygen::MoQExecutor> executor
  ) override;

private:
  std::shared_ptr<ORelay> relay_;
};

} // namespace openmoq::o_rly
