/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "logging/access/AccessLogSessionContext.h"

#include "logging/access/AccessLogPeriodic.h"

#include <folly/IPAddress.h>
#include <folly/io/async/EventBase.h>
#include <folly/logging/xlog.h>

#include "relay/SessionConfined.h"

namespace openmoq::moqx::logging {

uint64_t
msBetween(std::chrono::steady_clock::time_point from, std::chrono::steady_clock::time_point to) {
  return std::chrono::duration_cast<std::chrono::milliseconds>(to - from).count();
}

AccessLogSinkMask AccessLogSessionContext::periodicMask(uint64_t requestID) const {
  return log_->periodicMask(gate_, folly::to<std::string>(info_->session, ":", requestID));
}

bool AccessLogSessionContext::schedulePeriodic(std::weak_ptr<AccessLogRequest> logRequest) {
  auto interval = log_->periodicInterval(gate_);
  if (interval.count() == 0 || !evb_) {
    return false;
  }
  AccessLogPeriodic::on(*evb_, interval).add(std::move(logRequest));
  return true;
}

uint64_t msSince(std::chrono::steady_clock::time_point start) {
  auto elapsed = std::chrono::steady_clock::now() - start;
  return std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count();
}

namespace {

void setSetupFields(AccessLogSessionInfo& info, AccessLogSessionSetup setup) {
  info.service = std::move(setup.service);
  info.peerClass = std::move(setup.peerClass);
  info.version = setup.version;
  info.authority = std::move(setup.authority);
  info.pathRaw = std::move(setup.path);
  info.path = info.pathRaw.substr(0, info.pathRaw.find('?'));
}

} // namespace

std::string truncatePeerAddress(const folly::SocketAddress& addr) {
  if (!addr.isFamilyInet()) {
    return {};
  }
  auto ip = addr.getIPAddress();
  auto bits = ip.isV4() ? 24 : 48;
  return folly::to<std::string>(ip.mask(bits).str(), "/", bits);
}

AccessLogSessionContext::AccessLogSessionContext(
    std::shared_ptr<AccessLog> log,
    AccessLogOpenedBy openedBy,
    std::chrono::steady_clock::time_point openedAt
)
    : log_(std::move(log)), info_(std::make_shared<AccessLogSessionInfo>()), openedAt_(openedAt) {
  info_->openedBy = openedBy;
}

void AccessLogSessionContext::setup(AccessLogSessionSource source, AccessLogSessionSetup setup) {
  // Records share info_, so setup() builds a new one.
  auto info = std::make_shared<AccessLogSessionInfo>(*info_);
  info->session = std::move(source.sessionId);
  info->upstream = std::move(source.upstream);
  if (source.peer.isInitialized()) {
    info->peer = source.peer.describe();
    info->peerTrunc = truncatePeerAddress(source.peer);
  }
  // TODO: every session on a listener has the same local address, so its string could be
  // rendered once and shared.
  if (source.local.isInitialized()) {
    info->local = source.local.describe();
  }
  setSetupFields(*info, std::move(setup));
  info_ = std::move(info);
  gate_ = log_->gateSession(info_->service, info_->peerClass, info_->session);
  probe_ = std::move(source.probe);
  evb_ = source.evb;
}

void AccessLogSessionContext::update(AccessLogSessionSetup setup) {
  auto info = std::make_shared<AccessLogSessionInfo>(*info_);
  setSetupFields(*info, std::move(setup));
  info_ = std::move(info);
  gate_ = log_->gateSession(info_->service, info_->peerClass, info_->session);
}

void AccessLogSessionContext::writeSetup(
    AccessLogStatus status,
    std::optional<uint64_t> errorCode,
    std::string reason
) {
  auto isError = status != AccessLogStatus::Ok;
  write(AccessLogEvent::SETUP, isError, [&](AccessLogRecord& record) {
    record.status = status;
    record.latencyMs = msSince(openedAt_);
    record.errorCode = errorCode;
    record.reason = std::move(reason);
  });
}

void AccessLogSessionContext::writeSessionEnd(std::optional<uint64_t> errorCode) {
  if (std::exchange(ended_, true)) {
    return;
  }
  auto isError = errorCode.value_or(0) != 0;
  write(AccessLogEvent::SESSION_END, isError, [&](AccessLogRecord& record) {
    record.status = isError ? AccessLogStatus::Error : AccessLogStatus::Ok;
    record.errorCode = errorCode;
    record.durationMs = msSince(openedAt_);
    record.requests = requests_;
    addTransportStats(record);
  });
}

void AccessLogSessionContext::addTransportStats(AccessLogRecord& record) const {
  XDCHECK(!evb_ || evb_->isInEventBaseThread()) << "transport stats read off the session's thread";
  if (probe_) {
    probe_->addTransportStats(record);
  }
}

AccessLogSessionState AccessLogSessionContext::state() const {
  if (log_->shuttingDown()) {
    return AccessLogSessionState::ShuttingDown;
  }
  if (!probe_) {
    return AccessLogSessionState::Open;
  }
  XDCHECK(!evb_ || evb_->isInEventBaseThread()) << "access log session read off its thread";
  return probe_->closed() ? AccessLogSessionState::Closed : AccessLogSessionState::Open;
}

} // namespace openmoq::moqx::logging
