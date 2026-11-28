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
using Value = AccessLogValue;

Value textValue(std::string_view text) {
  return text.empty() ? Value{} : Value{text};
}

template <typename T> Value optionalValue(const std::optional<T>& value) {
  return value ? Value{*value} : Value{};
}

// Records without a session (LOG_DROPPED) print "-" for session fields.
#define MOQX_SESSION_TEXT(member)                                                                  \
  +[](const Rec& r, const Ctx&) -> Value {                                                         \
    return r.session ? textValue(r.session->member) : Value{};                                     \
  }
#define MOQX_RECORD_TEXT(member)                                                                   \
  +[](const Rec& r, const Ctx&) -> Value { return textValue(r.member); }
#define MOQX_RECORD_OPT(member)                                                                    \
  +[](const Rec& r, const Ctx&) -> Value { return optionalValue(r.member); }
#define MOQX_RELAY_TEXT(member)                                                                    \
  +[](const Rec&, const Ctx& c) -> Value { return textValue(c.relayInfo.member); }

Value valueOpened(const Rec& r, const Ctx&) {
  if (!r.session) {
    return {};
  }
  return std::string_view(
      r.session->openedBy == AccessLogOpenedBy::Peer ? "accepted" : "connected"
  );
}

Value valueDir(const Rec& r, const Ctx&) {
  if (!r.dir) {
    return {};
  }
  return std::string_view(*r.dir == AccessLogDir::Rx ? "rx" : "tx");
}

Value valueNamespace(const Rec& r, const Ctx&) {
  if (!r.ftn) {
    return {};
  }
  return safeName(r.ftn->trackNamespace);
}

Value valueTrack(const Rec& r, const Ctx&) {
  if (!r.ftn || r.namespaceOnly) {
    return {};
  }
  return safeName(r.ftn->trackName);
}

Value valueNsPrefix(const Rec& r, const Ctx& c) {
  return r.namespaceOnly ? valueNamespace(r, c) : Value{};
}

Value valueFtn(const Rec& r, const Ctx& c) {
  if (r.namespaceOnly) {
    return valueNamespace(r, c);
  }
  if (!r.ftn) {
    return {};
  }
  return safeName(*r.ftn);
}

Value valueGroupOrder(const Rec& r, const Ctx&) {
  if (!r.groupOrder) {
    return {};
  }
  return std::string_view(*r.groupOrder == moxygen::GroupOrder::OldestFirst ? "oldest" : "newest");
}

void renderTimeOf(std::chrono::system_clock::time_point time, std::string& out) {
  auto [secs, millis] = splitMillis(time);
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

void appendText(std::string_view text, AccessLogTextForm form, std::string& out) {
  switch (form) {
  case AccessLogTextForm::Escaped:
    appendEscaped(text, out);
    return;
  case AccessLogTextForm::Name:
    appendName(text, out);
    return;
  default:
    out.append(text);
    return;
  }
}

// Appends the value and returns true, or returns false for "-".
bool renderValue(const Value& value, AccessLogTextForm form, std::string& out) {
  return std::visit(
      [&](const auto& v) -> bool {
        using T = std::decay_t<decltype(v)>;
        if constexpr (std::is_same_v<T, std::monostate>) {
          return false;
        } else if constexpr (std::is_same_v<T, uint64_t>) {
          appendUint(v, out);
        } else if constexpr (std::is_same_v<T, bool>) {
          out.push_back(v ? '1' : '0');
        } else if constexpr (std::is_same_v<T, moxygen::AbsoluteLocation>) {
          appendLocation(v, out);
        } else if constexpr (std::is_same_v<T, std::chrono::system_clock::time_point>) {
          if (form == AccessLogTextForm::EpochMsec) {
            auto [secs, millis] = splitMillis(v);
            fmt::format_to(std::back_inserter(out), "{}.{:03}", secs, millis);
          } else {
            renderTimeOf(v, out);
          }
        } else {
          appendText(v, form, out);
        }
        return true;
      },
      value
  );
}

constexpr uint8_t kSession = accessLogClassBit(AccessLogClass::Session);
constexpr uint8_t kRequest = accessLogClassBit(AccessLogClass::Request);
constexpr uint8_t kPeriodic = accessLogClassBit(AccessLogClass::Periodic);
constexpr uint8_t kInternal = accessLogClassBit(AccessLogClass::Internal);
constexpr uint8_t kAll = kAllAccessLogClasses;

constexpr auto kPlain = AccessLogTextForm::Plain;
constexpr auto kEscaped = AccessLogTextForm::Escaped;
constexpr auto kName = AccessLogTextForm::Name;
constexpr auto kIsoTime = AccessLogTextForm::IsoTime;
constexpr auto kEpochMsec = AccessLogTextForm::EpochMsec;

const auto kVars = std::to_array<AccessLogVar>({
    // Common
    {"time", kAll, kIsoTime, +[](const Rec& r, const Ctx&) -> Value { return r.time; }},
    {"msec", kAll, kEpochMsec, +[](const Rec& r, const Ctx&) -> Value { return r.time; }},
    {"event",
     kAll,
     kPlain,
     +[](const Rec& r, const Ctx&) -> Value { return accessLogEventName(r.event); }},
    {"class",
     kAll,
     kPlain,
     +[](const Rec& r, const Ctx&) -> Value { return accessLogClassName(accessLogClass(r.event)); }
    },
    {"schema", kAll, kPlain, +[](const Rec&, const Ctx&) -> Value { return kAccessLogSchema; }},
    {"seq", kAll, kPlain, +[](const Rec&, const Ctx& c) -> Value { return c.seq; }},
    {"relay_id", kAll, kEscaped, MOQX_RELAY_TEXT(relayId)},
    {"boot_id", kAll, kEscaped, MOQX_RELAY_TEXT(bootId)},
    {"host", kAll, kEscaped, MOQX_RELAY_TEXT(host)},
    {"moqx_version", kAll, kEscaped, MOQX_RELAY_TEXT(moqxVersion)},
    {"service", kAll, kEscaped, MOQX_SESSION_TEXT(service)},
    {"session", kAll, kEscaped, MOQX_SESSION_TEXT(session)},
    {"opened", kAll, kPlain, valueOpened},
    {"peer", kAll, kEscaped, MOQX_SESSION_TEXT(peer)},
    {"local", kAll, kEscaped, MOQX_SESSION_TEXT(local)},
    {"peer_trunc", kAll, kEscaped, MOQX_SESSION_TEXT(peerTrunc)},
    {"peer_class", kAll, kEscaped, MOQX_SESSION_TEXT(peerClass)},
    {"version",
     kAll,
     kPlain,
     +[](const Rec& r, const Ctx&) -> Value {
       return r.session ? optionalValue(r.session->version) : Value{};
     }},
    {"authority", kAll, kEscaped, MOQX_SESSION_TEXT(authority)},
    {"path", kAll, kEscaped, MOQX_SESSION_TEXT(path)},
    {"path_raw", kAll, kEscaped, MOQX_SESSION_TEXT(pathRaw)},
    {"upstream", kAll, kEscaped, MOQX_SESSION_TEXT(upstream)},

    // Session and request
    {"status",
     kSession | kRequest,
     kPlain,
     +[](const Rec& r, const Ctx&) -> Value {
       return r.status ? Value{statusName(*r.status)} : Value{};
     }},
    {"latency_ms", kSession | kRequest, kPlain, MOQX_RECORD_OPT(latencyMs)},
    {"error_code", kSession | kRequest, kPlain, MOQX_RECORD_OPT(errorCode)},
    {"error_class", kSession | kRequest, kEscaped, MOQX_RECORD_TEXT(errorClass)},
    {"reason", kSession | kRequest, kEscaped, MOQX_RECORD_TEXT(reason)},
    {"dir", kSession | kRequest | kPeriodic, kPlain, valueDir},

    // Session
    {"goaway_uri", kSession, kEscaped, MOQX_RECORD_TEXT(goawayUri)},
    {"duration_ms", kSession, kPlain, MOQX_RECORD_OPT(durationMs)},
    {"requests", kSession, kPlain, MOQX_RECORD_OPT(requests)},

    // Request
    {"req", kRequest | kPeriodic, kPlain, MOQX_RECORD_OPT(requestID)},
    {"alias", kRequest | kPeriodic, kPlain, MOQX_RECORD_OPT(alias)},
    {"namespace", kRequest | kPeriodic, kName, valueNamespace},
    {"track", kRequest | kPeriodic, kName, valueTrack},
    {"ns_prefix", kRequest, kName, valueNsPrefix},
    {"ftn", kRequest | kPeriodic, kName, valueFtn},
    {"filter", kRequest, kEscaped, MOQX_RECORD_TEXT(filter)},
    {"start", kRequest, kPlain, MOQX_RECORD_OPT(start)},
    {"end", kRequest, kPlain, MOQX_RECORD_OPT(end)},
    {"priority",
     kRequest,
     kPlain,
     +[](const Rec& r, const Ctx&) -> Value {
       return r.priority ? Value{uint64_t(*r.priority)} : Value{};
     }},
    {"group_order", kRequest, kPlain, valueGroupOrder},
    {"forward",
     kRequest,
     kPlain,
     +[](const Rec& r, const Ctx&) -> Value { return r.forward ? Value{*r.forward} : Value{}; }},
    {"ngr", kRequest, kPlain, MOQX_RECORD_OPT(ngr)},
    {"fetch_type", kRequest, kEscaped, MOQX_RECORD_TEXT(fetchType)},
    {"largest", kRequest, kPlain, MOQX_RECORD_OPT(largest)},
    {"objects", kRequest | kPeriodic, kPlain, MOQX_RECORD_OPT(objects)},
    {"bytes", kRequest | kPeriodic, kPlain, MOQX_RECORD_OPT(bytes)},
    {"groups", kRequest | kPeriodic, kPlain, MOQX_RECORD_OPT(groups)},
    {"subgroups", kRequest | kPeriodic, kPlain, MOQX_RECORD_OPT(subgroups)},
    {"datagrams", kRequest | kPeriodic, kPlain, MOQX_RECORD_OPT(datagrams)},
    {"status_objects", kRequest | kPeriodic, kPlain, MOQX_RECORD_OPT(statusObjects)},
    {"resets", kRequest | kPeriodic, kPlain, MOQX_RECORD_OPT(resets)},
    {"last_reset_code", kRequest | kPeriodic, kPlain, MOQX_RECORD_OPT(lastResetCode)},
    {"open_subgroups", kRequest | kPeriodic, kPlain, MOQX_RECORD_OPT(openSubgroups)},
    {"bytes_queued", kRequest | kPeriodic, kPlain, MOQX_RECORD_OPT(bytesQueued)},
    {"payload_bytes_queued", kRequest | kPeriodic, kPlain, MOQX_RECORD_OPT(payloadBytesQueued)},
    {"bytes_written", kRequest | kPeriodic, kPlain, MOQX_RECORD_OPT(bytesWritten)},
    {"bytes_acked", kRequest | kPeriodic, kPlain, MOQX_RECORD_OPT(bytesAcked)},
    {"bytes_dropped", kRequest | kPeriodic, kPlain, MOQX_RECORD_OPT(bytesDropped)},
    {"bytes_cancelled", kRequest | kPeriodic, kPlain, MOQX_RECORD_OPT(bytesCancelled)},
    {"objects_written", kRequest | kPeriodic, kPlain, MOQX_RECORD_OPT(objectsWritten)},
    {"objects_acked", kRequest | kPeriodic, kPlain, MOQX_RECORD_OPT(objectsAcked)},
    {"ack_us_avg", kRequest | kPeriodic, kPlain, MOQX_RECORD_OPT(ackUsAvg)},
    {"ack_us_max", kRequest | kPeriodic, kPlain, MOQX_RECORD_OPT(ackUsMax)},
    {"srtt_us", kSession | kPeriodic, kPlain, MOQX_RECORD_OPT(srttUs)},
    {"min_rtt_us", kSession | kPeriodic, kPlain, MOQX_RECORD_OPT(minRttUs)},
    {"cwnd", kSession | kPeriodic, kPlain, MOQX_RECORD_OPT(cwnd)},
    {"inflight", kSession | kPeriodic, kPlain, MOQX_RECORD_OPT(inflight)},
    {"pkts_sent", kSession | kPeriodic, kPlain, MOQX_RECORD_OPT(pktsSent)},
    {"pkts_lost", kSession | kPeriodic, kPlain, MOQX_RECORD_OPT(pktsLost)},
    {"bytes_retx", kSession | kPeriodic, kPlain, MOQX_RECORD_OPT(bytesRetx)},
    {"age_ms", kRequest | kPeriodic, kPlain, MOQX_RECORD_OPT(ageMs)},
    {"end_reason", kRequest, kEscaped, MOQX_RECORD_TEXT(endReason)},
    {"ttfo_ms", kRequest | kPeriodic, kPlain, MOQX_RECORD_OPT(ttfoMs)},
    {"fetch_ms", kRequest, kPlain, MOQX_RECORD_OPT(fetchMs)},
    {"namespace_msgs", kRequest, kPlain, MOQX_RECORD_OPT(namespaceMsgs)},
    {"namespace_done_msgs", kRequest, kPlain, MOQX_RECORD_OPT(namespaceDoneMsgs)},
    {"publish_blocked_msgs", kRequest, kPlain, MOQX_RECORD_OPT(publishBlockedMsgs)},

    // Periodic
    {"interval_start",
     kPeriodic,
     kIsoTime,
     +[](const Rec& r,
         const Ctx&) -> Value { return r.intervalStart ? Value{*r.intervalStart} : Value{}; }},
    {"interval_ms", kPeriodic, kPlain, MOQX_RECORD_OPT(intervalMs)},
    {"d_objects", kPeriodic, kPlain, MOQX_RECORD_OPT(dObjects)},
    {"d_bytes", kPeriodic, kPlain, MOQX_RECORD_OPT(dBytes)},
    {"d_groups", kPeriodic, kPlain, MOQX_RECORD_OPT(dGroups)},
    {"d_subgroups", kPeriodic, kPlain, MOQX_RECORD_OPT(dSubgroups)},
    {"d_datagrams", kPeriodic, kPlain, MOQX_RECORD_OPT(dDatagrams)},
    {"d_bytes_queued", kPeriodic, kPlain, MOQX_RECORD_OPT(dBytesQueued)},
    {"d_payload_bytes_queued", kPeriodic, kPlain, MOQX_RECORD_OPT(dPayloadBytesQueued)},
    {"d_bytes_written", kPeriodic, kPlain, MOQX_RECORD_OPT(dBytesWritten)},
    {"d_bytes_acked", kPeriodic, kPlain, MOQX_RECORD_OPT(dBytesAcked)},
    {"d_objects_written", kPeriodic, kPlain, MOQX_RECORD_OPT(dObjectsWritten)},
    {"d_objects_acked", kPeriodic, kPlain, MOQX_RECORD_OPT(dObjectsAcked)},
    {"d_ack_us_avg", kPeriodic, kPlain, MOQX_RECORD_OPT(dAckUsAvg)},
    {"d_status_objects", kPeriodic, kPlain, MOQX_RECORD_OPT(dStatusObjects)},
    {"d_resets", kPeriodic, kPlain, MOQX_RECORD_OPT(dResets)},

    // Internal
    {"dropped", kInternal, kPlain, MOQX_RECORD_OPT(dropped)},
});

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

AccessLogValue accessLogVarValue(
    const AccessLogVar& var,
    const AccessLogRecord& record,
    const AccessLogRenderContext& ctx
) {
  if (!(var.classes & accessLogClassBit(accessLogClass(record.event)))) {
    return {};
  }
  return var.value(record, ctx);
}

void renderAccessLogVar(
    const AccessLogVar& var,
    const AccessLogRecord& record,
    const AccessLogRenderContext& ctx,
    std::string& out
) {
  if (!renderValue(accessLogVarValue(var, record, ctx), var.form, out)) {
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
