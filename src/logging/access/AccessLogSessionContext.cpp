/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "logging/access/AccessLogSessionContext.h"

#include <folly/IPAddress.h>
#include <folly/logging/xlog.h>
#include <moxygen/MoQVersions.h>

#include "relay/ExecutorCheck.h"

namespace openmoq::moqx::logging {

uint64_t msSince(std::chrono::steady_clock::time_point start) {
  auto elapsed = std::chrono::steady_clock::now() - start;
  return std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count();
}

namespace {

void readSetupFields(AccessLogSessionInfo& info, const moxygen::MoQSession& session) {
  if (auto version = session.getNegotiatedVersion()) {
    info.version = moxygen::getDraftMajorVersion(*version);
  }
  info.authority = session.getAuthority();
  info.pathRaw = session.getPath();
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

void AccessLogSessionContext::setup(
    const std::shared_ptr<moxygen::MoQSession>& session,
    std::string service,
    std::string peerClass,
    std::string upstream,
    const folly::SocketAddress& local
) {
  // Records share info_, so setup() builds a new one.
  auto info = std::make_shared<AccessLogSessionInfo>(*info_);
  info->session = folly::to<std::string>(session->sessionId().value);
  info->service = std::move(service);
  info->peerClass = std::move(peerClass);
  info->upstream = std::move(upstream);
  auto peer = session->getPeerAddress();
  if (peer.isInitialized()) {
    info->peer = peer.describe();
    info->peerTrunc = truncatePeerAddress(peer);
  }
  // TODO: every session on a listener has the same local address, so its string could be
  // rendered once and shared.
  if (local.isInitialized()) {
    info->local = local.describe();
  }
  readSetupFields(*info, *session);
  info_ = std::move(info);
  gate_ = log_->gateSession(info_->service, info_->peerClass, info_->session);
  session_ = session;
  exec_ = session->getExecutor();
}

void AccessLogSessionContext::setService(const moxygen::MoQSession& session, std::string service) {
  auto info = std::make_shared<AccessLogSessionInfo>(*info_);
  info->service = std::move(service);
  readSetupFields(*info, session);
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

void AccessLogSessionContext::writeSessionEnd(const moxygen::MoQSession& session) {
  std::optional<uint64_t> errorCode;
  if (const auto& result = session.getCloseResult()) {
    errorCode = folly::to_underlying(result->error);
  }
  writeSessionEnd(errorCode);
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
  });
}

AccessLogSessionState AccessLogSessionContext::state() const {
  if (log_->shuttingDown()) {
    return AccessLogSessionState::ShuttingDown;
  }
  if (!exec_) {
    return AccessLogSessionState::Open;
  }
  XDCHECK(onExecutor(exec_)) << "access log session read off its executor";
  auto session = session_.lock();
  if (!session || session->isClosed()) {
    return AccessLogSessionState::Closed;
  }
  return AccessLogSessionState::Open;
}

} // namespace openmoq::moqx::logging
