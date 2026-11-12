/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include <moxygen/MoQTypes.h>

namespace openmoq::moqx::logging {

enum class AccessLogClass : uint8_t { Session, Request, Periodic, Internal };

// Event names are unique across classes, so an event identifies its class.
enum class AccessLogEvent : uint8_t {
  // session
  SETUP,
  GOAWAY,
  SESSION_END,
  // request: long-lived transaction lines
  SUBSCRIBE,
  PUBLISH,
  PUBLISH_NAMESPACE,
  SUBSCRIBE_NAMESPACE,
  SUBSCRIBE_TRACKS,
  // request: long-lived end lines
  PUBLISH_DONE,
  PUBLISH_NAMESPACE_DONE,
  UNSUBSCRIBE,
  PUBLISH_NAMESPACE_CANCEL,
  UNSUBSCRIBE_NAMESPACE,
  UNSUBSCRIBE_TRACKS,
  // request: single lines
  FETCH,
  TRACK_STATUS,
  REQUEST_UPDATE,
  // request: messages within an established SUBSCRIBE_NAMESPACE or SUBSCRIBE_TRACKS
  NAMESPACE,
  NAMESPACE_DONE,
  PUBLISH_BLOCKED,
  // periodic
  SUB_STATS,
  // internal
  LOG_DROPPED,
};

AccessLogClass accessLogClass(AccessLogEvent event);
// NAMESPACE, NAMESPACE_DONE and PUBLISH_BLOCKED. A sink's events list takes them by name or
// by kAccessLogMessages, and the request class does not include them.
bool accessLogMessage(AccessLogEvent event);
inline constexpr std::string_view kAccessLogMessages = "messages";
std::string_view accessLogEventName(AccessLogEvent event);
std::optional<AccessLogEvent> parseAccessLogEvent(std::string_view name);
std::string_view accessLogClassName(AccessLogClass cls);
std::optional<AccessLogClass> parseAccessLogClass(std::string_view name);

// Who sent a request or GOAWAY, or closed the session. rx: the peer.
// tx: the relay.
enum class AccessLogDir : uint8_t { Rx, Tx };

// Which side opened the session: the peer (accepted) or the relay (connected).
enum class AccessLogOpenedBy : uint8_t { Peer, Relay };

enum class AccessLogStatus : uint8_t { Ok, Error, Aborted, Rejected };

// The relay's identity. Every record carries it.
struct AccessLogRelayInfo {
  std::string relayId;
  std::string bootId;
  std::string region;
  std::string pop;
  std::string cluster;
  std::string host;
  // origin, mid or edge
  std::string tier;
  std::string moqxVersion;
};

// Fields that are the same for every record of one session. Filled once at SETUP and
// shared by that session's records.
struct AccessLogSessionInfo {
  std::string session;
  AccessLogOpenedBy openedBy{AccessLogOpenedBy::Peer};
  std::string service;
  std::string peer;
  std::string local;
  // The peer address truncated to /24 (IPv4) or /48 (IPv6).
  std::string peerTrunc;
  // participant or relay-peer
  std::string peerClass;
  std::optional<uint64_t> version;
  std::string authority;
  // The path without its query, which can carry tokens. pathRaw keeps the query.
  std::string path;
  std::string pathRaw;
  // The upstream URL, on sessions the relay connected.
  std::string upstream;
};

// Planned fields, added with the code that fills them:
//   cid                QUIC connection ID, to join with qlog
//   transport          quic or webtransport
//   peer_hash          HMAC of the peer address
//   moqt_impl          the peer's MOQT_IMPLEMENTATION setup parameter
//   user_agent         WebTransport User-Agent header; "-" on raw QUIC
//   auth_subject       token subject; auth_subject_hash is its HMAC
//   auth_key_id        ID of the key that verified the token
//   qlog / mlog        1 if a trace exists for the session
//   bytes_rx/tx        MoQ payload bytes; wire_bytes_rx/tx and retx_bytes at the QUIC level
//   up_req/ms/status   the upstream request a downstream request waited on
//   hop_id / hop_path  relay-hop identity and path
//   hdr_bytes          object header bytes on an end line
//   hit_*, miss_*, coalesced_* objects and bytes, upstream_fetches  FETCH cache outcome

// One typed record, built on the session thread and rendered on the writer. Unset
// fields render as "-".
struct AccessLogRecord {
  AccessLogEvent event{AccessLogEvent::SETUP};
  std::chrono::system_clock::time_point time;
  std::shared_ptr<const AccessLogSessionInfo> session;

  // Request identity. A namespace request has an empty track name in ftn.
  std::optional<AccessLogDir> dir;
  std::optional<uint64_t> requestID;
  std::optional<uint64_t> alias;
  std::shared_ptr<const moxygen::FullTrackName> ftn;
  bool namespaceOnly{false};

  // Outcome. errorCode is a REQUEST_ERROR, PUBLISH_DONE, close or stream reset code.
  // endReason says which.
  std::optional<AccessLogStatus> status;
  std::optional<uint64_t> latencyMs;
  std::optional<uint64_t> errorCode;
  std::string errorClass;
  // The peer's reason phrase.
  std::string reason;
  // How a request ended: fin, request_error, reset, cancelled, session_closed, ...
  std::string endReason;

  // Request parameters
  std::string filter;
  std::optional<moxygen::AbsoluteLocation> start;
  std::optional<moxygen::AbsoluteLocation> end;
  std::optional<uint8_t> priority;
  std::optional<moxygen::GroupOrder> groupOrder;
  std::optional<bool> forward;
  // NEW_GROUP_REQUEST
  std::optional<uint64_t> ngr;
  std::string fetchType;
  std::optional<moxygen::AbsoluteLocation> largest;

  // Totals and timings
  std::optional<uint64_t> objects;
  std::optional<uint64_t> bytes;
  std::optional<uint64_t> groups;
  std::optional<uint64_t> ageMs;
  std::optional<uint64_t> ttfoMs;
  std::optional<uint64_t> fetchMs;

  // Session class
  std::string goawayUri;
  std::optional<uint64_t> durationMs;
  std::optional<uint64_t> requests;

  // LOG_DROPPED
  std::optional<uint64_t> dropped;
};

} // namespace openmoq::moqx::logging
