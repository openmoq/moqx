/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "logging/access/AccessLogVars.h"

#include <array>
#include <ctime>
#include <iterator>

#include <fmt/format.h>

#include "SafeTrackName.h"

namespace openmoq::moqx::logging {

namespace {

constexpr std::array<std::string_view, 22> kEventNames = {
    "SETUP",
    "GOAWAY",
    "SESSION_END",
    "SUBSCRIBE",
    "PUBLISH",
    "PUBLISH_NAMESPACE",
    "SUBSCRIBE_NAMESPACE",
    "SUBSCRIBE_TRACKS",
    "PUBLISH_DONE",
    "PUBLISH_NAMESPACE_DONE",
    "UNSUBSCRIBE",
    "PUBLISH_NAMESPACE_CANCEL",
    "UNSUBSCRIBE_NAMESPACE",
    "UNSUBSCRIBE_TRACKS",
    "FETCH",
    "TRACK_STATUS",
    "REQUEST_UPDATE",
    "NAMESPACE",
    "NAMESPACE_DONE",
    "PUBLISH_BLOCKED",
    "SUB_STATS",
    "LOG_DROPPED",
};
static_assert(kEventNames.size() == size_t(AccessLogEvent::LOG_DROPPED) + 1);

constexpr std::array<std::string_view, 4> kClassNames =
    {"session", "request", "periodic", "internal"};

void appendUint(uint64_t value, std::string& out) {
  fmt::format_to(std::back_inserter(out), "{}", value);
}

void appendLocation(const moxygen::AbsoluteLocation& loc, std::string& out) {
  fmt::format_to(std::back_inserter(out), "{}:{}", loc.group, loc.object);
}

bool appendText(std::string_view text, std::string& out) {
  if (text.empty()) {
    return false;
  }
  appendEscaped(text, out);
  return true;
}

bool appendOptional(const std::optional<uint64_t>& value, std::string& out) {
  if (!value) {
    return false;
  }
  appendUint(*value, out);
  return true;
}

bool appendOptional(const std::optional<moxygen::AbsoluteLocation>& loc, std::string& out) {
  if (!loc) {
    return false;
  }
  appendLocation(*loc, out);
  return true;
}

std::string_view statusName(AccessLogStatus status) {
  switch (status) {
  case AccessLogStatus::Ok:
    return "ok";
  case AccessLogStatus::Error:
    return "error";
  case AccessLogStatus::Aborted:
    return "aborted";
  case AccessLogStatus::Rejected:
    return "rejected";
  }
  return "?";
}

std::pair<int64_t, int64_t> splitMillis(std::chrono::system_clock::time_point time) {
  auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(time.time_since_epoch()).count();
  auto secs = ms / 1000;
  auto millis = ms % 1000;
  if (millis < 0) {
    secs -= 1;
    millis += 1000;
  }
  return {secs, millis};
}

using Rec = AccessLogRecord;
using Ctx = AccessLogRenderContext;

// Records without a session (LOG_DROPPED) print "-" for session fields.
#define MOQX_SESSION_TEXT(member)                                                                  \
  +[](const Rec& r, const Ctx&, std::string& out) -> bool {                                        \
    return r.session && appendText(r.session->member, out);                                        \
  }
#define MOQX_RECORD_TEXT(member)                                                                   \
  +[](const Rec& r, const Ctx&, std::string& out) -> bool { return appendText(r.member, out); }
#define MOQX_RECORD_OPT(member)                                                                    \
  +[](const Rec& r, const Ctx&, std::string& out) -> bool { return appendOptional(r.member, out); }
#define MOQX_RELAY_TEXT(member)                                                                    \
  +[](const Rec&, const Ctx& c, std::string& out) -> bool {                                        \
    return appendText(c.relayInfo.member, out);                                                    \
  }

bool renderTime(const Rec& r, const Ctx&, std::string& out) {
  auto [secs, millis] = splitMillis(r.time);
  std::time_t t = secs;
  std::tm tm{};
  gmtime_r(&t, &tm);
  fmt::format_to(
      std::back_inserter(out),
      "{:04}-{:02}-{:02}T{:02}:{:02}:{:02}.{:03}Z",
      tm.tm_year + 1900,
      tm.tm_mon + 1,
      tm.tm_mday,
      tm.tm_hour,
      tm.tm_min,
      tm.tm_sec,
      millis
  );
  return true;
}

bool renderMsec(const Rec& r, const Ctx&, std::string& out) {
  auto [secs, millis] = splitMillis(r.time);
  fmt::format_to(std::back_inserter(out), "{}.{:03}", secs, millis);
  return true;
}

bool renderOpened(const Rec& r, const Ctx&, std::string& out) {
  if (!r.session) {
    return false;
  }
  out.append(r.session->openedBy == AccessLogOpenedBy::Peer ? "accepted" : "connected");
  return true;
}

bool renderDir(const Rec& r, const Ctx&, std::string& out) {
  if (!r.dir) {
    return false;
  }
  out.append(*r.dir == AccessLogDir::Rx ? "rx" : "tx");
  return true;
}

// An empty name is a real value, so it renders as "" to keep its column. A '"' cannot
// appear otherwise: safeName and appendEscaped both encode it.
void appendName(std::string_view name, std::string& out) {
  if (name.empty()) {
    out.append("\"\"");
  } else {
    out.append(name);
  }
}

bool renderNamespace(const Rec& r, const Ctx&, std::string& out) {
  if (!r.ftn) {
    return false;
  }
  appendName(safeName(r.ftn->trackNamespace), out);
  return true;
}

bool renderTrack(const Rec& r, const Ctx&, std::string& out) {
  if (!r.ftn || r.namespaceOnly) {
    return false;
  }
  appendName(safeName(r.ftn->trackName), out);
  return true;
}

bool renderNsPrefix(const Rec& r, const Ctx& c, std::string& out) {
  return r.namespaceOnly && renderNamespace(r, c, out);
}

bool renderFtn(const Rec& r, const Ctx& c, std::string& out) {
  if (r.namespaceOnly) {
    return renderNamespace(r, c, out);
  }
  if (!r.ftn) {
    return false;
  }
  out.append(safeName(*r.ftn));
  return true;
}

bool renderFlag(bool value, std::string& out) {
  out.push_back(value ? '1' : '0');
  return true;
}

constexpr uint8_t kSession = accessLogClassBit(AccessLogClass::Session);
constexpr uint8_t kRequest = accessLogClassBit(AccessLogClass::Request);
constexpr uint8_t kPeriodic = accessLogClassBit(AccessLogClass::Periodic);
constexpr uint8_t kInternal = accessLogClassBit(AccessLogClass::Internal);
constexpr uint8_t kAll = kAllAccessLogClasses;

const std::
    array
        kVars =
            std::
                to_array<AccessLogVar>(
                    {
                        // Common
                        {"time", kAll, renderTime},
                        {"msec", kAll, renderMsec},
                        {"event",
                         kAll,
                         +[](const Rec& r, const Ctx&, std::string& out) -> bool {
                           out.append(accessLogEventName(r.event));
                           return true;
                         }},
                        {"class",
                         kAll,
                         +[](const Rec& r, const Ctx&, std::string& out) -> bool {
                           out.append(accessLogClassName(accessLogClass(r.event)));
                           return true;
                         }},
                        {"schema",
                         kAll,
                         +[](const Rec&, const Ctx&, std::string& out) -> bool {
                           out.append(kAccessLogSchema);
                           return true;
                         }},
                        {"seq",
                         kAll,
                         +[](const Rec&, const Ctx& c, std::string& out) -> bool {
                           appendUint(c.seq, out);
                           return true;
                         }},
                        {"relay_id", kAll, MOQX_RELAY_TEXT(relayId)},
                        {"boot_id", kAll, MOQX_RELAY_TEXT(bootId)},
                        {"host", kAll, MOQX_RELAY_TEXT(host)},
                        {"moqx_version", kAll, MOQX_RELAY_TEXT(moqxVersion)},
                        {"service", kAll, MOQX_SESSION_TEXT(service)},
                        {"session", kAll, MOQX_SESSION_TEXT(session)},
                        {"opened", kAll, renderOpened},
                        {"peer", kAll, MOQX_SESSION_TEXT(peer)},
                        {"local", kAll, MOQX_SESSION_TEXT(local)},
                        {"peer_trunc", kAll, MOQX_SESSION_TEXT(peerTrunc)},
                        {"peer_class", kAll, MOQX_SESSION_TEXT(peerClass)},
                        {"version",
                         kAll,
                         +[](const Rec& r, const Ctx&, std::string& out
                          ) -> bool { return r.session && appendOptional(r.session->version, out); }
                        },
                        {"authority", kAll, MOQX_SESSION_TEXT(authority)},
                        {"path", kAll, MOQX_SESSION_TEXT(path)},
                        {"path_raw", kAll, MOQX_SESSION_TEXT(pathRaw)},
                        {"upstream", kAll, MOQX_SESSION_TEXT(upstream)},

                        // Session and request
                        {"status",
                         kSession | kRequest,
                         +[](const Rec& r, const Ctx&, std::string& out) -> bool {
                           if (!r.status) {
                             return false;
                           }
                           out.append(statusName(*r.status));
                           return true;
                         }},
                        {"latency_ms", kSession | kRequest, MOQX_RECORD_OPT(latencyMs)},
                        {"error_code", kSession | kRequest, MOQX_RECORD_OPT(errorCode)},
                        {"error_class", kSession | kRequest, MOQX_RECORD_TEXT(errorClass)},
                        {"reason", kSession | kRequest, MOQX_RECORD_TEXT(reason)},
                        {"dir", kSession | kRequest | kPeriodic, renderDir},

                        // Session
                        {"goaway_uri", kSession, MOQX_RECORD_TEXT(goawayUri)},
                        {"duration_ms", kSession, MOQX_RECORD_OPT(durationMs)},
                        {"requests", kSession, MOQX_RECORD_OPT(requests)},

                        // Request
                        {"req", kRequest | kPeriodic, MOQX_RECORD_OPT(requestID)},
                        {"alias", kRequest | kPeriodic, MOQX_RECORD_OPT(alias)},
                        {"namespace", kRequest | kPeriodic, renderNamespace},
                        {"track", kRequest | kPeriodic, renderTrack},
                        {"ns_prefix", kRequest, renderNsPrefix},
                        {"ftn", kRequest | kPeriodic, renderFtn},
                        {"filter", kRequest, MOQX_RECORD_TEXT(filter)},
                        {"start", kRequest, MOQX_RECORD_OPT(start)},
                        {"end", kRequest, MOQX_RECORD_OPT(end)},
                        {"priority",
                         kRequest,
                         +[](const Rec& r, const Ctx&, std::string& out) -> bool {
                           if (!r.priority) {
                             return false;
                           }
                           appendUint(*r.priority, out);
                           return true;
                         }},
                        {"group_order",
                         kRequest,
                         +[](const Rec& r, const Ctx&, std::string& out) -> bool {
                           if (!r.groupOrder) {
                             return false;
                           }
                           out.append(
                               *r.groupOrder == moxygen::GroupOrder::OldestFirst ? "oldest"
                                                                                 : "newest"
                           );
                           return true;
                         }},
                        {"forward",
                         kRequest,
                         +[](const Rec& r, const Ctx&, std::string& out) -> bool {
                           return r.forward && renderFlag(*r.forward, out);
                         }},
                        {"ngr", kRequest, MOQX_RECORD_OPT(ngr)},
                        {"fetch_type", kRequest, MOQX_RECORD_TEXT(fetchType)},
                        {"largest", kRequest, MOQX_RECORD_OPT(largest)},
                        {"objects", kRequest, MOQX_RECORD_OPT(objects)},
                        {"bytes", kRequest, MOQX_RECORD_OPT(bytes)},
                        {"groups", kRequest, MOQX_RECORD_OPT(groups)},
                        {"age_ms", kRequest, MOQX_RECORD_OPT(ageMs)},
                        {"end_reason", kRequest, MOQX_RECORD_TEXT(endReason)},
                        {"ttfo_ms", kRequest, MOQX_RECORD_OPT(ttfoMs)},
                        {"fetch_ms", kRequest, MOQX_RECORD_OPT(fetchMs)},

                        // Internal
                        {"dropped", kInternal, MOQX_RECORD_OPT(dropped)},
                    }
                );

#undef MOQX_SESSION_TEXT
#undef MOQX_RECORD_TEXT
#undef MOQX_RECORD_OPT
#undef MOQX_RELAY_TEXT

} // namespace

bool accessLogMessage(AccessLogEvent event) {
  return event == AccessLogEvent::NAMESPACE || event == AccessLogEvent::NAMESPACE_DONE ||
         event == AccessLogEvent::PUBLISH_BLOCKED;
}

AccessLogClass accessLogClass(AccessLogEvent event) {
  switch (event) {
  case AccessLogEvent::SETUP:
  case AccessLogEvent::GOAWAY:
  case AccessLogEvent::SESSION_END:
    return AccessLogClass::Session;
  case AccessLogEvent::SUB_STATS:
    return AccessLogClass::Periodic;
  case AccessLogEvent::LOG_DROPPED:
    return AccessLogClass::Internal;
  default:
    return AccessLogClass::Request;
  }
}

std::string_view accessLogEventName(AccessLogEvent event) {
  return kEventNames[size_t(event)];
}

std::optional<AccessLogEvent> parseAccessLogEvent(std::string_view name) {
  for (size_t i = 0; i < kEventNames.size(); ++i) {
    if (kEventNames[i] == name) {
      return AccessLogEvent(i);
    }
  }
  return std::nullopt;
}

std::string_view accessLogClassName(AccessLogClass cls) {
  return kClassNames[size_t(cls)];
}

std::optional<AccessLogClass> parseAccessLogClass(std::string_view name) {
  for (size_t i = 0; i < kClassNames.size(); ++i) {
    if (kClassNames[i] == name) {
      return AccessLogClass(i);
    }
  }
  return std::nullopt;
}

std::span<const AccessLogVar> accessLogVars() {
  return kVars;
}

const AccessLogVar* findAccessLogVar(std::string_view name) {
  for (const auto& var : kVars) {
    if (var.name == name) {
      return &var;
    }
  }
  return nullptr;
}

void renderAccessLogVar(
    const AccessLogVar& var,
    const AccessLogRecord& record,
    const AccessLogRenderContext& ctx,
    std::string& out
) {
  if (!(var.classes & accessLogClassBit(accessLogClass(record.event))) ||
      !var.render(record, ctx, out)) {
    out.push_back('-');
  }
}

void appendEscaped(std::string_view text, std::string& out) {
  constexpr std::string_view kHex = "0123456789abcdef";
  for (unsigned char c : text) {
    if (c <= 0x20 || c >= 0x7f || c == '"' || c == '\\') {
      out.append({'\\', 'x', kHex[c >> 4], kHex[c & 0xf]});
    } else {
      out.push_back(char(c));
    }
  }
}

} // namespace openmoq::moqx::logging
