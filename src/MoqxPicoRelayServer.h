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
#include <moxygen/events/MoQExecutor.h>
#include <moxygen/mlog/MLoggerFactory.h>
#include <moxygen/openmoq/transport/pico/MoQPicoQuicShardedServer.h>
#include <proxygen/lib/http/webtransport/WebTransport.h>

namespace openmoq::moqx {

// MoQ relay server backed by the picoquic QUIC stack.
//
// Shards across every EventBase in the supplied IOThreadPoolExecutor via
// SO_REUSEPORT (see MoQPicoQuicShardedServer). QUIC connection migration is
// disabled automatically whenever more than one shard is in use, since
// plain SO_REUSEPORT hashing cannot route a migrated connection's packets
// to the shard holding its state.
class MoqxPicoRelayServer : public moxygen::MoQPicoQuicShardedServer {
public:
  MoqxPicoRelayServer(
      const config::ListenerConfig& listenerCfg,
      std::shared_ptr<MoqxRelayContext> context,
      folly::IOThreadPoolExecutor* ioExecutor
  );

  ~MoqxPicoRelayServer() override;

  // Idempotent; safe to call from main and again from ~MoqxPicoRelayServer.
  void stop() override;

  void setStatsRegistry(std::shared_ptr<stats::StatsRegistry> registry);

  void setMLoggerFactory(std::shared_ptr<moxygen::MLoggerFactory> factory) {
    moxygen::MoQServerBase::setMLoggerFactory(std::move(factory));
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

private:
  config::ListenerConfig listenerCfg_;
  std::shared_ptr<MoqxRelayContext> context_;
  folly::IOThreadPoolExecutor* ioExecutor_;
  bool stopped_{false};
};

} // namespace openmoq::moqx
