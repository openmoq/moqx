#pragma once

#include <memory>
#include <optional>

#include <folly/executors/IOThreadPoolExecutor.h>
#include <moqx/MoqxRelayContext.h>
#include <moqx/config/config.h>
#include <moqx/stats/StatsRegistry.h>
#include <moxygen/MoQServer.h>
#include <quic/server/QuicServerTransport.h>

namespace openmoq::moqx {

class MoqxRelayServer : public moxygen::MoQServer {
public:
  MoqxRelayServer(
      const config::ListenerConfig& listenerCfg,
      std::shared_ptr<MoqxRelayContext> context,
      std::shared_ptr<folly::IOThreadPoolExecutor> ioExecutor,
      std::optional<quic::TransportSettings> transportSettings = std::nullopt
  );

  ~MoqxRelayServer() override;

  void setStatsRegistry(std::shared_ptr<stats::StatsRegistry> registry);

  // Preferred entry point: binds the address from the stored ListenerConfig.
  void start();

  // Satisfies MoQServerBase pure virtual; delegates to start().
  void start(const folly::SocketAddress& addr) override;

  void onNewSession(std::shared_ptr<moxygen::MoQSession> clientSession) override;

  void terminateClientSession(std::shared_ptr<moxygen::MoQSession> session) override;

  folly::Expected<folly::Unit, moxygen::SessionCloseErrorCode> validateAuthority(
      const moxygen::ClientSetup& clientSetup,
      uint64_t negotiatedVersion,
      std::shared_ptr<moxygen::MoQSession> session
  ) override;

protected:
  std::shared_ptr<moxygen::MoQSession> createSession(
      folly::MaybeManagedPtr<proxygen::WebTransport> wt,
      std::shared_ptr<moxygen::MoQExecutor> executor
  ) override;

private:
  config::ListenerConfig listenerCfg_;
  std::shared_ptr<MoqxRelayContext> context_;
  std::shared_ptr<folly::IOThreadPoolExecutor> ioExecutor_;
};

} // namespace openmoq::moqx
