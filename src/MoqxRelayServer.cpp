#include <moqx/MoqxRelayServer.h>
#include <moqx/stats/QuicStatsCollector.h>
#include <moxygen/MoQRelaySession.h>
#include <moxygen/events/MoQFollyExecutorImpl.h>
#include <moxygen/util/InsecureVerifierDangerousDoNotUseInProduction.h>
#include <proxygen/httpserver/samples/hq/FizzContext.h>

#include <folly/logging/xlog.h>

using namespace moxygen;

namespace openmoq::moqx {

namespace {

std::vector<std::string> buildAlpns(const std::string& versions) {
  std::vector<std::string> alpns = {"h3"};
  auto moqt = getMoqtProtocols(versions, true);
  alpns.insert(alpns.end(), moqt.begin(), moqt.end());
  return alpns;
}

std::shared_ptr<const fizz::server::FizzServerContext>
buildFizzContext(const config::ListenerConfig& cfg) {
  auto alpns = buildAlpns(cfg.moqtVersions);
  return std::visit(
      [&alpns](const auto& tls) -> std::shared_ptr<const fizz::server::FizzServerContext> {
        using T = std::decay_t<decltype(tls)>;
        if constexpr (std::is_same_v<T, config::Insecure>) {
          return quic::samples::createFizzServerContextWithInsecureDefault(
              alpns,
              fizz::server::ClientAuthMode::None,
              "",
              ""
          );
        } else {
          return quic::samples::createFizzServerContext(
              alpns,
              fizz::server::ClientAuthMode::Optional,
              tls.certFile,
              tls.keyFile
          );
        }
      },
      cfg.tlsMode
  );
}

} // namespace

MoqxRelayServer::MoqxRelayServer(
    const config::ListenerConfig& listenerCfg,
    std::shared_ptr<MoqxRelayContext> context,
    std::shared_ptr<folly::IOThreadPoolExecutor> ioExecutor,
    std::optional<quic::TransportSettings> transportSettings
)
    : MoQServer(buildFizzContext(listenerCfg), listenerCfg.endpoint, std::move(transportSettings)),
      listenerCfg_(listenerCfg), context_(std::move(context)), ioExecutor_(std::move(ioExecutor)) {}

MoqxRelayServer::~MoqxRelayServer() {
  // Close incoming connections, drain worker EVBs, then destroy EVBs.
  MoQServer::stop();
}

void MoqxRelayServer::setStatsRegistry(std::shared_ptr<stats::StatsRegistry> registry) {
  context_->setStatsRegistry(registry);
  setQuicStatsFactory(std::make_unique<stats::QuicStatsCollector::Factory>(std::move(registry)));
}

void MoqxRelayServer::start() {
  auto evbKAs = ioExecutor_->getAllEventBases();
  std::vector<folly::EventBase*> evbs;
  evbs.reserve(evbKAs.size());
  for (auto& ka : evbKAs) {
    evbs.push_back(ka.get());
  }
  MoQServer::start(listenerCfg_.address, std::move(evbs));
}

void MoqxRelayServer::start(const folly::SocketAddress& /*addr*/) {
  start();
}

void MoqxRelayServer::onNewSession(std::shared_ptr<MoQSession> clientSession) {
  context_->onNewSession(std::move(clientSession));
}

void MoqxRelayServer::terminateClientSession(std::shared_ptr<MoQSession> session) {
  context_->onSessionEnd();
  MoQServer::terminateClientSession(std::move(session));
}

folly::Expected<folly::Unit, SessionCloseErrorCode> MoqxRelayServer::validateAuthority(
    const ClientSetup& clientSetup,
    uint64_t negotiatedVersion,
    std::shared_ptr<MoQSession> session
) {
  // Base class format validation
  auto base = MoQServer::validateAuthority(clientSetup, negotiatedVersion, session);
  if (!base.hasValue()) {
    return base;
  }
  return context_->validateAuthority(clientSetup, negotiatedVersion, std::move(session));
}

std::shared_ptr<MoQSession> MoqxRelayServer::createSession(
    folly::MaybeManagedPtr<proxygen::WebTransport> wt,
    std::shared_ptr<MoQExecutor> executor
) {
  return std::make_shared<MoQRelaySession>(
      folly::MaybeManagedPtr<proxygen::WebTransport>(std::move(wt)),
      *this,
      std::move(executor)
  );
}

} // namespace openmoq::moqx
