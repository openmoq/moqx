/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "logging/access/AccessLogSetup.h"

#include <unistd.h>

#include <fmt/format.h>
#include <folly/Random.h>

#include "logging/access/FileTransport.h"
#include "moqx/Version.h"

namespace openmoq::moqx::logging {

namespace {

constexpr size_t kEventCount = size_t(AccessLogEvent::LOG_DROPPED) + 1;

std::string hostName() {
  // The POSIX limit. macOS does not define HOST_NAME_MAX.
  constexpr size_t kHostNameMax = 255;
  char buf[kHostNameMax + 1] = {};
  if (::gethostname(buf, sizeof(buf) - 1) != 0) {
    return {};
  }
  return buf;
}

} // namespace

AccessLogSinkFilter makeAccessLogSinkFilter(const config::AccessLogSinkConfig& sink) {
  AccessLogSinkFilter filter;
  for (size_t e = 0; e < kEventCount; ++e) {
    auto event = AccessLogEvent(e);
    if (event == AccessLogEvent::LOG_DROPPED) {
      continue;
    }
    auto message = accessLogMessage(event);
    bool take = sink.events.empty() && !message;
    for (const auto& name : sink.events) {
      if (parseAccessLogEvent(name) == event || (message && name == kAccessLogMessages) ||
          (!message && parseAccessLogClass(name) == accessLogClass(event))) {
        take = true;
        break;
      }
    }
    filter.events[e] = take;
  }
  filter.errorsOnly = sink.errorsOnly;
  filter.services = sink.services;
  filter.peerClasses = sink.peerClasses;
  filter.sessionSample = sink.sessionSample;
  return filter;
}

folly::Expected<std::shared_ptr<MoqxAccessLog>, std::string>
makeAccessLog(const config::Config& config) {
  if (!config.logging || !config.logging->access || config.logging->access->sinks.empty()) {
    return std::make_shared<MoqxAccessLog>(
        AccessLogRelayInfo{},
        std::vector<std::unique_ptr<AccessLogSink>>{},
        0
    );
  }
  const auto& logging = *config.logging;
  const auto& access = *logging.access;

  AccessLogRelayInfo relay;
  relay.relayId = config.relayID;
  relay.bootId = fmt::format("{:016x}", folly::Random::rand64());
  relay.host = hostName();
  relay.moqxVersion = std::string(kVersion);

  std::vector<std::unique_ptr<AccessLogSink>> sinks;
  for (const auto& sinkConfig : access.sinks) {
    auto fail = [&](std::string_view error) {
      return folly::makeUnexpected(fmt::format("access log sink '{}': {}", sinkConfig.name, error));
    };
    auto encoder =
        makeTemplateEncoder(sinkConfig.classFormats, sinkConfig.eventFormats, access.vars);
    if (encoder.hasError()) {
      return fail(encoder.error());
    }

    std::unique_ptr<AccessLogTransport> transport;
    std::string error;
    std::visit(
        [&](const config::AccessLogFileOutput& file) {
          if (file.path == "-") {
            transport = FileTransport::standardOutput();
            return;
          }
          auto opened = FileTransport::open(file.path);
          if (opened.hasError()) {
            error = fmt::format("cannot open '{}': {}", file.path, opened.error());
            return;
          }
          transport = std::move(*opened);
        },
        sinkConfig.output
    );
    if (!transport) {
      return fail(error);
    }

    AccessLogSinkOptions options;
    options.name = sinkConfig.name;
    options.filter = makeAccessLogSinkFilter(sinkConfig);
    options.batchRecords = sinkConfig.batchRecords;
    options.flushInterval = std::chrono::milliseconds(sinkConfig.flushMs);
    sinks.push_back(std::make_unique<AccessLogSink>(
        std::move(options),
        std::move(*encoder),
        std::move(transport)
    ));
  }
  return std::make_shared<MoqxAccessLog>(std::move(relay), std::move(sinks), access.queueLimit);
}

} // namespace openmoq::moqx::logging
