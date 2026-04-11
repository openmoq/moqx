/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * Originally from github.com/facebookexperimental/moxygen.
 * See deps/moxygen/LICENSE for the original license terms.
 *
 * Copyright (c) OpenMOQ contributors.
 */

#include "MoqxRelay.h"
#include <moxygen/MoQFilters.h>
#include <moxygen/MoQTrackProperties.h>

namespace {
constexpr uint8_t kDefaultUpstreamPriority = 128;
constexpr std::chrono::seconds kUpstreamConnectWaitTimeout(5);
} // namespace

using namespace moxygen;

namespace openmoq::moqx {

// Bridges NAMESPACE/NAMESPACE_DONE messages from a peer relay directly into
// MoqxRelay::doPublishNamespace/doPublishNamespaceDone — no coroutine overhead,
// no handle map needed.
class MoqxRelayNamespaceHandle : public Publisher::NamespacePublishHandle {
public:
  MoqxRelayNamespaceHandle(std::weak_ptr<MoqxRelay> relay, std::shared_ptr<MoQSession> session)
      : relay_(std::move(relay)), session_(std::move(session)) {}

  void namespaceMsg(const TrackNamespace& suffix) override {
    auto relay = relay_.lock();
    if (!relay || !session_) {
      return;
    }
    PublishNamespace pubNs;
    pubNs.trackNamespace = suffix;
    relay->doPublishNamespace(std::move(pubNs), session_, nullptr);
  }

  void namespaceDoneMsg(const TrackNamespace& suffix) override {
    auto relay = relay_.lock();
    if (!relay || !session_) {
      return;
    }
    relay->doPublishNamespaceDone(suffix, session_);
  }

private:
  std::weak_ptr<MoqxRelay> relay_;
  std::shared_ptr<MoQSession> session_;
};

std::shared_ptr<Publisher::NamespacePublishHandle>
makeNamespaceBridgeHandle(std::weak_ptr<MoqxRelay> relay, std::shared_ptr<MoQSession> session) {
  return std::make_shared<MoqxRelayNamespaceHandle>(std::move(relay), std::move(session));
}

folly::coro::Task<void> MoqxRelay::onUpstreamConnect(std::shared_ptr<MoQSession> session) {
  auto nsHandle = makeNamespaceBridgeHandle(weak_from_this(), session);
  auto result = co_await session->subscribeNamespace(makePeerSubNs(relayID_), nsHandle);
  if (result.hasValue()) {
    upstreamSubNsHandle_ = std::move(result.value());
  } else {
    XLOG(ERR) << "MoqxRelay: upstream peer subNs failed: " << result.error().reasonPhrase;
  }
}

void MoqxRelay::onUpstreamDisconnect() {
  upstreamSubNsHandle_.reset();
}

// Sends SUBSCRIBE_UPDATE to update forwarding state. Called from:
// - subscribeNamespace: forwarder was empty, new subscriber added
// (forward=true)
// - subscribe: first forwarding subscriber added (forward=true)
// - onEmpty: last subscriber left a publish subscription (forward=false)
// - forwardChanged: forwarding subscriber count changed
folly::coro::Task<void>
MoqxRelay::doSubscribeUpdate(std::shared_ptr<Publisher::SubscriptionHandle> handle, bool forward) {
  auto updateRes = co_await handle->requestUpdate(
      {RequestID(0),
       handle->subscribeOk().requestID,
       kLocationMin,
       kLocationMax.group,
       kDefaultPriority,
       /*forward=*/forward}
  );
  if (updateRes.hasError()) {
    XLOG(ERR) << "requestUpdate failed: " << updateRes.error().reasonPhrase;
  }
}

// Sends a REQUEST_UPDATE that carries only the NEW_GROUP_REQUEST parameter.
folly::coro::Task<void> MoqxRelay::doNewGroupRequestUpdate(
    std::shared_ptr<Publisher::SubscriptionHandle> handle,
    uint64_t newGroupRequestValue
) {
  XLOG(DBG4) << "Sending NEW_GROUP_REQUEST update: " << newGroupRequestValue;
  RequestUpdate update;
  update.requestID = RequestID(0);
  update.existingRequestID = handle->subscribeOk().requestID;
  update.params.insertParam(
      Parameter(folly::to_underlying(TrackRequestParamKey::NEW_GROUP_REQUEST), newGroupRequestValue)
  );
  auto updateRes = co_await handle->requestUpdate(std::move(update));
  if (updateRes.hasError()) {
    XLOG(ERR) << "NEW_GROUP_REQUEST update failed: " << updateRes.error().reasonPhrase;
  }
}

std::shared_ptr<MoqxRelay::NamespaceNode> MoqxRelay::findNamespaceNode(
    const TrackNamespace& ns,
    bool createMissingNodes,
    MatchType matchType,
    std::vector<std::pair<std::shared_ptr<MoQSession>, NamespaceNode::NamespaceSubscriberInfo>>*
        sessions
) {
  std::shared_ptr<NamespaceNode> nodePtr(std::shared_ptr<void>(), &publishNamespaceRoot_);
  for (auto i = 0ul; i < ns.size(); i++) {
    if (sessions) {
      // Extract session pointers with their subscriber info from the map
      for (const auto& [session, info] : nodePtr->sessions) {
        sessions->emplace_back(session, info);
      }
    }
    auto& name = ns[i];
    auto it = nodePtr->children.find(name);
    if (it == nodePtr->children.end()) {
      if (createMissingNodes) {
        auto node = std::make_shared<NamespaceNode>(*this, nodePtr.get());
        nodePtr->children.emplace(name, node);
        // Don't increment yet - only when content is actually added
        nodePtr = std::move(node);
      } else if (matchType == MatchType::Prefix && nodePtr.get() != &publishNamespaceRoot_) {
        return nodePtr;
      } else {
        XLOG(ERR) << "prefix not found in publishNamespace tree";
        return nullptr;
      }
    } else {
      nodePtr = it->second;
    }
  }
  return nodePtr;
}

std::shared_ptr<Subscriber::PublishNamespaceHandle> MoqxRelay::doPublishNamespace(
    PublishNamespace pubNs,
    std::shared_ptr<MoQSession> session,
    std::shared_ptr<Subscriber::PublishNamespaceCallback> callback
) {
  XLOG(DBG1) << __func__ << " ns=" << pubNs.trackNamespace;
  // check auth
  if (!pubNs.trackNamespace.startsWith(allowedNamespacePrefix_)) {
    return nullptr;
  }
  std::vector<std::pair<std::shared_ptr<MoQSession>, NamespaceNode::NamespaceSubscriberInfo>>
      sessions;
  auto nodePtr = findNamespaceNode(
      pubNs.trackNamespace,
      /*createMissingNodes=*/true,
      MatchType::Exact,
      &sessions
  );

  // Log if there is already a session that has publishNamespace-d this track
  if (nodePtr->sourceSession) {
    XLOG(WARNING) << "PublishNamespace: Existing session (" << nodePtr->sourceSession.get()
                  << ") has already published trackNamespace=" << pubNs.trackNamespace;
    // Since we don't fully support multiple publishers -- cancel the old
    // publishNamespace and remove ongoing subscriptions to this publisher
    // in that namespace.  Note: it could have publishNamespace-d a more
    // specific namespace which hasn't been overridden by the new publisher, but
    // for now we don't support that.
    if (nodePtr->publishNamespaceCallback) {
      nodePtr->publishNamespaceCallback->publishNamespaceCancel(
          PublishNamespaceErrorCode::CANCELLED,
          "New publisher"
      );
      nodePtr->publishNamespaceCallback.reset();
    }
    for (auto it = subscriptions_.begin(); it != subscriptions_.end();) {
      // Check if the subscription's FullTrackName is in this namespace
      if (it->first.trackNamespace.startsWith(pubNs.trackNamespace) &&
          it->second.upstream == nodePtr->sourceSession) {
        XLOG(DBG4) << "Erasing subscription to " << it->first;
        it = subscriptions_.erase(it);
      } else {
        ++it;
      }
    }
    nodePtr->sourceSession.reset();
  }

  // Include sessions that subscribed exactly this namespace as well.
  // findNamespaceNode(..., &sessions) collects subscribers attached to prefixes
  // along the path, but does not include this node's own sessions.
  for (const auto& [outSession, info] : nodePtr->sessions) {
    sessions.emplace_back(outSession, info);
  }

  bool wasEmpty = !nodePtr->hasLocalSessions();
  nodePtr->sourceSession = session;
  nodePtr->publishNamespaceCallback = std::move(callback);
  nodePtr->trackNamespace_ = pubNs.trackNamespace;
  nodePtr->setPublishNamespaceOk({.requestID = pubNs.requestID, .requestSpecificParams = {}});

  // If this is the first content added to this node, notify parent
  if (wasEmpty && nodePtr->parent_) {
    nodePtr->parent_->incrementActiveChildren();
  }
  for (auto& [outSession, info] : sessions) {
    if (outSession != session && (info.options == SubscribeNamespaceOptions::NAMESPACE ||
                                  info.options == SubscribeNamespaceOptions::BOTH)) {
      if (info.namespacePublishHandle) {
        // Draft 16+: send NAMESPACE message on the bidi stream
        TrackNamespace suffix(std::vector<std::string>(
            pubNs.trackNamespace.trackNamespace.begin() + info.trackNamespacePrefix.size(),
            pubNs.trackNamespace.trackNamespace.end()
        ));
        info.namespacePublishHandle->namespaceMsg(suffix);
      } else {
        // Draft <= 15: send PUBLISH_NAMESPACE on a new stream
        auto exec = outSession->getExecutor();
        co_withExecutor(exec, publishNamespaceToSession(outSession, pubNs, nodePtr)).start();
      }
    }
  }
  return nodePtr;
}

folly::coro::Task<Subscriber::PublishNamespaceResult> MoqxRelay::publishNamespace(
    PublishNamespace pubNs,
    std::shared_ptr<Subscriber::PublishNamespaceCallback> callback
) {
  // TODO: store auth for forwarding on future SubscribeNamespace?
  auto session = MoQSession::getRequestSession();
  auto requestID = pubNs.requestID;
  auto result = doPublishNamespace(std::move(pubNs), session, std::move(callback));
  if (!result) {
    co_return folly::makeUnexpected(
        PublishNamespaceError{requestID, PublishNamespaceErrorCode::UNINTERESTED, "bad namespace"}
    );
  }
  co_return result;
}

folly::coro::Task<void> MoqxRelay::publishNamespaceToSession(
    std::shared_ptr<MoQSession> session,
    PublishNamespace pubNs,
    std::shared_ptr<NamespaceNode> nodePtr
) {
  auto publishNamespaceHandle = co_await session->publishNamespace(pubNs);
  if (publishNamespaceHandle.hasError()) {
    XLOG(ERR) << "PublishNamespace failed err=" << publishNamespaceHandle.error().reasonPhrase;
  } else {
    // This can race with unsubscribeNamespace
    nodePtr->namespacesPublished[session] = std::move(publishNamespaceHandle.value());
  }
}

// NamespaceNode ref count management methods for pruning
void MoqxRelay::NamespaceNode::incrementActiveChildren() {
  activeChildCount_++;
  // Propagate up if this was the first active child
  if (activeChildCount_ == 1 && parent_ && !hasLocalSessions()) {
    parent_->incrementActiveChildren();
  }
}

void MoqxRelay::NamespaceNode::decrementActiveChildren() {
  XCHECK_GT(activeChildCount_, 0);
  activeChildCount_--;
}

// Walk up the tree to find and prune the highest empty ancestor
void MoqxRelay::NamespaceNode::tryPruneChild(const std::string& childKey) {
  auto it = children.find(childKey);
  if (it == children.end()) {
    return;
  }

  auto childNode = it->second.get();
  if (childNode->shouldKeep()) {
    return;
  }

  // Walk up the tree, decrementing counts, to find highest empty ancestor
  // Track the key to remove and the parent to remove it from
  std::string keyToRemove = childKey;
  NamespaceNode* parentOfNodeToRemove = this;
  NamespaceNode* current = this;

  while (current) {
    XCHECK_GT(current->activeChildCount_, 0);
    current->activeChildCount_--;

    // If current still has content or children after decrement, stop walking
    // Remove keyToRemove from parentOfNodeToRemove
    if (current->hasLocalSessions() || current->activeChildCount_ > 0) {
      break;
    }

    // Current is now empty too - if it has a parent, continue walking up
    if (!current->parent_) {
      // We've reached root - can't remove root, so stop
      break;
    }

    // Current is empty and not root, so it becomes the new candidate for
    // removal Find the key for current in its parent's children map
    for (const auto& [key, node] : current->parent_->children) {
      if (node.get() == current) {
        keyToRemove = key;
        parentOfNodeToRemove = current->parent_;
        break;
      }
    }

    current = current->parent_;
  }

  // Remove the highest empty node from its parent
  XLOG(DBG1) << "Pruning empty subtree at: " << keyToRemove;
  parentOfNodeToRemove->children.erase(keyToRemove);
}

void MoqxRelay::doPublishNamespaceDone(
    const TrackNamespace& trackNamespace,
    std::shared_ptr<MoQSession> session
) {
  XLOG(DBG1) << __func__ << " ns=" << trackNamespace;
  // Node would be useful if there were back links
  auto nodePtr = findNamespaceNode(trackNamespace);
  if (!nodePtr) {
    // Node was already pruned, nothing to do, maybe app called unannouce twice?
    XLOG(DBG1) << "Node already pruned for ns=" << trackNamespace;
    return;
  }

  // Track if node had local content before modification
  bool hadLocalContent = nodePtr->hasLocalSessions();

  // Only allow publishNamespaceDone if there is an owner and the caller is that
  // owner
  if (nodePtr->sourceSession == nullptr || nodePtr->sourceSession != session) {
    XLOG(DBG1) << "Ignoring publishNamespaceDone for ns=" << trackNamespace
               << " (no owner or non-owner session)";
    return;
  }

  nodePtr->sourceSession = nullptr;
  nodePtr->publishNamespaceCallback.reset();

  // Notify downstream namespace subscribers
  std::vector<std::pair<std::shared_ptr<MoQSession>, NamespaceNode::NamespaceSubscriberInfo>>
      sessions;
  findNamespaceNode(
      trackNamespace,
      /*createMissingNodes=*/false,
      MatchType::Exact,
      &sessions
  );
  // Also include sessions at the target node itself
  for (const auto& [sessionPtr, info] : nodePtr->sessions) {
    sessions.emplace_back(sessionPtr, info);
  }
  for (auto& [outSession, info] : sessions) {
    if (outSession != session && (info.options == SubscribeNamespaceOptions::NAMESPACE ||
                                  info.options == SubscribeNamespaceOptions::BOTH)) {
      auto maybeVersion = outSession->getNegotiatedVersion();
      if (maybeVersion.has_value() && getDraftMajorVersion(*maybeVersion) >= 16) {
        // Draft 16+: send NAMESPACE_DONE on the bidi stream
        if (info.namespacePublishHandle) {
          TrackNamespace suffix(std::vector<std::string>(
              trackNamespace.trackNamespace.begin() + info.trackNamespacePrefix.size(),
              trackNamespace.trackNamespace.end()
          ));
          info.namespacePublishHandle->namespaceDoneMsg(suffix);
        }
      } else {
        // Draft <= 15: send PUBLISH_NAMESPACE_DONE on the existing stream
        auto it = nodePtr->namespacesPublished.find(outSession);
        if (it != nodePtr->namespacesPublished.end()) {
          auto exec = outSession->getExecutor();
          exec->add([publishNamespaceHandle = it->second] {
            publishNamespaceHandle->publishNamespaceDone();
          });
        }
      }
    }
  }
  nodePtr->namespacesPublished.clear();

  // Prune if node became empty and has a parent
  if (hadLocalContent && !nodePtr->shouldKeep() && nodePtr->parent_ &&
      !trackNamespace.trackNamespace.empty()) {
    nodePtr->parent_->tryPruneChild(trackNamespace.trackNamespace.back());
  }
}

void MoqxRelay::publishNamespaceDone(const TrackNamespace& trackNamespace, NamespaceNode*) {
  doPublishNamespaceDone(trackNamespace, MoQSession::getRequestSession());
}

void MoqxRelay::onPublishDone(const FullTrackName& ftn) {
  XLOG(DBG1) << __func__ << " ftn=" << ftn;

  auto it = subscriptions_.find(ftn);
  if (it != subscriptions_.end()) {
    if (it->second.isPublish) {
      // Remove from publishes map
      auto nodePtr = findNamespaceNode(ftn.trackNamespace);
      if (nodePtr) {
        // If the publisher also had a TRACK_FILTER subscription, remove the
        // self-track from their exclusion set before erasing from publishes.
        auto publishIt = nodePtr->publishes.find(ftn.trackName);
        if (publishIt != nodePtr->publishes.end()) {
          auto& publisherSession = publishIt->second;
          auto sessIt = nodePtr->sessions.find(publisherSession);
          if (sessIt != nodePtr->sessions.end() && sessIt->second.trackFilter) {
            auto& tf = *sessIt->second.trackFilter;
            auto rankingIt = nodePtr->rankings.find(tf.propertyType);
            if (rankingIt != nodePtr->rankings.end()) {
              rankingIt->second
                  ->removePublishedTrackFromSession(tf.maxSelected, publisherSession, ftn);
            }
          }
        }
        bool hadLocalContent = nodePtr->hasLocalSessions();
        nodePtr->publishes.erase(ftn.trackName);

        // Prune if node became empty and has a parent
        if (hadLocalContent && !nodePtr->shouldKeep() && nodePtr->parent_ &&
            !ftn.trackNamespace.trackNamespace.empty()) {
          nodePtr->parent_->tryPruneChild(ftn.trackNamespace.trackNamespace.back());
        }
      }
    }

    // Clear the handle and upstream - this signals publisher is done
    // Clearing upstream is important to break circular reference:
    // session holds FilterConsumer, relay holds session in upstream
    it->second.handle.reset();
    it->second.upstream.reset();

    // If forwarder has no subscribers, clean up immediately
    // Otherwise onEmpty will be called when last subscriber leaves
    if (it->second.forwarder->empty()) {
      XLOG(DBG1) << "Publisher terminated with no subscribers, cleaning up " << ftn;
      subscriptions_.erase(it);
    }
  }
}

Subscriber::PublishResult
MoqxRelay::publish(PublishRequest pub, std::shared_ptr<Publisher::SubscriptionHandle> handle) {
  XLOG(DBG1) << __func__ << " ftn=" << pub.fullTrackName;
  XCHECK(handle) << "Publish handle cannot be null";
  if (!pub.fullTrackName.trackNamespace.startsWith(allowedNamespacePrefix_)) {
    return folly::makeUnexpected(
        PublishError{pub.requestID, PublishErrorCode::UNINTERESTED, "bad namespace"}
    );
  }

  if (pub.fullTrackName.trackNamespace.empty()) {
    return folly::makeUnexpected(
        PublishError({pub.requestID, PublishErrorCode::INTERNAL_ERROR, "namespace required"})
    );
  }

  // Find All Nodes that Subscribed to this namespace (including
  // prefix ns)
  std::vector<std::pair<std::shared_ptr<MoQSession>, NamespaceNode::NamespaceSubscriberInfo>>
      sessions = {};
  auto nodePtr = findNamespaceNode(
      pub.fullTrackName.trackNamespace,
      /*createMissingNodes=*/true,
      MatchType::Exact,
      &sessions
  );
  // Extract session pointers with their subscriber info from the map
  for (const auto& [sessionPtr, info] : nodePtr->sessions) {
    sessions.emplace_back(sessionPtr, info);
  }

  auto session = MoQSession::getRequestSession();
  bool wasEmpty = !nodePtr->hasLocalSessions();

  auto it = subscriptions_.find(pub.fullTrackName);
  if (it != subscriptions_.end()) {
    // someone already published this FTN, we don't support
    // multipublisher
    XLOG(DBG1) << "New publisher for existing subscription";
    nodePtr->publishes.erase(pub.fullTrackName.trackName);
    // handle is null when the previous publisher already terminated (e.g.
    // reconnect after a dropped connection with subscribers still draining open
    // subgroups).  onPublishDone() already reset handle and drained the
    // forwarder, so skip both calls to avoid a null deref and a double
    // publishDone.
    if (it->second.handle) {
      it->second.handle->unsubscribe();
      it->second.forwarder->publishDone(
          {RequestID(0),
           PublishDoneStatusCode::SUBSCRIPTION_ENDED,
           0, // filled in by session
           "upstream disconnect"}
      );
    }
    XLOG(DBG4) << "Erasing subscription to " << it->first;
    subscriptions_.erase(it);
  }
  auto res = nodePtr->publishes.emplace(pub.fullTrackName.trackName, session);
  XCHECK(res.second) << "Duplicate publish";

  // If this is the first content added to this node, notify parent
  if (wasEmpty && nodePtr->hasLocalSessions() && nodePtr->parent_) {
    nodePtr->parent_->incrementActiveChildren();
  }

  // Create Forwarder for this publish
  auto forwarder = std::make_shared<MoQForwarder>(pub.fullTrackName, pub.largest);

  // Set Forwarder Params
  forwarder->setExtensions(pub.extensions);

  auto subRes = subscriptions_.emplace(
      std::piecewise_construct,
      std::forward_as_tuple(pub.fullTrackName),
      std::forward_as_tuple(forwarder, session)
  );
  auto& rsub = subRes.first->second;
  rsub.promise.setValue(folly::unit);
  rsub.requestID = pub.requestID;
  rsub.handle = std::move(handle);
  rsub.isPublish = true;

  // Build filter chain: TopNFilter → TerminationFilter → Forwarder.
  // TopNFilter sits at the front so it sees every object and can update
  // PropertyRanking with current property values.
  auto terminationFilter =
      std::make_shared<TerminationFilter>(shared_from_this(), pub.fullTrackName, forwarder);
  auto topNFilter = std::make_shared<TopNFilter>(
      pub.fullTrackName,
      std::static_pointer_cast<TrackConsumer>(terminationFilter)
  );
  rsub.topNFilter = topNFilter;

  // Wire activity tracking: TopNFilter writes lastObjectTime on every object,
  // and fires onActivity callbacks (throttled) for idle sweep triggering.
  topNFilter->setActivityTarget(&rsub.lastObjectTime);
  topNFilter->setActivityThreshold(activityThreshold_);

  // Set up self-exclusion for any publisher-subscriber (this session is also a
  // TRACK_FILTER subscriber) BEFORE registerTrack fires selection notifications.
  // If addPublishedTrackToSession ran after registerTrack, the publisher would
  // briefly receive its own track before the eviction corrected it.
  for (const auto& [outSession, info] : sessions) {
    if (info.trackFilter && outSession == session) {
      auto rankingIt = nodePtr->rankings.find(info.trackFilter->propertyType);
      if (rankingIt != nodePtr->rankings.end()) {
        rankingIt->second
            ->addPublishedTrackToSession(info.trackFilter->maxSelected, session, pub.fullTrackName);
      }
    }
  }

  // Register track with all PropertyRankings already active on this node and
  // wire up value-change, track-ended, and activity observers on the TopNFilter.
  for (auto& [propertyType, ranking] : nodePtr->rankings) {
    auto initialValue = pub.extensions.getIntExtension(propertyType);
    ranking->registerTrack(pub.fullTrackName, initialValue, session);
    topNFilter->registerObserver(
        propertyType,
        PropertyObserver{
            .onValueChanged = [ranking, ftn = pub.fullTrackName](uint64_t value
                              ) { ranking->updateSortValue(ftn, value); },
            .onTrackEnded = [ranking, ftn = pub.fullTrackName]() { ranking->removeTrack(ftn); },
            .onActivity = [ranking]() { ranking->sweepIdle(); }
        }
    );
  }

  uint64_t nSubscribers = 0;
  bool hasTrackFilterSub = false;
  for (auto& [outSession, info] : sessions) {
    if (info.trackFilter) {
      // TRACK_FILTER subscribers: PropertyRanking handles selection via
      // onTrackSelected; don't publish directly here.
      hasTrackFilterSub = true;
      continue;
    }
    if (outSession != session && (info.options == SubscribeNamespaceOptions::PUBLISH ||
                                  info.options == SubscribeNamespaceOptions::BOTH)) {
      nSubscribers++;
      auto exec = outSession->getExecutor();
      co_withExecutor(exec, publishToSession(outSession, forwarder, info.forward)).start();
    }
  }
  forwarder->setCallback(shared_from_this());

  // Forward if there are direct subscribers OR TRACK_FILTER subscribers
  // (PropertyRanking needs objects to evaluate property values for ranking).
  bool shouldForward = (nSubscribers > 0) || hasTrackFilterSub;

  return PublishConsumerAndReplyTask{
      std::static_pointer_cast<TrackConsumer>(topNFilter),
      folly::coro::makeTask<folly::Expected<PublishOk, PublishError>>(PublishOk{
          pub.requestID,
          /*forward=*/shouldForward,
          kDefaultPriority,
          pub.groupOrder,
          LocationType::AbsoluteRange,
          kLocationMin,
          kLocationMax.group
      })
  };
}

folly::coro::Task<void> MoqxRelay::publishToSession(
    std::shared_ptr<MoQSession> session,
    std::shared_ptr<MoQForwarder> forwarder,
    bool forward,
    bool trackFilterSubscriber
) {
  if (session->isClosed()) {
    XLOG(DBG4) << "publishToSession: session closed, skipping " << forwarder->fullTrackName();
    co_return;
  }
  auto subscriber = forwarder->addSubscriber(session, forward);
  if (!subscriber) {
    XLOG(ERR) << "Subscribe failed: addSubscriber returned null for " << forwarder->fullTrackName();
    co_return;
  }
  // Direct subscribers are pinned (not evictable by PropertyRanking).
  // TRACK_FILTER subscribers are unpinned so onTrackEvicted can remove them.
  subscriber->pinned = !trackFilterSubscriber;
  XLOG(DBG4) << "added subscriber for ftn=" << forwarder->fullTrackName();
  auto guard = folly::makeGuard([subscriber] { subscriber->unsubscribe(); });

  auto pubInitial = session->publish(subscriber->getPublishRequest(), subscriber);
  if (pubInitial.hasError()) {
    XLOG(ERR) << "Publish failed err=" << pubInitial.error().reasonPhrase;
    co_return;
  }
  subscriber->trackConsumer = std::move(pubInitial->consumer);
  auto pubResult = co_await co_awaitTry(std::move(pubInitial->reply));
  if (pubResult.hasException()) {
    XLOG(ERR) << "Publish failed err=" << pubResult.exception().what();
    co_return;
  }
  if (pubResult.value().hasError()) {
    XLOG(ERR) << "Publish failed err=" << pubResult.value().error().reasonPhrase;
    co_return;
  }
  guard.dismiss();
  XLOG(DBG1) << "Publish OK sess=" << session.get();
  auto& pubOk = pubResult.value().value();

  // Process the PUBLISH_OK response - updates range, forward flag, and
  // handles NEW_GROUP_REQUEST forwarding via callback
  subscriber->onPublishOk(pubOk);
}

class MoqxRelay::NamespaceSubscription : public Publisher::SubscribeNamespaceHandle {
public:
  NamespaceSubscription(
      std::shared_ptr<MoqxRelay> relay,
      std::shared_ptr<MoQSession> session,
      SubscribeNamespaceOk ok,
      TrackNamespace trackNamespacePrefix
  )
      : Publisher::SubscribeNamespaceHandle(std::move(ok)), relay_(std::move(relay)),
        session_(std::move(session)), trackNamespacePrefix_(std::move(trackNamespacePrefix)) {}

  void unsubscribeNamespace() override {
    if (relay_) {
      relay_->unsubscribeNamespace(trackNamespacePrefix_, std::move(session_));
      relay_.reset();
    }
  }

  folly::coro::Task<RequestUpdateResult> requestUpdate(RequestUpdate reqUpdate) override {
    co_return folly::makeUnexpected(RequestError{
        reqUpdate.requestID,
        RequestErrorCode::NOT_SUPPORTED,
        "REQUEST_UPDATE not supported for relay SUBSCRIBE_NAMESPACE"
    });
  }

private:
  std::shared_ptr<MoqxRelay> relay_;
  std::shared_ptr<MoQSession> session_;
  TrackNamespace trackNamespacePrefix_;
};

// Filter TrackConsumer that intercepts publishDone to clean up relay state.
// Holds a weak_ptr to avoid a reference cycle: relay owns RelaySubscription
// which owns the filter chain (TopNFilter→TerminationFilter), so a strong
// relay ref here would prevent the relay from ever being destroyed.
class MoqxRelay::TerminationFilter : public TrackConsumerFilter {
public:
  TerminationFilter(
      std::weak_ptr<MoqxRelay> relay,
      FullTrackName ftn,
      std::shared_ptr<TrackConsumer> downstream
  )
      : TrackConsumerFilter(std::move(downstream)), relay_(std::move(relay)), ftn_(std::move(ftn)) {
  }

  folly::Expected<folly::Unit, MoQPublishError> publishDone(PublishDone pubDone) override {
    // Notify relay that publisher is done - this will:
    // 1. Remove from nodePtr->publishes
    // 2. Clear subscription.handle
    if (auto relay = relay_.lock()) {
      relay->onPublishDone(ftn_);
    }
    // Change the downstream code to something like "upstream ended"?
    return TrackConsumerFilter::publishDone(std::move(pubDone));
  }

private:
  std::weak_ptr<MoqxRelay> relay_;
  FullTrackName ftn_;
};

std::shared_ptr<TrackConsumer> MoqxRelay::getSubscribeWriteback(
    const FullTrackName& ftn,
    std::shared_ptr<TrackConsumer> consumer
) {
  auto baseConsumer =
      cache_ ? cache_->getSubscribeWriteback(ftn, std::move(consumer)) : std::move(consumer);
  auto filterConsumer =
      std::make_shared<TerminationFilter>(shared_from_this(), ftn, std::move(baseConsumer));
  return std::static_pointer_cast<TrackConsumer>(filterConsumer);
}

folly::coro::Task<Publisher::SubscribeNamespaceResult> MoqxRelay::subscribeNamespace(
    SubscribeNamespace subNs,
    std::shared_ptr<NamespacePublishHandle> namespacePublishHandle
) {
  XLOG(DBG1) << __func__ << " nsp=" << subNs.trackNamespacePrefix;

  auto session = MoQSession::getRequestSession();

  // Relay peering: if the incoming subNs carries a relay auth token, the peer
  // is a relay. Reciprocate with our own peer subNs so the peer gets our
  // namespace announcements as publishers connect.
  if (!relayID_.empty() && isPeerSubNs(subNs)) {
    XLOG(INFO) << __func__ << ": peer relay detected, reciprocating peer subNs";
    auto handle = makeNamespaceBridgeHandle(weak_from_this(), session);
    auto recipResult = co_await session->subscribeNamespace(
        makePeerSubNs(),
        handle
    ); // no token: reciprocal, prevents loop
    if (recipResult.hasError()) {
      XLOG(ERR) << "Reciprocal peer subNs failed: " << recipResult.error().reasonPhrase;
    } else {
      peerSubNsHandles_.emplace(session.get(), std::move(recipResult.value()));
    }
    // Fall through: register the peer as a normal subNs subscriber so it
    // receives namespace announcements as publishers connect.
  }
  auto maybeNegotiatedVersion = session->getNegotiatedVersion();
  CHECK(maybeNegotiatedVersion.has_value());

  // check auth
  // Allow empty namespace prefix only for draft-16 and above.
  if (subNs.trackNamespacePrefix.empty() && getDraftMajorVersion(*maybeNegotiatedVersion) < 16) {
    co_return folly::makeUnexpected(SubscribeNamespaceError{
        subNs.requestID,
        SubscribeNamespaceErrorCode::NAMESPACE_PREFIX_UNKNOWN,
        "empty"
    });
  }
  auto nodePtr = findNamespaceNode(subNs.trackNamespacePrefix, /*createMissingNodes=*/true);

  SubscribeNamespaceOptions effectiveOptions;
  effectiveOptions = subNs.options;

  // Parse TRACK_FILTER parameter if present (draft-16+, SUBSCRIBE_NAMESPACE only)
  std::optional<TrackFilter> trackFilter;
  for (const auto& param : subNs.params) {
    if (param.key == folly::to_underlying(TrackRequestParamKey::TRACK_FILTER)) {
      trackFilter = param.asTrackFilter;
      break;
    }
  }

  // Check if this is the first session subscriber for this node
  bool wasEmpty = !nodePtr->hasLocalSessions();
  nodePtr->sessions.emplace(
      session,
      NamespaceNode::NamespaceSubscriberInfo{
          subNs.forward,
          effectiveOptions,
          namespacePublishHandle,
          subNs.trackNamespacePrefix,
          trackFilter
      }
  );

  // If TRACK_FILTER is present, enroll session in PropertyRanking for top-N selection.
  // onTrackSelected will publish individual tracks as they enter the top-N.
  if (trackFilter) {
    auto ranking = getOrCreateRanking(nodePtr, trackFilter->propertyType);
    // Collect tracks already published by this session so the ranking can set
    // up self-exclusion from the start (publisher-subscriber case).
    std::vector<FullTrackName> publishedBySession;
    for (const auto& [trackName, publishSession] : nodePtr->publishes) {
      if (publishSession == session) {
        publishedBySession.emplace_back(FullTrackName{nodePtr->trackNamespace_, trackName});
      }
    }
    ranking->addSessionToTopNGroup(
        trackFilter->maxSelected,
        session,
        subNs.forward,
        std::move(publishedBySession)
    );
  }

  // If this is the first content added to this node, notify parent
  if (wasEmpty && nodePtr->hasLocalSessions() && nodePtr->parent_) {
    nodePtr->parent_->incrementActiveChildren();
  }

  // Find all nested PublishNamespaces/Publishes and forward
  std::deque<std::tuple<TrackNamespace, std::shared_ptr<NamespaceNode>>> nodes{
      {subNs.trackNamespacePrefix, nodePtr}
  };
  auto exec = session->getExecutor();
  while (!nodes.empty()) {
    auto [prefix, nodePtr] = std::move(*nodes.begin());
    nodes.pop_front();
    if (nodePtr->sourceSession && nodePtr->sourceSession != session) {
      if (getDraftMajorVersion(*maybeNegotiatedVersion) >= 16) {
        if (subNs.options == SubscribeNamespaceOptions::NAMESPACE ||
            subNs.options == SubscribeNamespaceOptions::BOTH) {
          // Compute the suffix: prefix minus subNs.trackNamespacePrefix
          TrackNamespace suffix(std::vector<std::string>(
              prefix.trackNamespace.begin() + subNs.trackNamespacePrefix.size(),
              prefix.trackNamespace.end()
          ));
          namespacePublishHandle->namespaceMsg(suffix);
        }
      } else {
        // TODO: Auth/params
        co_withExecutor(
            exec,
            publishNamespaceToSession(session, {subNs.requestID, prefix}, nodePtr)
        )
            .start();
      }
    }
    for (auto& publishEntry : nodePtr->publishes) {
      auto& publishSession = publishEntry.second;
      FullTrackName ftn{prefix, publishEntry.first};
      auto subscriptionIt = subscriptions_.find(ftn);
      if (subscriptionIt == subscriptions_.end()) {
        XLOG(ERR) << "Invalid state, no subscription for publish ftn=" << ftn;
        continue;
      }
      auto& forwarder = subscriptionIt->second.forwarder;
      auto& rsub = subscriptionIt->second;
      if (forwarder->empty() && !rsub.isPublish) {
        // Use forward value from this namespace subscription
        co_withExecutor(exec, doSubscribeUpdate(rsub.handle, subNs.forward)).start();
      }
      auto maybeNegotiatedVersion = session->getNegotiatedVersion();
      CHECK(maybeNegotiatedVersion.has_value());

      // TRACK_FILTER subscribers: PropertyRanking drives selection via
      // onTrackSelected; skip direct publish here.
      if (trackFilter) {
        continue;
      }

      if (getDraftMajorVersion(*maybeNegotiatedVersion) <= 15 ||
          (subNs.options == SubscribeNamespaceOptions::BOTH ||
           subNs.options == SubscribeNamespaceOptions::PUBLISH)) {
        if (publishSession != session) {
          co_withExecutor(exec, publishToSession(session, forwarder, subNs.forward)).start();
        }
      }
    }
    for (auto& nextNodeIt : nodePtr->children) {
      TrackNamespace nodePrefix(prefix);
      nodePrefix.append(nextNodeIt.first);
      nodes.emplace_back(std::forward_as_tuple(nodePrefix, nextNodeIt.second));
    }
  }
  co_return std::make_shared<NamespaceSubscription>(
      shared_from_this(),
      std::move(session),
      SubscribeNamespaceOk{.requestID = subNs.requestID, .requestSpecificParams = {}},
      subNs.trackNamespacePrefix
  );
}

void MoqxRelay::unsubscribeNamespace(
    const TrackNamespace& trackNamespacePrefix,
    std::shared_ptr<MoQSession> session
) {
  XLOG(DBG1) << __func__ << " nsp=" << trackNamespacePrefix;
  // Clean up the reciprocal peer subNs handle for this session if present.
  peerSubNsHandles_.erase(session.get());
  auto nodePtr = findNamespaceNode(trackNamespacePrefix);
  if (!nodePtr) {
    // TODO: maybe error?
    return;
  }

  // Track if node had local content before modification
  bool hadLocalContent = nodePtr->hasLocalSessions();

  auto it = nodePtr->sessions.find(session);
  if (it != nodePtr->sessions.end()) {
    // If session used TRACK_FILTER, remove it from the PropertyRanking group.
    if (it->second.trackFilter) {
      auto rankingIt = nodePtr->rankings.find(it->second.trackFilter->propertyType);
      if (rankingIt != nodePtr->rankings.end()) {
        rankingIt->second->removeSessionFromTopNGroup(it->second.trackFilter->maxSelected, session);
      }
    }

    nodePtr->sessions.erase(it);

    // Prune if node became empty and has a parent
    if (hadLocalContent && !nodePtr->shouldKeep() && nodePtr->parent_ &&
        !trackNamespacePrefix.trackNamespace.empty()) {
      nodePtr->parent_->tryPruneChild(trackNamespacePrefix.trackNamespace.back());
    }
    return;
  }
  // TODO: error?
  XLOG(DBG1) << "Namespace prefix was not subscribed by this session";
}

std::shared_ptr<MoQSession> MoqxRelay::findPublishNamespaceSession(const TrackNamespace& ns) {
  /*
   * This function is called from subscribe() and fetch().
   * We use MatchType::Prefix here because the relay routes SUBSCRIBE and FETCH
   * to the publisher who published the closest matching broader
   * namespace, not necessarily the exact match.
   */
  auto nodePtr = findNamespaceNode(ns, /*createMissingNodes=*/false, MatchType::Prefix);
  if (!nodePtr) {
    return nullptr;
  }
  return nodePtr->sourceSession;
}

MoqxRelay::PublishState MoqxRelay::findPublishState(const FullTrackName& ftn) {
  PublishState state;
  auto nodePtr =
      findNamespaceNode(ftn.trackNamespace, /*createMissingNodes=*/false, MatchType::Exact);

  if (!nodePtr) {
    // Node doesn't exist - tree was properly pruned
    return state;
  }

  state.nodeExists = true;

  auto it = nodePtr->publishes.find(ftn.trackName);
  if (it != nodePtr->publishes.end()) {
    state.session = it->second;
  }

  return state;
}

folly::coro::Task<Publisher::SubscribeResult>
MoqxRelay::subscribe(SubscribeRequest subReq, std::shared_ptr<TrackConsumer> consumer) {
  auto session = MoQSession::getRequestSession();
  auto subscriptionIt = subscriptions_.find(subReq.fullTrackName);
  if (subscriptionIt == subscriptions_.end()) {
    // first subscriber

    // check auth
    // get trackNamespace
    if (subReq.fullTrackName.trackNamespace.empty()) {
      // message error?
      co_return folly::makeUnexpected(SubscribeError(
          {subReq.requestID, SubscribeErrorCode::TRACK_NOT_EXIST, "namespace required"}
      ));
    }
    auto upstreamSession = findPublishNamespaceSession(subReq.fullTrackName.trackNamespace);
    if (!upstreamSession && upstream_) {
      co_await upstream_->waitForConnected(kUpstreamConnectWaitTimeout);
      upstreamSession = findPublishNamespaceSession(subReq.fullTrackName.trackNamespace);
    }
    if (!upstreamSession) {
      // no such namespace has been published
      co_return folly::makeUnexpected(SubscribeError(
          {subReq.requestID, SubscribeErrorCode::TRACK_NOT_EXIST, "no such namespace or track"}
      ));
    }
    subReq.priority = kDefaultUpstreamPriority;
    subReq.groupOrder = GroupOrder::Default;
    // We only subscribe upstream with LargestObject. This is to satisfy other
    // subscribers that join with narrower filters
    subReq.locType = LocationType::LargestObject;
    auto forwarder = std::make_shared<MoQForwarder>(subReq.fullTrackName, std::nullopt);
    forwarder->setCallback(shared_from_this());
    auto emplaceRes = subscriptions_.emplace(
        std::piecewise_construct,
        std::forward_as_tuple(subReq.fullTrackName),
        std::forward_as_tuple(forwarder, upstreamSession)
    );
    // The iterator returned from emplace does not survive across coroutine
    // resumption, so both the guard and updating the RelaySubscription below
    // require another lookup in the subscriptions_ map.
    auto g = folly::makeGuard([this, trackName = subReq.fullTrackName] {
      auto it = subscriptions_.find(trackName);
      if (it != subscriptions_.end()) {
        it->second.promise.setException(std::runtime_error("failed"));
        XLOG(DBG4) << "Erasing subscription to " << it->first;
        subscriptions_.erase(it);
      }
    });
    // Add subscriber first in case objects come before subscribe OK.
    auto subscriber = forwarder->addSubscriber(std::move(session), subReq, std::move(consumer));
    if (!subscriber) {
      XLOG(ERR) << "addSubscriber returned null (draining?) for " << subReq.fullTrackName
                << " reqID=" << subReq.requestID;
      co_return folly::makeUnexpected(SubscribeError{
          subReq.requestID,
          SubscribeErrorCode::INTERNAL_ERROR,
          "failed to add subscriber"
      });
    }
    XLOG(DBG4) << "added subscriber for ftn=" << subReq.fullTrackName;
    // As per the spec, we must set forward = true in the subscribe request
    // to the upstream.
    // But should we if this is forward=0?
    subReq.forward = forwarder->numForwardingSubscribers() > 0;

    emplaceRes.first->second.requestID = upstreamSession->peekNextRequestID();
    auto subRes = co_await upstreamSession->subscribe(
        subReq,
        getSubscribeWriteback(subReq.fullTrackName, forwarder)
    );
    if (subRes.hasError()) {
      co_return folly::makeUnexpected(SubscribeError(
          {subReq.requestID,
           subRes.error().errorCode,
           folly::to<std::string>("upstream subscribe failed: ", subRes.error().reasonPhrase)}
      ));
    }
    // is it more correct to co_await folly::coro::co_safe_point here?
    g.dismiss();
    auto largest = subRes.value()->subscribeOk().largest;
    if (largest) {
      forwarder->updateLargest(largest->group, largest->object);
      subscriber->updateLargest(*largest);
    }
    // Save upstream extensions to forwarder (and existing subscribers) and
    // cache
    auto& upstreamExtensions = subRes.value()->subscribeOk().extensions;
    forwarder->setExtensions(upstreamExtensions);
    if (cache_) {
      cache_->setTrackExtensions(subReq.fullTrackName, upstreamExtensions);
    }

    auto it = subscriptions_.find(subReq.fullTrackName);
    // There are cases that remove the subscription like failing to
    // publish a datagram that was received before the subscribeOK
    // and then gets flushed
    if (it == subscriptions_.end()) {
      XLOG(ERR) << "Subscription is GONE, returning exception";
      co_yield folly::coro::co_error(std::runtime_error("subscription is gone"));
    }
    auto& rsub = it->second;
    rsub.requestID = subRes.value()->subscribeOk().requestID;
    rsub.handle = std::move(subRes.value());
    // Record NGR as outstanding (no fire — it rides the outgoing SUBSCRIBE).
    forwarder->tryProcessNewGroupRequest(subReq.params, /*fire=*/false);
    rsub.promise.setValue(folly::unit);
    co_return subscriber;
  } else {
    if (!subscriptionIt->second.promise.isFulfilled()) {
      // this will throw if the dependent subscribe failed, which is good
      // because subscriptionIt will be invalid
      co_await subscriptionIt->second.promise.getFuture();
    }
    auto& forwarder = subscriptionIt->second.forwarder;
    if (forwarder->largest() && subReq.locType == LocationType::AbsoluteRange &&
        subReq.endGroup < forwarder->largest()->group) {
      co_return folly::makeUnexpected(SubscribeError{
          subReq.requestID,
          SubscribeErrorCode::INVALID_RANGE,
          "Range in the past, use FETCH"
      });
      // start may be in the past, it will get adjusted forward to largest
    }
    bool forwarding = subscriptionIt->second.forwarder->numForwardingSubscribers() > 0;
    auto subscriber = subscriptionIt->second.forwarder
                          ->addSubscriber(std::move(session), subReq, std::move(consumer));
    if (!subscriber) {
      XLOG(ERR) << "addSubscriber returned null (draining?) for " << subReq.fullTrackName
                << " reqID=" << subReq.requestID;
      co_return folly::makeUnexpected(SubscribeError{
          subReq.requestID,
          SubscribeErrorCode::INTERNAL_ERROR,
          "failed to add subscriber"
      });
    }
    XLOG(DBG4) << "added subscriber for ftn=" << subReq.fullTrackName;
    if (!forwarding && subscriptionIt->second.forwarder->numForwardingSubscribers() > 0) {
      auto exec = subscriptionIt->second.upstream->getExecutor();
      co_withExecutor(exec, doSubscribeUpdate(subscriptionIt->second.handle, /*forward=*/true))
          .start();
    }

    forwarder->tryProcessNewGroupRequest(subReq.params);
    co_return subscriber;
  }
}

folly::coro::Task<Publisher::FetchResult>
MoqxRelay::fetch(Fetch fetch, std::shared_ptr<FetchConsumer> consumer) {
  auto session = MoQSession::getRequestSession();

  // check auth
  // get trackNamespace
  if (fetch.fullTrackName.trackNamespace.empty()) {
    co_return folly::makeUnexpected(
        FetchError({fetch.requestID, FetchErrorCode::TRACK_NOT_EXIST, "namespace required"})
    );
  }

  auto [standalone, joining] = fetchType(fetch);
  if (joining) {
    auto subscriptionIt = subscriptions_.find(fetch.fullTrackName);
    if (subscriptionIt == subscriptions_.end()) {
      XLOG(ERR) << "No subscription for joining fetch";
      // message error
      co_return folly::makeUnexpected(FetchError(
          {fetch.requestID, FetchErrorCode::TRACK_NOT_EXIST, "No subscription for joining fetch"}
      ));
    } else if (subscriptionIt->second.promise.isFulfilled()) {
      auto res = subscriptionIt->second.forwarder->resolveJoiningFetch(session, *joining);
      if (res.hasError()) {
        co_return folly::makeUnexpected(res.error());
      }
      fetch.args = StandaloneFetch(res.value().start, res.value().end);
      joining = nullptr;
    } else {
      // Upstream is resolving the subscribe, forward joining fetch
      joining->joiningRequestID = subscriptionIt->second.requestID;
    }
  }

  auto upstreamSession = findPublishNamespaceSession(fetch.fullTrackName.trackNamespace);
  if (!upstreamSession && upstream_) {
    co_await upstream_->waitForConnected(kUpstreamConnectWaitTimeout);
    upstreamSession = findPublishNamespaceSession(fetch.fullTrackName.trackNamespace);
  }
  if (!upstreamSession) {
    // Attempt to find matching upstream subscription (from publish)
    auto subscriptionIt = subscriptions_.find(fetch.fullTrackName);
    if (subscriptionIt != subscriptions_.end()) {
      upstreamSession = subscriptionIt->second.upstream;
    } else {
      // no such namespace has been published
      co_return folly::makeUnexpected(
          FetchError({fetch.requestID, FetchErrorCode::TRACK_NOT_EXIST, "no such namespace"})
      );
    }
  }
  if (session.get() == upstreamSession.get()) {
    co_return folly::makeUnexpected(
        FetchError({fetch.requestID, FetchErrorCode::INTERNAL_ERROR, "self fetch"})
    );
  }
  fetch.priority = kDefaultUpstreamPriority;
  if (!cache_ || joining) {
    // We can't use the cache on an unresolved joining fetch - we don't know
    // which objects are being requested.  However, once we have that resolved,
    // we SHOULD be able to serve from cache.
    if (standalone) {
      XLOG(DBG1) << "Upstream fetch {" << standalone->start.group << "," << standalone->start.object
                 << "}.." << standalone->end.group << "," << standalone->end.object << "}";
    }
    co_return co_await upstreamSession->fetch(fetch, std::move(consumer));
  }
  co_return co_await cache_->fetch(fetch, std::move(consumer), std::move(upstreamSession));
}

folly::coro::Task<Publisher::TrackStatusResult> MoqxRelay::trackStatus(TrackStatus trackStatus) {
  XLOG(DBG1) << __func__ << " ftn=" << trackStatus.fullTrackName;

  if (trackStatus.fullTrackName.trackNamespace.empty()) {
    co_return folly::makeUnexpected(TrackStatusError(
        {trackStatus.requestID, TrackStatusErrorCode::TRACK_NOT_EXIST, "namespace required"}
    ));
  }

  auto subscriptionIt = subscriptions_.find(trackStatus.fullTrackName);
  if (subscriptionIt != subscriptions_.end() &&
      subscriptionIt->second.forwarder->numForwardingSubscribers() > 0) {
    // We have active subscription - answer directly from local forwarder state
    auto& subscription = subscriptionIt->second;
    auto& forwarder = subscription.forwarder;

    TrackStatusCode statusCode = TrackStatusCode::TRACK_NOT_STARTED;
    // forwarder->largest() being set means: we have actually
    // received at least one object for this track.
    // subscription.handle being non-null means: the relay still has a
    // live upstream Publisher::SubscriptionHandle for this track
    if (forwarder->largest()) {
      if (subscription.handle) {
        statusCode = TrackStatusCode::IN_PROGRESS;
      } else {
        statusCode = TrackStatusCode::UNKNOWN;
      }
    }

    TrackStatusOk trackStatusOk;
    trackStatusOk.requestID = trackStatus.requestID;
    trackStatusOk.groupOrder = forwarder->groupOrder();
    trackStatusOk.largest = forwarder->largest();
    trackStatusOk.fullTrackName = trackStatus.fullTrackName;
    trackStatusOk.statusCode = statusCode;

    XLOG(DBG1) << "Returning local track status for " << trackStatus.fullTrackName
               << " statusCode=" << (uint32_t)statusCode;
    co_return trackStatusOk;
  } else {
    // No subscription - forward to upstream
    auto upstreamSession = findPublishNamespaceSession(trackStatus.fullTrackName.trackNamespace);
    if (!upstreamSession && upstream_) {
      co_await upstream_->waitForConnected(kUpstreamConnectWaitTimeout);
      upstreamSession = findPublishNamespaceSession(trackStatus.fullTrackName.trackNamespace);
    }
    if (!upstreamSession) {
      XLOG(DBG1) << "No upstream session for track: " << trackStatus.fullTrackName;
      co_return folly::makeUnexpected(TrackStatusError{
          trackStatus.requestID,
          TrackStatusErrorCode::TRACK_NOT_EXIST,
          "no such namespace or track"
      });
    }

    // Forward the trackStatus request to the upstream publisher session
    auto result = co_await upstreamSession->trackStatus(trackStatus);

    if (result.hasError()) {
      XLOG(DBG1) << "Upstream trackStatus failed: " << result.error().reasonPhrase;
    } else {
      XLOG(DBG1) << "Upstream trackStatus succeeded";
    }
    co_return result;
  }
}

void MoqxRelay::onEmpty(MoQForwarder* forwarder) {
  auto subscriptionIt = subscriptions_.find(forwarder->fullTrackName());
  if (subscriptionIt == subscriptions_.end()) {
    return;
  }
  auto& subscription = subscriptionIt->second;

  if (!subscription.handle) {
    // Handle is null - publisher terminated via FilterConsumer
    XLOG(INFO) << "Publisher terminated for " << subscriptionIt->first;
    subscriptions_.erase(subscriptionIt);
    return;
  }

  // Handle exists - just last subscriber left
  XLOG(INFO) << "Last subscriber removed for " << subscriptionIt->first;
  if (subscription.isPublish) {
    // if it's publish, don't unsubscribe, just subscribeUpdate forward=false
    XLOG(DBG1) << "Updating upstream subscription forward=false";
    auto exec = subscription.upstream->getExecutor();
    co_withExecutor(exec, doSubscribeUpdate(subscription.handle, /*forward=*/false)).start();
  } else {
    subscription.handle->unsubscribe();
    XLOG(DBG4) << "Erasing subscription to " << subscriptionIt->first;
    subscriptions_.erase(subscriptionIt);
  }
}

void MoqxRelay::forwardChanged(MoQForwarder* forwarder) {
  auto subscriptionIt = subscriptions_.find(forwarder->fullTrackName());
  if (subscriptionIt == subscriptions_.end()) {
    return;
  }
  auto& subscription = subscriptionIt->second;
  if (!subscription.promise.isFulfilled()) {
    // Ignore: it's the first subscriber, forward update not needed
    return;
  }
  XLOG(INFO) << "Updating forward for " << subscriptionIt->first
             << " numForwardingSubs=" << forwarder->numForwardingSubscribers();

  auto exec = subscription.upstream->getExecutor();
  co_withExecutor(
      exec,
      doSubscribeUpdate(
          subscription.handle,
          /*forward=*/forwarder->numForwardingSubscribers() > 0
      )
  )
      .start();
}

void MoqxRelay::newGroupRequested(MoQForwarder* forwarder, uint64_t group) {
  auto subscriptionIt = subscriptions_.find(forwarder->fullTrackName());
  if (subscriptionIt == subscriptions_.end()) {
    return;
  }
  auto& subscription = subscriptionIt->second;
  // Check if handle is still valid (publisher may have terminated)
  if (!subscription.handle) {
    XLOG(DBG4) << "Ignoring NEW_GROUP_REQUEST for " << subscriptionIt->first
               << " - publisher terminated";
    return;
  }
  XLOG(INFO) << "New group request detected for " << subscriptionIt->first;

  auto exec = subscription.upstream->getExecutor();
  auto handle = subscription.handle;
  co_withExecutor(exec, doNewGroupRequestUpdate(std::move(handle), group)).start();
}

// TRACK_FILTER support

std::shared_ptr<PropertyRanking>
MoqxRelay::getOrCreateRanking(std::shared_ptr<NamespaceNode> node, uint64_t propertyType) {
  auto& ranking = node->rankings[propertyType];
  if (!ranking) {
    ranking = std::make_shared<PropertyRanking>(
        propertyType,
        maxDeselected_,
        idleTimeout_,
        [this](const FullTrackName& ftn) -> std::chrono::steady_clock::time_point {
          auto it = subscriptions_.find(ftn);
          if (it == subscriptions_.end()) {
            return std::chrono::steady_clock::time_point{};
          }
          return it->second.lastObjectTime;
        },
        // Batch callback: called once per track-selected event with all sessions
        [this](
            const FullTrackName& ftn,
            const std::vector<std::pair<std::shared_ptr<MoQSession>, bool>>& sessions
        ) {
          for (const auto& [session, forward] : sessions) {
            onTrackSelected(ftn, session, forward);
          }
        },
        // Individual callback: called by addSessionToTopNGroup to notify a newly
        // joined session of tracks already in top-N at the time it subscribes.
        [this](const FullTrackName& ftn, std::shared_ptr<MoQSession> session, bool forward) {
          onTrackSelected(ftn, session, forward);
        },
        // Eviction callback
        [this](const FullTrackName& ftn, std::shared_ptr<MoQSession> session) {
          onTrackEvicted(ftn, session);
        }
    );

    // Retroactively register tracks already published under this node so that
    // a subscriber who arrives after publishers can still see existing tracks.
    // Use the forwarder's stored extensions to seed the initial property value.
    for (auto& [trackName, publishSession] : node->publishes) {
      FullTrackName ftn{node->trackNamespace_, trackName};
      std::optional<uint64_t> initialValue;
      auto subIt = subscriptions_.find(ftn);
      if (subIt != subscriptions_.end()) {
        initialValue = subIt->second.forwarder->extensions().getIntExtension(propertyType);
        // Wire value-change and track-ended observers on the existing TopNFilter.
        if (subIt->second.topNFilter) {
          auto rankingPtr = ranking;
          subIt->second.topNFilter->registerObserver(
              propertyType,
              PropertyObserver{
                  .onValueChanged = [rankingPtr, ftn](uint64_t value
                                    ) { rankingPtr->updateSortValue(ftn, value); },
                  .onTrackEnded = [rankingPtr, ftn]() { rankingPtr->removeTrack(ftn); },
                  .onActivity = [rankingPtr]() { rankingPtr->sweepIdle(); }
              }
          );
        }
      }
      ranking->registerTrack(ftn, initialValue, publishSession);
      XLOG(DBG4) << "[getOrCreateRanking] Retroactively registered track " << ftn;
    }
  }
  return ranking;
}

void MoqxRelay::onTrackSelected(
    const FullTrackName& ftn,
    std::shared_ptr<MoQSession> session,
    bool forward
) {
  XLOG(DBG4) << "[MoqxRelay] Track selected: " << ftn << " session=" << session.get()
             << " forward=" << forward;

  if (!session || session->isClosed()) {
    XLOG(DBG4) << "onTrackSelected: session null or closed, skipping " << ftn;
    return;
  }

  auto subIt = subscriptions_.find(ftn);
  if (subIt == subscriptions_.end() || !subIt->second.forwarder) {
    XLOG(DBG4) << "onTrackSelected: no subscription/forwarder for " << ftn;
    return;
  }

  auto exec = session->getExecutor();
  if (!exec) {
    XLOG(ERR) << "onTrackSelected: null executor for session " << session.get();
    return;
  }

  co_withExecutor(
      exec,
      publishToSession(session, subIt->second.forwarder, forward, /*trackFilterSubscriber=*/true)
  )
      .start();
}

void MoqxRelay::onTrackEvicted(const FullTrackName& ftn, std::shared_ptr<MoQSession> session) {
  XLOG(DBG4) << "[MoqxRelay] Track evicted: " << ftn << " session=" << session.get();

  if (!session || session->isClosed()) {
    return;
  }

  auto subIt = subscriptions_.find(ftn);
  if (subIt == subscriptions_.end()) {
    return;
  }

  if (!subIt->second.forwarder) {
    return;
  }
  auto& forwarder = subIt->second.forwarder;
  auto sub = forwarder->getSubscriber(session.get());
  if (!sub || sub->isPinned()) {
    XLOG(DBG4) << "onTrackEvicted: pinned subscriber, skipping";
    return;
  }
  forwarder->removeSubscriber(session, std::nullopt, "onTrackEvicted");
}

} // namespace openmoq::moqx
