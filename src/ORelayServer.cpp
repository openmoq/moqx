#include <moxygen/MoQRelaySession.h>
#include <o_rly/ORelayServer.h>

using namespace moxygen;

namespace openmoq::o_rly {

ORelayServer::ORelayServer(
    std::shared_ptr<const fizz::server::FizzServerContext> fizzContext,
    const std::string& endpoint,
    size_t maxCachedTracks,
    size_t maxCachedGroupsPerTrack
)
    : MoQServer(std::move(fizzContext), endpoint),
      relay_(std::make_shared<ORelay>(maxCachedTracks, maxCachedGroupsPerTrack)) {}

void ORelayServer::onNewSession(std::shared_ptr<MoQSession> clientSession) {
  clientSession->setPublishHandler(relay_);
  clientSession->setSubscribeHandler(relay_);
}

std::shared_ptr<MoQSession> ORelayServer::createSession(
    folly::MaybeManagedPtr<proxygen::WebTransport> wt,
    std::shared_ptr<MoQExecutor> executor
) {
  return std::make_shared<MoQRelaySession>(
      folly::MaybeManagedPtr<proxygen::WebTransport>(std::move(wt)),
      *this,
      std::move(executor)
  );
}

} // namespace openmoq::o_rly
