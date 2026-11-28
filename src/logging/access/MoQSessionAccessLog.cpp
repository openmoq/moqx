/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "logging/access/MoQSessionAccessLog.h"

#include <folly/Conv.h>
#include <moxygen/MoQVersions.h>
#include <moxygen/events/MoQFollyExecutorImpl.h>

namespace openmoq::moqx::logging {

namespace {

class MoQSessionProbe : public AccessLogSessionProbe {
public:
  explicit MoQSessionProbe(std::weak_ptr<moxygen::MoQSession> session)
      : session_(std::move(session)) {}

  bool closed() const override {
    auto session = session_.lock();
    return !session || session->isClosed();
  }

  void addTransportStats(AccessLogRecord& record) const override {
    auto session = session_.lock();
    if (!session) {
      return;
    }
    auto info = session->getTransportInfo();
    if (info.srtt.count() == 0) {
      return;
    }
    record.srttUs = info.srtt.count();
    if (info.maybeMinRtt) {
      record.minRttUs = info.maybeMinRtt->count();
    }
    record.cwnd = info.congestionWindow;
    record.inflight = info.bytesInFlight;
    record.pktsSent = info.totalPacketsSent;
    record.pktsLost = info.totalPacketsMarkedLost;
    record.bytesRetx = info.totalBytesRetransmitted;
  }

private:
  std::weak_ptr<moxygen::MoQSession> session_;
};

} // namespace

AccessLogSessionSetup
accessLogSetup(const moxygen::MoQSession& session, std::string service, std::string peerClass) {
  return {
      .service = std::move(service),
      .peerClass = std::move(peerClass),
      .version = moxygen::getDraftMajorVersion(session.getNegotiatedVersion()),
      .authority = session.getAuthority(),
      .path = session.getPath(),
  };
}

AccessLogSessionSource accessLogSource(
    const std::shared_ptr<moxygen::MoQSession>& session,
    std::string upstream,
    const folly::SocketAddress& local
) {
  AccessLogSessionSource source{
      .sessionId = folly::to<std::string>(session->sessionId().value),
      .peer = session->getPeerAddress(),
      .local = local,
      .upstream = std::move(upstream),
      .probe = std::make_unique<MoQSessionProbe>(session),
  };
  if (auto* exec = dynamic_cast<moxygen::MoQFollyExecutorImpl*>(session->getExecutor())) {
    source.evb = exec->getBackingEventBase();
  }
  return source;
}

std::optional<uint64_t> accessLogCloseCode(const moxygen::MoQSession& session) {
  if (const auto& result = session.getCloseResult()) {
    return folly::to_underlying(result->error);
  }
  return std::nullopt;
}

} // namespace openmoq::moqx::logging
