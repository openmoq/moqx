#include <moxygen/MoQRelaySession.h>
#include <o_rly/ORelayServer.h>

#include <folly/logging/xlog.h>

using namespace moxygen;

namespace openmoq::o_rly {

namespace {

std::vector<std::string> buildAlpns(const std::string& versions) {
  std::vector<std::string> alpns = {"h3"};
  auto moqt = getMoqtProtocols(versions, true);
  alpns.insert(alpns.end(), moqt.begin(), moqt.end());
  return alpns;
}

} // namespace

void ORelayServer::initRelays(const std::vector<config::ServiceConfig>& services) {
  relays_.reserve(services.size());
  for (const auto& svc : services) {
    relays_.push_back(
        std::make_shared<ORelay>(svc.cache.maxCachedTracks, svc.cache.maxCachedGroupsPerTrack)
    );
  }
}

ORelayServer::ORelayServer(
    const std::string& cert,
    const std::string& key,
    const std::string& endpoint,
    const std::string& versions,
    std::vector<config::ServiceConfig> services
)
    : MoQServer(
          quic::samples::createFizzServerContext(
              buildAlpns(versions),
              fizz::server::ClientAuthMode::Optional,
              cert,
              key
          ),
          endpoint
      ),
      serviceMatcher_(services) {
  initRelays(services);
}

ORelayServer::ORelayServer(
    const std::string& endpoint,
    const std::string& versions,
    std::vector<config::ServiceConfig> services
)
    : MoQServer(
          quic::samples::createFizzServerContextWithInsecureDefault(
              buildAlpns(versions),
              fizz::server::ClientAuthMode::None,
              "" /* cert */,
              "" /* key */
          ),
          endpoint
      ),
      serviceMatcher_(services) {
  initRelays(services);
}

void ORelayServer::onNewSession(std::shared_ptr<MoQSession> /*clientSession*/) {
  // Routing deferred to validateAuthority() where authority is available
}

folly::Expected<folly::Unit, SessionCloseErrorCode> ORelayServer::validateAuthority(
    const ClientSetup& clientSetup,
    uint64_t negotiatedVersion,
    std::shared_ptr<MoQSession> session
) {
  // Base class format validation
  auto base = MoQServer::validateAuthority(clientSetup, negotiatedVersion, session);
  if (!base.hasValue()) {
    return base;
  }

  // Match service by authority + path
  const auto& authority = session->getAuthority();
  const auto& path = session->getPath();
  auto idx = serviceMatcher_.match(authority, path);
  if (!idx) {
    XLOG(ERR) << "No service matched authority=" << authority << " path=" << path;
    return folly::makeUnexpected(SessionCloseErrorCode::INVALID_AUTHORITY);
  }

  // Route: set per-service relay as handler
  const auto& relay = relays_[*idx];
  session->setPublishHandler(relay);
  session->setSubscribeHandler(relay);
  return folly::unit;
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
