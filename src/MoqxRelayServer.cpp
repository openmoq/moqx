/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "MoqxRelayServer.h"
#include "stats/EventBaseStatsCollector.h"
#include "stats/QuicStatsCollector.h"
#include <moxygen/MoQRelaySession.h>
#include <moxygen/events/MoQFollyExecutorImpl.h>
#include <moxygen/util/InsecureVerifierDangerousDoNotUseInProduction.h>
#include <proxygen/httpserver/samples/hq/FizzContext.h>

#include <folly/logging/xlog.h>
#include <quic/QuicConstants.h>

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

quic::TransportSettings
buildTransportSettings(const config::QuicConfig& quic, const config::MvfstConfig& mvfst) {
  // Start with MoQServer's optimized defaults, then apply config overrides.
  quic::TransportSettings ts;
  ts.defaultCongestionController = quic::CongestionControlType::Copa;
  ts.pacingEnabled = mvfst.pacingEnabled;
  ts.maxCwndInMss = mvfst.maxCwndInMss;
  ts.batchingMode = mvfst.enableGSO ? quic::QuicBatchingMode::BATCHING_MODE_GSO
                                    : quic::QuicBatchingMode::BATCHING_MODE_SENDMMSG;
  ts.maxBatchSize = mvfst.maxConnPacketsSentPerLoop;
  ts.writeConnectionDataPacketsLimit = mvfst.maxConnPacketsSentPerLoop;
  ts.dataPathType = quic::DataPathType::ContinuousMemory;
  ts.shouldUseWrapperRecvmmsgForBatchRecv = mvfst.useRecvmmsg;
  ts.maxServerRecvPacketsPerLoop = mvfst.maxServerRecvPacketsPerLoop;
  ts.maxRecvBatchSize = mvfst.maxServerRecvPacketsPerLoop;
  ts.numGROBuffers_ = mvfst.numGROBuffers;
  ts.advertisedInitialConnectionFlowControlWindow = quic.maxData;
  ts.advertisedInitialBidiLocalStreamFlowControlWindow = quic.maxStreamData;
  ts.advertisedInitialBidiRemoteStreamFlowControlWindow = quic.maxStreamData;
  ts.advertisedInitialUniStreamFlowControlWindow = quic.maxStreamData;
  ts.advertisedInitialMaxStreamsBidi = quic.maxBidiStreams;
  ts.advertisedInitialMaxStreamsUni = quic.maxUniStreams;
  ts.idleTimeout = std::chrono::milliseconds(quic.idleTimeoutMs);
  ts.minAckDelay = std::chrono::microseconds(quic.minAckDelayUs);
  if (quic.maxAckDelayUs != config::QuicConfig{}.maxAckDelayUs) {
    XLOG(DBG1) << "quic.max_ack_delay_us is set but mvfst does not support it; ignoring";
  }
  auto ccType = quic::congestionControlStrToType(quic.ccAlgo);
  XDCHECK(ccType) << "cc_algo '" << quic.ccAlgo << "' passed validation but is unknown to mvfst";
  if (ccType) {
    ts.defaultCongestionController = *ccType;
  }
  // Wire CongestionControlConfig fields.
  auto& cca = ts.ccaConfig;
  // Copa
  ts.copaDeltaParam = mvfst.copa.deltaParam;

  // BBR
  cca.conservativeRecovery = mvfst.bbr.conservativeRecovery;
  cca.largeProbeRttCwnd = mvfst.bbr.largeProbeRttCwnd;
  cca.enableAckAggregationInStartup = mvfst.bbr.enableAckAggregationInStartup;
  cca.probeRttDisabledIfAppLimited = mvfst.bbr.probeRttDisabledIfAppLimited;
  cca.drainToTarget = mvfst.bbr.drainToTarget;

  // Cubic
  cca.additiveIncreaseAfterHystart = mvfst.cubic.additiveIncreaseAfterHystart;
  cca.onlyGrowCwndWhenLimited = mvfst.cubic.onlyGrowCwndWhenLimited;
  cca.leaveHeadroomForCwndLimited = mvfst.cubic.leaveHeadroomForCwndLimited;

  // BBR2
  cca.ignoreInflightLongTerm = mvfst.bbr2.ignoreInflightLongTerm;
  cca.ignoreShortTerm = mvfst.bbr2.ignoreShortTerm;
  cca.exitStartupOnLoss = mvfst.bbr2.exitStartupOnLoss;
  cca.enableRecoveryInStartup = mvfst.bbr2.enableRecoveryInStartup;
  cca.enableRecoveryInProbeStates = mvfst.bbr2.enableRecoveryInProbeStates;
  cca.enableRenoCoexistence = mvfst.bbr2.enableRenoCoexistence;
  cca.paceInitCwnd = mvfst.bbr2.paceInitCwnd;
  cca.overrideCruisePacingGain = mvfst.bbr2.overrideCruisePacingGain;
  cca.overrideCruiseCwndGain = mvfst.bbr2.overrideCruiseCwndGain;
  cca.overrideStartupPacingGain = mvfst.bbr2.overrideStartupPacingGain;
  cca.overrideBwShortBeta = mvfst.bbr2.overrideBwShortBeta;

  // L4S
  cca.l4sCETarget = mvfst.l4s.ceTarget;
  // TODO: wire defaultStreamPriority / defaultDatagramPriority for mvfst once
  // moxygen exposes a function to construct a PriorityQueue::Priority from an integer.
  return ts;
}

} // namespace

MoqxRelayServer::MoqxRelayServer(
    const config::ListenerConfig& listenerCfg,
    std::shared_ptr<MoqxRelayContext> context,
    folly::IOThreadPoolExecutor* ioExecutor
)
    : MoQServer(
          buildFizzContext(listenerCfg),
          listenerCfg.endpoint,
          MoQServer::Options{
              .transportSettings = buildTransportSettings(listenerCfg.quic, listenerCfg.mvfst),
              .udpSendBufferBytes = listenerCfg.mvfst.udpSocketBufferBytes,
              .udpRecvBufferBytes = listenerCfg.mvfst.udpSocketBufferBytes,
          }
      ),
      listenerCfg_(listenerCfg), context_(std::move(context)), ioExecutor_(ioExecutor) {}

MoqxRelayServer::~MoqxRelayServer() {
  // Close incoming connections, drain worker EVBs, then destroy EVBs.
  stop();
}

void MoqxRelayServer::stop() {
  if (stopped_) {
    return;
  }
  stopped_ = true;
  // Keep context_ alive: terminateClientSession can run after stop() returns,
  // from handleClientSession coroutines still draining on IO threads.
  MoQServer::stop();
}

void MoqxRelayServer::setStatsRegistry(std::shared_ptr<stats::StatsRegistry> registry) {
  context_->setStatsRegistry(registry);
  for (auto& ka : ioExecutor_->getAllEventBases()) {
    stats::EventBaseStatsCollector::create(registry, ka.get());
  }
  setQuicStatsFactory(std::make_unique<stats::QuicStatsCollector::Factory>(std::move(registry)));
}

void MoqxRelayServer::start() {
  auto evbKAs = ioExecutor_->getAllEventBases();
  std::vector<folly::EventBase*> evbs;
  evbs.reserve(evbKAs.size());
  for (auto& ka : evbKAs) {
    evbs.push_back(ka.get());
  }
  ioExecutor_ = nullptr;
  MoQServer::start(listenerCfg_.address, std::move(evbs));
}

void MoqxRelayServer::start(const folly::SocketAddress& /*addr*/) {
  start();
}

void MoqxRelayServer::onNewSession(std::shared_ptr<MoQSession> clientSession) {
  context_->onNewSession(std::move(clientSession));
}

void MoqxRelayServer::terminateClientSession(std::shared_ptr<MoQSession> session) {
  context_->onSessionEnd(session);
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
