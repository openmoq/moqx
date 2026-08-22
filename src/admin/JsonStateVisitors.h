/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#pragma once

#include <chrono>
#include <string_view>

#include "MoqxRelayContext.h"
#include "admin/ChunkedJsonWriter.h"
#include "admin/JsonWriter.h"

namespace openmoq::moqx::admin {

// RelayStateVisitor that writes JSON as the walk proceeds, handing off a chunk
// whenever the writer crosses its threshold. Section begin/end callbacks map
// directly to JSON array/object open/close, so no state tracking or deferred
// finalization is needed here.
class JsonRelayStateVisitor : public RelayStateVisitor {
public:
  explicit JsonRelayStateVisitor(ChunkedJsonWriter& c) : c_(c), w_(c.json()) {}

  void onPeersBegin() override {
    w_.key("downstream_peers");
    w_.beginArray();
  }
  void
  onPeer(std::string_view address, std::string_view authority, std::string_view relayID) override {
    w_.beginObject();
    w_.field("address", address);
    w_.field("authority", authority);
    if (!relayID.empty()) {
      w_.field("relay_id", relayID);
    }
    w_.endObject();
    c_.maybeFlush();
  }
  void onPeersEnd() override { w_.endArray(); }

  void onSubscriptionsBegin() override {
    w_.key("subscriptions");
    w_.beginArray();
  }
  void onSubscription(const SubscriptionInfo& info) override {
    w_.beginObject();
    w_.key("namespace");
    w_.beginArray();
    for (const auto& t : info.ftn.trackNamespace.trackNamespace) {
      w_.strVal(t);
    }
    w_.endArray();
    w_.field("track_name", info.ftn.trackName);
    w_.field("is_publish", info.isPublish);
    w_.key("subscribers");
    w_.uintVal(static_cast<uint64_t>(info.subscribers));
    w_.field("forwarding_subscribers", info.forwardingSubscribers);
    w_.field("total_groups_received", info.totalGroupsReceived);
    w_.field("total_objects_received", info.totalObjectsReceived);
    w_.field("source_address", info.sourceAddress);
    if (info.largest) {
      w_.key("largest");
      w_.beginObject();
      w_.field("group", info.largest->group);
      w_.field("object", info.largest->object);
      w_.endObject();
    }
    w_.endObject();
    c_.maybeFlush();
  }
  void onSubscriptionsEnd() override { w_.endArray(); }

  void onNamespaceTreeBegin() override {
    w_.key("namespace_tree");
    // The root beginNamespaceNode call immediately follows and opens the object.
  }
  void beginNamespaceNode(
      std::string_view childKey,
      const moxygen::TrackNamespace& ns,
      size_t sessionCount,
      std::string_view publisherAddress,
      std::string_view peerID
  ) override {
    if (!childKey.empty()) {
      w_.key(childKey);
    }
    w_.beginObject();
    w_.key("full_namespace");
    w_.beginArray();
    for (const auto& t : ns.trackNamespace) {
      w_.strVal(t);
    }
    w_.endArray();
    w_.key("namespace_subscribers");
    w_.uintVal(static_cast<uint64_t>(sessionCount));
    if (!publisherAddress.empty()) {
      w_.field("publisher", publisherAddress);
    }
    if (!peerID.empty()) {
      w_.field("peer_id", peerID);
    }
    w_.key("children");
    w_.beginObject();
  }
  void endNamespaceNode() override {
    w_.endObject(); // children
    w_.endObject(); // node
    c_.maybeFlush();
  }
  void onNamespaceTreeEnd() override {}

  void onCacheBegin(size_t totalBytes, MoqxCache::TimePoint now) override {
    now_ = now;
    w_.key("cache");
    w_.beginObject();
    w_.key("total_bytes");
    w_.uintVal(static_cast<uint64_t>(totalBytes));
    w_.key("tracks");
    w_.beginArray();
  }
  bool onCacheTrack(const MoqxCache::TrackStatsView& t) override {
    w_.beginObject();
    w_.key("namespace");
    w_.beginArray();
    for (const auto& s : t.name.trackNamespace.trackNamespace) {
      w_.strVal(s);
    }
    w_.endArray();
    w_.field("track_name", t.name.trackName);
    w_.field("end_of_track", t.endOfTrack);
    w_.key("last_write_ms_ago");
    if (t.lastWrite == MoqxCache::TimePoint::min()) {
      w_.nullVal();
    } else {
      auto msAgo =
          std::chrono::duration_cast<std::chrono::milliseconds>(now_ - t.lastWrite).count();
      w_.intVal(static_cast<int64_t>(msAgo));
    }
    w_.key("groups");
    w_.beginArray();
    for (const auto& g : t.groups) {
      w_.beginObject();
      w_.field("group_id", g.groupId);
      w_.key("objects");
      w_.uintVal(static_cast<uint64_t>(g.objects));
      w_.endObject();
    }
    w_.endArray();
    w_.endObject();
    return c_.maybeFlush();
  }
  void onCacheEnd() override {
    w_.endArray();
    w_.endObject();
  }

private:
  ChunkedJsonWriter& c_;
  JsonWriter& w_;
  MoqxCache::TimePoint now_{};
};

// Builds the top-level envelope around the per-service walks. Shares one
// ChunkedJsonWriter with the relay visitor so comma state stays consistent
// across the two layers and a chunk can be cut anywhere.
class JsonRelayContextVisitor : public RelayContextVisitor {
public:
  explicit JsonRelayContextVisitor(ChunkedJsonWriter& c) : c_(c), w_(c.json()), relayVisitor_(c) {}

  void onRelayBegin(std::string_view relayID, int64_t activeSessions) override {
    w_.beginObject();
    w_.field("relay_id", relayID);
    w_.field("active_sessions", activeSessions);
    w_.key("services");
    w_.beginObject();
  }

  RelayStateVisitor& onServiceBegin(std::string_view name) override {
    w_.key(name);
    w_.beginObject();
    return relayVisitor_;
  }

  void onServiceUpstream(std::string_view url, std::string_view state) override {
    w_.key("upstream");
    w_.beginObject();
    w_.field("url", url);
    w_.field("state", state);
    w_.endObject();
  }

  void onServiceEnd() override {
    w_.endObject();
    c_.maybeFlush();
  }

  void onRelayEnd() override {
    w_.endObject(); // services
    w_.endObject(); // relay
    c_.raw("\n");
  }

private:
  ChunkedJsonWriter& c_;
  JsonWriter& w_;
  JsonRelayStateVisitor relayVisitor_;
};

} // namespace openmoq::moqx::admin
