/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <memory>

#include "MoqxRelayContext.h"
#include "config/Config.h"
#include "stats/StatsRegistry.h"
#include <folly/executors/IOThreadPoolExecutor.h>
#include <moxygen/MoQServer.h>
#include <moxygen/mlog/MLoggerFactory.h>

namespace openmoq::moqx {

class MoqxRelayServer : public moxygen::MoQServer {
public:
  MoqxRelayServer(
      const config::ListenerConfig& listenerCfg,
      std::shared_ptr<MoqxRelayContext> context,
      folly::IOThreadPoolExecutor* ioExecutor
  );

  ~MoqxRelayServer() override;

  // Idempotent; safe to call from main and again from ~MoqxRelayServer.
  void stop() override;

  void setStatsRegistry(std::shared_ptr<stats::StatsRegistry> registry);

  void setMLoggerFactory(std::shared_ptr<moxygen::MLoggerFactory> factory) {
    moxygen::MoQServerBase::setMLoggerFactory(std::move(factory));
  }

  void setQLogConfig(const config::QLogConfig& cfg) {
    qlogDir_ = cfg.dir;
    qlogSampleRate_ = cfg.sampleRate;
  }

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

  std::shared_ptr<quic::QLogger> makeQLogger(quic::VantagePoint vantagePoint) override;

private:
  config::ListenerConfig listenerCfg_;
  std::shared_ptr<MoqxRelayContext> context_;
  folly::IOThreadPoolExecutor* ioExecutor_;
  bool stopped_{false};
  std::string qlogDir_;
  float qlogSampleRate_{0.0f};
};

} // namespace openmoq::moqx
