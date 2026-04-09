/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "admin/StateHandler.h"

#include <chrono>
#include <folly/CancellationToken.h>
#include <folly/coro/Task.h>
#include <folly/experimental/coro/WithCancellation.h>
#include <folly/io/IOBuf.h>
#include <folly/io/IOBufQueue.h>
#include <folly/io/async/EventBaseManager.h>
#include <folly/logging/xlog.h>
#include <proxygen/httpserver/ResponseBuilder.h>
#include <proxygen/lib/http/HTTPMessage.h>

#include "MoqxRelayContext.h"
#include "admin/AdminServer.h"
#include "admin/JsonWriter.h"

namespace openmoq::moqx::admin {

namespace {

// RelayStateVisitor that writes JSON directly via a shared JsonWriter.
// Section begin/end callbacks map directly to JSON array/object open/close,
// so no state tracking or deferred finalization is needed here.
class JsonRelayStateVisitor : public RelayStateVisitor {
  JsonWriter& w_;

public:
  explicit JsonRelayStateVisitor(JsonWriter& w) : w_(w) {}

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
  }
  void onSubscriptionsEnd() override { w_.endArray(); }

  void onNamespaceTreeBegin() override {
    w_.key("namespace_tree");
    // The root beginNamespaceNode call immediately follows and opens the object.
  }
  void beginNamespaceNode(
      std::string_view childKey,
      const moxygen::TrackNamespace& ns,
      size_t sessionCount
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
    w_.key("children");
    w_.beginObject();
  }
  void endNamespaceNode() override {
    w_.endObject(); // children
    w_.endObject(); // node
  }
  void onNamespaceTreeEnd() override {}

  void onCacheStats(
      size_t totalBytes,
      const std::vector<moxygen::MoQCache::TrackStats>& tracks,
      moxygen::MoQCache::TimePoint now
  ) override {
    w_.key("cache");
    w_.beginObject();
    w_.key("total_bytes");
    w_.uintVal(static_cast<uint64_t>(totalBytes));
    w_.key("tracks");
    w_.beginArray();
    for (const auto& t : tracks) {
      w_.beginObject();
      w_.key("namespace");
      w_.beginArray();
      for (const auto& s : t.name.trackNamespace.trackNamespace) {
        w_.strVal(s);
      }
      w_.endArray();
      w_.field("track_name", t.name.trackName);
      w_.field("end_of_track", t.endOfTrack);
      auto msAgo = std::chrono::duration_cast<std::chrono::milliseconds>(now - t.lastWrite).count();
      w_.field("last_write_ms_ago", static_cast<int64_t>(msAgo));
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
    }
    w_.endArray();
    w_.endObject();
  }
};

// RelayContextVisitor that builds the top-level JSON envelope.
// A single JsonWriter is shared with JsonRelayStateVisitor so comma state
// remains consistent across the two visitor layers.
class JsonRelayContextVisitor : public RelayContextVisitor {
  folly::IOBufQueue queue_{folly::IOBufQueue::cacheChainLength()};
  folly::io::QueueAppender app_{&queue_, 4096};
  JsonWriter w_{app_};
  JsonRelayStateVisitor relayVisitor_{w_};

public:
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

  void onServiceEnd() override { w_.endObject(); }

  void onRelayEnd() override {
    w_.endObject(); // services
    w_.endObject(); // relay
    static constexpr uint8_t kNewline = '\n';
    app_.write(kNewline);
  }

  std::unique_ptr<folly::IOBuf> move() { return queue_.move(); }
};

// Runs on the relay worker EVB. Dumps state and returns the serialized body.
folly::coro::Task<std::unique_ptr<folly::IOBuf>>
buildStateBody(std::shared_ptr<MoqxRelayContext> ctx) {
  JsonRelayContextVisitor visitor;
  ctx->dumpState(visitor);
  co_return visitor.move();
}

} // namespace

void registerStateRoute(AdminServer& adminServer, std::shared_ptr<MoqxRelayContext> context) {
  adminServer.addRoute(
      "GET",
      "/state",
      [context = std::move(context
       )](auto /*req*/, auto /*body*/, auto* downstream, folly::CancellationToken cancelToken) {
        auto* workerEvb = context->workerEvb();
        if (!workerEvb) {
          proxygen::ResponseBuilder(downstream)
              .status(503, proxygen::HTTPMessage::getDefaultReason(503))
              .body(folly::IOBuf::copyBuffer("relay not ready\n"))
              .sendWithEOM();
          return;
        }

        // Outer coroutine stays on the admin EVB so sendWithEOM is thread-safe.
        // buildStateBody switches to workerEvb and returns the IOBuf; after the
        // co_await completes execution resumes here on the admin EVB.
        folly::coro::co_withCancellation(
            cancelToken,
            folly::coro::co_withExecutor(
                folly::EventBaseManager::get()->getEventBase(),
                [](auto ctx, auto* ds, auto* wEvb, auto token) -> folly::coro::Task<void> {
                  if (token.isCancellationRequested()) {
                    co_return;
                  }
                  std::unique_ptr<folly::IOBuf> body;
                  try {
                    body = co_await folly::coro::co_withExecutor(wEvb, buildStateBody(ctx));
                  } catch (const std::exception& e) {
                    XLOG(ERR) << "StateHandler: dumpState threw: " << e.what();
                    if (!token.isCancellationRequested()) {
                      proxygen::ResponseBuilder(ds)
                          .status(500, proxygen::HTTPMessage::getDefaultReason(500))
                          .body(folly::IOBuf::copyBuffer("internal error\n"))
                          .sendWithEOM();
                    }
                    co_return;
                  }
                  if (token.isCancellationRequested()) {
                    co_return;
                  }
                  proxygen::ResponseBuilder(ds)
                      .status(200, proxygen::HTTPMessage::getDefaultReason(200))
                      .header("Content-Type", "application/json")
                      .body(std::move(body))
                      .sendWithEOM();
                }(context, downstream, workerEvb, cancelToken)
            )
        )
            .start();
      }
  );
}

} // namespace openmoq::moqx::admin
