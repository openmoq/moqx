/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "stats/MoQStatsCollector.h"

#include <folly/logging/xlog.h>

namespace openmoq::moqx::stats {

/* static */
std::shared_ptr<MoQStatsCollector>
MoQStatsCollector::create_moq_stats_collector(std::shared_ptr<StatsRegistry> registry) {
  auto collector = std::shared_ptr<MoQStatsCollector>(new MoQStatsCollector());

  // Build aliased shared_ptrs: they share the parent's refcount but point to
  // the value-member inner objects.  Holding only an inner callback extends the
  // parent's lifetime.
  collector->pubCallbackPtr_ =
      std::shared_ptr<moxygen::MoQPublisherStatsCallback>(collector, &collector->pubCallback_);
  collector->subCallbackPtr_ =
      std::shared_ptr<moxygen::MoQSubscriberStatsCallback>(collector, &collector->subCallback_);

  registry->registerCollector(collector);
  return collector;
}

MoQStatsCollector::MoQStatsCollector() : pubCallback_(*this), subCallback_(*this) {}

void MoQStatsCollector::setExecutor(folly::Executor* executor) {
  owningExecutor_.store(executor, std::memory_order_relaxed);
}

// --- StatsCollectorBase ---

StatsSnapshot MoQStatsCollector::snapshot() const {
  StatsSnapshot snap;

#define COPY_FIELD(type, name) snap.name = name##_;
  STATS_MOQ_COUNTER_FIELDS(COPY_FIELD)
  STATS_GAUGE_FIELDS(COPY_FIELD)
#undef COPY_FIELD

#define COPY_HISTOGRAM(name, bounds)                                                               \
  name##_.fillCumulative(snap.name##Buckets);                                                      \
  snap.name##Sum = name##_.sum;                                                                    \
  snap.name##Count = name##_.count;
  STATS_HISTOGRAM_FIELDS(COPY_HISTOGRAM)
#undef COPY_HISTOGRAM

#define COPY_ERROR_ARRAY(name)                                                                     \
  for (size_t i = 0; i < kRequestErrorCodeCount; ++i) {                                            \
    snap.name##ByCodes[i] = name##ByCodes_[i];                                                     \
  }
  STATS_ERROR_COUNTER_FIELDS(COPY_ERROR_ARRAY)
#undef COPY_ERROR_ARRAY

  return snap;
}

folly::Executor* MoQStatsCollector::owningExecutor() const {
  return owningExecutor_.load(std::memory_order_relaxed);
}

std::shared_ptr<moxygen::MoQPublisherStatsCallback> MoQStatsCollector::publisherCallback() const {
  return pubCallbackPtr_;
}

std::shared_ptr<moxygen::MoQSubscriberStatsCallback> MoQStatsCollector::subscriberCallback() const {
  return subCallbackPtr_;
}

// --- Session lifecycle ---

void MoQStatsCollector::onSessionStart() {
  ++moqActiveSessions_;
}

void MoQStatsCollector::onSessionEnd() {
  --moqActiveSessions_;
}

// --- PublisherCallback implementations ---
// Each method corresponds to an event from the relay's publisher role

void MoQStatsCollector::PublisherCallback::onSubscribeSuccess() {
  // Relay accepted a downstream subscriber's SUBSCRIBE request.
  ++parent_.pubSubscribeSuccess_;
  ++parent_.pubActiveSubscriptions_;
}

void MoQStatsCollector::PublisherCallback::onSubscribeError(moxygen::SubscribeErrorCode errorCode) {
  ++parent_.pubSubscribeError_;
  ++parent_.pubSubscribeErrorByCodes_[requestErrorCodeIndex(errorCode)];
}

void MoQStatsCollector::PublisherCallback::onFetchSuccess() {
  ++parent_.pubFetchSuccess_;
}

void MoQStatsCollector::PublisherCallback::onFetchError(moxygen::FetchErrorCode errorCode) {
  ++parent_.pubFetchError_;
  ++parent_.pubFetchErrorByCodes_[requestErrorCodeIndex(errorCode)];
}

void MoQStatsCollector::PublisherCallback::onPublishNamespaceSuccess() {
  ++parent_.pubPublishNamespaceSuccess_;
}

void MoQStatsCollector::PublisherCallback::onPublishNamespaceError(
    moxygen::PublishNamespaceErrorCode errorCode
) {
  ++parent_.pubPublishNamespaceError_;
  ++parent_.pubPublishNamespaceErrorByCodes_[requestErrorCodeIndex(errorCode)];
}

void MoQStatsCollector::PublisherCallback::onPublishNamespaceDone() {
  ++parent_.pubPublishNamespaceDone_;
}

void MoQStatsCollector::PublisherCallback::onPublishNamespaceCancel() {
  ++parent_.pubPublishNamespaceCancel_;
}

void MoQStatsCollector::PublisherCallback::onSubscribeNamespaceSuccess() {
  ++parent_.pubSubscribeNamespaceSuccess_;
}

void MoQStatsCollector::PublisherCallback::onSubscribeNamespaceError(
    moxygen::SubscribeNamespaceErrorCode errorCode
) {
  ++parent_.pubSubscribeNamespaceError_;
  ++parent_.pubSubscribeNamespaceErrorByCodes_[requestErrorCodeIndex(errorCode)];
}

void MoQStatsCollector::PublisherCallback::onUnsubscribeNamespace() {
  ++parent_.pubUnsubscribeNamespace_;
}

void MoQStatsCollector::PublisherCallback::onTrackStatus() {
  ++parent_.pubTrackStatus_;
}

void MoQStatsCollector::PublisherCallback::onUnsubscribe() {
  // Downstream subscriber unsubscribed.
  // moxygen always follows an UNSUBSCRIBE with a PUBLISH_DONE (via terminatePublish),
}

void MoQStatsCollector::PublisherCallback::
    onPublishDone(moxygen::PublishDoneStatusCode /*statusCode*/) {
  // Relay (as publisher) sent PUBLISH_DONE to a downstream subscriber.
  // This fires for both client-initiated unsubscribes and server-initiated teardowns,
  // making it the single authoritative end-of-subscription signal.
  ++parent_.pubPublishDone_;
  --parent_.pubActiveSubscriptions_;
}

void MoQStatsCollector::PublisherCallback::onRequestUpdate() {
  ++parent_.pubRequestUpdate_;
}

void MoQStatsCollector::PublisherCallback::onSubscriptionStreamOpened() {
  ++parent_.pubSubscriptionStreamOpened_;
  ++parent_.pubActiveSubscriptionStreams_;
}

void MoQStatsCollector::PublisherCallback::onSubscriptionStreamClosed() {
  ++parent_.pubSubscriptionStreamClosed_;
  --parent_.pubActiveSubscriptionStreams_;
}

void MoQStatsCollector::PublisherCallback::recordPublishNamespaceLatency(uint64_t latencyMsec) {
  parent_.moqPublishNamespaceLatency_.addValue(latencyMsec);
}

void MoQStatsCollector::PublisherCallback::recordPublishLatency(uint64_t latencyMsec) {
  parent_.moqPublishLatency_.addValue(latencyMsec);
}

void MoQStatsCollector::PublisherCallback::onPublishError(moxygen::PublishErrorCode errorCode) {
  // Relay sent PUBLISH; received PUBLISH_ERROR back.
  ++parent_.moqPublishError_;
  ++parent_.moqPublishErrorByCodes_[requestErrorCodeIndex(errorCode)];
}

void MoQStatsCollector::PublisherCallback::onPublishSuccess() {
  // Relay sent PUBLISH; received PUBLISH_OK back.
  ++parent_.moqPublishSuccess_;
}

// --- SubscriberCallback implementations ---
// Each method corresponds to an event from the relay's subscriber role

void MoQStatsCollector::SubscriberCallback::onSubscribeSuccess() {
  // Relay's outgoing SUBSCRIBE was accepted by upstream.
  ++parent_.subSubscribeSuccess_;
  ++parent_.subActivePublishers_;
}

void MoQStatsCollector::SubscriberCallback::onSubscribeError(moxygen::SubscribeErrorCode errorCode
) {
  ++parent_.subSubscribeError_;
  ++parent_.subSubscribeErrorByCodes_[requestErrorCodeIndex(errorCode)];
  --parent_.subActivePublishers_;
}

void MoQStatsCollector::SubscriberCallback::onFetchSuccess() {
  ++parent_.subFetchSuccess_;
}

void MoQStatsCollector::SubscriberCallback::onFetchError(moxygen::FetchErrorCode errorCode) {
  ++parent_.subFetchError_;
  ++parent_.subFetchErrorByCodes_[requestErrorCodeIndex(errorCode)];
}

void MoQStatsCollector::SubscriberCallback::onPublishNamespaceSuccess() {
  ++parent_.subPublishNamespaceSuccess_;
}

void MoQStatsCollector::SubscriberCallback::onPublishNamespaceError(
    moxygen::PublishNamespaceErrorCode errorCode
) {
  ++parent_.subPublishNamespaceError_;
  ++parent_.subPublishNamespaceErrorByCodes_[requestErrorCodeIndex(errorCode)];
}

void MoQStatsCollector::SubscriberCallback::onPublishNamespaceDone() {
  ++parent_.subPublishNamespaceDone_;
}

void MoQStatsCollector::SubscriberCallback::onPublishNamespaceCancel() {
  ++parent_.subPublishNamespaceCancel_;
}

void MoQStatsCollector::SubscriberCallback::onSubscribeNamespaceSuccess() {
  ++parent_.subSubscribeNamespaceSuccess_;
}

void MoQStatsCollector::SubscriberCallback::onSubscribeNamespaceError(
    moxygen::SubscribeNamespaceErrorCode errorCode
) {
  ++parent_.subSubscribeNamespaceError_;
  ++parent_.subSubscribeNamespaceErrorByCodes_[requestErrorCodeIndex(errorCode)];
}

void MoQStatsCollector::SubscriberCallback::onUnsubscribeNamespace() {
  ++parent_.subUnsubscribeNamespace_;
}

void MoQStatsCollector::SubscriberCallback::onTrackStatus() {
  ++parent_.subTrackStatus_;
}

void MoQStatsCollector::SubscriberCallback::onUnsubscribe() {
  // Relay unsubscribed from upstream.
  --parent_.subActivePublishers_;
}

void MoQStatsCollector::SubscriberCallback::
    onPublishDone(moxygen::PublishDoneStatusCode /*statusCode*/) {
  // Relay (as subscriber) received PUBLISH_DONE from upstream publisher.
  ++parent_.subPublishDone_;
  --parent_.subActivePublishers_;
}

void MoQStatsCollector::SubscriberCallback::onRequestUpdate() {
  ++parent_.subRequestUpdate_;
}

void MoQStatsCollector::SubscriberCallback::onSubscriptionStreamOpened() {
  ++parent_.subSubscriptionStreamOpened_;
  ++parent_.subActiveSubscriptionStreams_;
}

void MoQStatsCollector::SubscriberCallback::onSubscriptionStreamClosed() {
  ++parent_.subSubscriptionStreamClosed_;
  --parent_.subActiveSubscriptionStreams_;
}

void MoQStatsCollector::SubscriberCallback::recordSubscribeLatency(uint64_t latencyMsec) {
  parent_.moqSubscribeLatency_.addValue(latencyMsec);
}

void MoQStatsCollector::SubscriberCallback::recordFetchLatency(uint64_t latencyMsec) {
  parent_.moqFetchLatency_.addValue(latencyMsec);
}

void MoQStatsCollector::SubscriberCallback::onPublish() {
  // Relay received a PUBLISH request from an upstream publisher.
  ++parent_.moqPublishReceived_;
}

void MoQStatsCollector::SubscriberCallback::onPublishOk() {
  // Relay sent PUBLISH_OK to an upstream publisher; it is now active.
  ++parent_.moqPublishOkSent_;
  ++parent_.subActivePublishers_;
}

void MoQStatsCollector::SubscriberCallback::onPublishError(moxygen::PublishErrorCode errorCode) {
  // Relay rejected an upstream publisher with PUBLISH_ERROR.
  ++parent_.subPublishError_;
  ++parent_.subPublishErrorByCodes_[requestErrorCodeIndex(errorCode)];
}

} // namespace openmoq::moqx::stats
