/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "admin/PeeringHandler.h"

#include <initializer_list>
#include <string>
#include <utility>

#include <folly/io/IOBuf.h>
#include <folly/json/dynamic.h>
#include <folly/json/json.h>
#include <folly/logging/xlog.h>
#include <proxygen/httpserver/ResponseBuilder.h>
#include <proxygen/lib/http/HTTPMessage.h>

#include "admin/AdminServer.h"
#include "config/PeerEndpoint.h"
#include "peering/PeerJson.h"

namespace openmoq::moqx::admin {
namespace {

constexpr size_t kMaxBodyBytes = 4096;

void sendJson(proxygen::ResponseHandler* downstream, uint16_t status, const std::string& body) {
  proxygen::ResponseBuilder(downstream)
      .status(status, proxygen::HTTPMessage::getDefaultReason(status))
      .header("Content-Type", "application/json")
      .body(folly::IOBuf::copyBuffer(body))
      .sendWithEOM();
}

void sendError(proxygen::ResponseHandler* downstream, uint16_t status, std::string_view message) {
  sendJson(downstream, status, peering::peeringErrorJson(message));
}

bool contentTypeIsJson(const proxygen::HTTPMessage& req) {
  if (!req.getHeaders().exists("Content-Type")) {
    return true;
  }
  auto type = req.getHeaders().getSingleOrEmpty("Content-Type");
  auto semi = type.find(';');
  if (semi != std::string::npos) {
    type = type.substr(0, semi);
  }
  while (!type.empty() && type.back() == ' ') {
    type.pop_back();
  }
  return type == "application/json";
}

std::string readBody(const folly::IOBuf* body, bool& tooLarge) {
  tooLarge = false;
  if (!body) {
    return {};
  }
  if (body->computeChainDataLength() > kMaxBodyBytes) {
    tooLarge = true;
    return {};
  }
  return body->clone()->moveToFbString().toStdString();
}

bool allowedKey(const folly::dynamic& obj, std::initializer_list<const char*> keys) {
  for (const auto& item : obj.items()) {
    bool found = false;
    for (const char* key : keys) {
      if (item.first.asString() == key) {
        found = true;
        break;
      }
    }
    if (!found) {
      return false;
    }
  }
  return true;
}

struct PeerRequest {
  std::string host;
  uint16_t port{0};
  std::optional<uint64_t> metric;
};

// Positive allow-list for the JSON object. Hosts are normalized the same way
// as config, so a value cannot be a URL or a log-injection payload. Private
// addresses stay legal: peering dials them.
std::optional<std::string> parsePeerRequest(
    const std::string& body,
    bool metricRequired,
    bool metricAllowed,
    PeerRequest& out
) {
  folly::dynamic parsed;
  try {
    parsed = folly::parseJson(body);
  } catch (const std::exception&) {
    return std::string("invalid JSON body");
  }
  if (!parsed.isObject()) {
    return std::string("JSON body must be an object");
  }
  if (metricAllowed) {
    if (!allowedKey(parsed, {"host", "port", "metric"})) {
      return std::string("unknown field in peering request");
    }
  } else if (!allowedKey(parsed, {"host", "port"})) {
    return std::string("unknown field in peering request");
  }

  auto* host = parsed.get_ptr("host");
  auto* port = parsed.get_ptr("port");
  if (!host || !host->isString()) {
    return std::string("host must be a string");
  }
  if (!port || !port->isInt()) {
    return std::string("port must be an integer");
  }
  auto portValue = port->asInt();
  if (portValue < 1 || portValue > 65535) {
    return std::string("port must be from 1 to 65535");
  }

  std::string normalized;
  if (auto error = config::normalizePeerHost(host->asString(), normalized)) {
    return error;
  }
  out.host = std::move(normalized);
  out.port = static_cast<uint16_t>(portValue);

  auto* metric = parsed.get_ptr("metric");
  if (metricRequired && !metric) {
    return std::string("metric is required");
  }
  if (metric) {
    if (metric->isNull() && !metricRequired) {
      return std::nullopt;
    }
    if (!metric->isInt()) {
      return std::string("metric must be an integer");
    }
    auto value = metric->asInt();
    if (value < 0) {
      return std::string("metric must be zero or greater");
    }
    out.metric = static_cast<uint64_t>(value);
  }
  return std::nullopt;
}

void logCommand(std::string_view action, const peering::PeerCommandResult& result) {
  std::string message = result.message;
  for (char& c : message) {
    if (c == '\n' || c == '\r') {
      c = ' ';
    }
  }
  XLOG(INFO) << "peering " << action << " host=" << result.host << " port=" << result.port
             << " ok=" << (result.ok ? "true" : "false") << " message=" << message;
}

} // namespace

void registerPeeringRoutes(
    AdminServer& adminServer,
    config::PeeringMethod method,
    std::optional<uint64_t> clusterHopId,
    std::shared_ptr<peering::MoqPeeringRuntime> runtime
) {
  const bool moqPeering = method == config::PeeringMethod::MoqPeering;

  adminServer.addRoute(
      "GET",
      "/peering",
      [moqPeering, clusterHopId, weak = std::weak_ptr<peering::MoqPeeringRuntime>(runtime)](
          auto /*req*/,
          auto /*body*/,
          auto* downstream,
          auto /*cancelToken*/,
          const auto& /*egress*/
      ) {
        if (!moqPeering) {
          sendJson(
              downstream,
              200,
              peering::clusterPeeringJson(clusterHopId.has_value(), clusterHopId)
          );
          return;
        }
        auto runtimeLocked = weak.lock();
        if (!runtimeLocked) {
          sendError(downstream, 503, "moq-peering is not running");
          return;
        }
        sendJson(downstream, 200, peering::peeringStatusJson(runtimeLocked->status()));
      }
  );

  struct Command {
    const char* path;
    const char* name;
    bool metricRequired;
    bool metricAllowed;
  };
  const Command commands[] = {
      {"/peering/add-peer", "add-peer", false, true},
      {"/peering/remove-peer", "remove-peer", false, false},
      {"/peering/restart-peer", "restart-peer", false, false},
      {"/peering/set-peer-metric", "set-peer-metric", true, true},
  };

  for (const auto& command : commands) {
    adminServer.addRoute(
        "POST",
        command.path,
        [moqPeering,
         weak = std::weak_ptr<peering::MoqPeeringRuntime>(runtime),
         name = std::string(command.name),
         metricRequired = command.metricRequired,
         metricAllowed = command.metricAllowed](
            std::unique_ptr<proxygen::HTTPMessage> req,
            std::unique_ptr<folly::IOBuf> body,
            proxygen::ResponseHandler* downstream,
            auto /*cancelToken*/,
            const auto& /*egress*/
        ) {
          if (!moqPeering) {
            sendError(downstream, 409, "peering method is cluster");
            return;
          }
          auto runtimeLocked = weak.lock();
          if (!runtimeLocked) {
            sendError(downstream, 503, "moq-peering is not running");
            return;
          }
          if (req && !contentTypeIsJson(*req)) {
            sendError(downstream, 415, "Content-Type must be application/json");
            return;
          }
          bool tooLarge = false;
          auto bodyText = readBody(body.get(), tooLarge);
          if (tooLarge) {
            sendError(downstream, 413, "JSON body is larger than 4096 bytes");
            return;
          }
          if (bodyText.empty()) {
            sendError(downstream, 400, "JSON body is required");
            return;
          }
          PeerRequest parsed;
          if (auto error = parsePeerRequest(bodyText, metricRequired, metricAllowed, parsed)) {
            sendError(downstream, 400, *error);
            return;
          }

          peering::PeerCommandResult result;
          if (name == "add-peer") {
            result = runtimeLocked->addPeer(parsed.host, parsed.port, parsed.metric);
          } else if (name == "remove-peer") {
            result = runtimeLocked->removePeer(parsed.host, parsed.port);
          } else if (name == "restart-peer") {
            result = runtimeLocked->restartPeer(parsed.host, parsed.port);
          } else {
            result =
                runtimeLocked->setPeerMetric(parsed.host, parsed.port, parsed.metric.value_or(0));
          }
          logCommand(name, result);
          sendJson(downstream, result.ok ? 200 : 409, peering::peerCommandJson(result));
        }
    );
  }
}

} // namespace openmoq::moqx::admin
