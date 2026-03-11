#include <o_rly/stats/MoQStatsCollector.h>

#include <folly/logging/xlog.h>

namespace openmoq::o_rly::stats {

MoQStatsCollector::MoQStatsCollector(
    folly::Executor::KeepAlive<> owningExecutor,
    std::shared_ptr<StatsRegistry> registry)
    : owningExecutor_(std::move(owningExecutor)),
      registry_(registry) {}

MoQStatsCollector::~MoQStatsCollector() {
  if (auto registry = registry_.lock()) {
    registry->deregisterCollector(this);
  }
}


StatsSnapshot MoQStatsCollector::snapshot() const {
  StatsSnapshot snap;

  // Copy scalars
#define COPY_FIELD(type, name) snap.name = name##_;
  STATS_COUNTER_FIELDS(COPY_FIELD)
  STATS_GAUGE_FIELDS(COPY_FIELD)
#undef COPY_FIELD

  // Copy histogram bucket cumulative counts
#define COPY_HISTOGRAM(name, bounds)                              \
  name##_.fillCumulative(snap.name##Buckets);                     \
  snap.name##Sum = name##_.sum;                                   \
  snap.name##Count = name##_.count;
  STATS_HISTOGRAM_FIELDS(COPY_HISTOGRAM)
#undef COPY_HISTOGRAM

  return snap;
}

folly::Executor::KeepAlive<> MoQStatsCollector::owningExecutor() const {
  return owningExecutor_;
}

void MoQStatsCollector::onSubscribeSuccess() {
  ++moqSubscribeSuccess_;
  ++moqActiveSubscriptions_;
}

void MoQStatsCollector::onSubscribeError(
    moxygen::SubscribeErrorCode /*errorCode*/) {
  ++moqSubscribeError_;
}

void MoQStatsCollector::onFetchSuccess() {
  ++moqFetchSuccess_;
}

void MoQStatsCollector::onFetchError(moxygen::FetchErrorCode /*errorCode*/) {
  ++moqFetchError_;
}

void MoQStatsCollector::onPublishNamespaceSuccess() {
  ++moqPublishNamespaceSuccess_;
  ++moqActivePublishNamespaces_;
}

void MoQStatsCollector::onPublishNamespaceError(
    moxygen::PublishNamespaceErrorCode /*errorCode*/) {
  ++moqPublishNamespaceError_;
}

void MoQStatsCollector::onPublishNamespaceDone() {
  ++moqPublishNamespaceDone_;
  --moqActivePublishNamespaces_;
}

void MoQStatsCollector::onPublishNamespaceCancel() {
  ++moqPublishNamespaceCancel_;
  --moqActivePublishNamespaces_;
}

void MoQStatsCollector::onSubscribeNamespaceSuccess() {
  ++moqSubscribeNamespaceSuccess_;
  ++moqActiveSubscribeNamespaces_;
}

void MoQStatsCollector::onSubscribeNamespaceError(
    moxygen::SubscribeNamespaceErrorCode /*errorCode*/) {
  ++moqSubscribeNamespaceError_;
}

void MoQStatsCollector::onUnsubscribeNamespace() {
  ++moqUnsubscribeNamespace_;
  --moqActiveSubscribeNamespaces_;
}

void MoQStatsCollector::onTrackStatus() {
  ++moqTrackStatus_;
}

void MoQStatsCollector::onUnsubscribe() {
  --moqActiveSubscriptions_;
}

void MoQStatsCollector::onPublishDone(
    moxygen::PublishDoneStatusCode /*statusCode*/) {
  ++moqPublishDone_;
  --moqActivePublishers_;
}

void MoQStatsCollector::onRequestUpdate() {
  ++moqRequestUpdate_;
}

void MoQStatsCollector::onSubscriptionStreamOpened() {
  ++moqSubscriptionStreamOpened_;
  ++moqActiveSubscriptionStreams_;
}

void MoQStatsCollector::onSubscriptionStreamClosed() {
  ++moqSubscriptionStreamClosed_;
  --moqActiveSubscriptionStreams_;
}

void MoQStatsCollector::recordPublishNamespaceLatency(
    uint64_t latencyMsec) {
  moqPublishNamespaceLatency_.addValue(latencyMsec);
}

void MoQStatsCollector::recordPublishLatency(uint64_t latencyMsec) {
  moqPublishLatency_.addValue(latencyMsec);
}

void MoQStatsCollector::onPublishError(
    moxygen::PublishErrorCode /*errorCode*/) {
  ++moqPublishError_;
}

void MoQStatsCollector::onPublishSuccess() {
  ++moqPublishSuccess_;
  ++moqActivePublishers_;
}

void MoQStatsCollector::recordSubscribeLatency(uint64_t latencyMsec) {
  moqSubscribeLatency_.addValue(latencyMsec);
}

void MoQStatsCollector::recordFetchLatency(uint64_t latencyMsec) {
  moqFetchLatency_.addValue(latencyMsec);
}

void MoQStatsCollector::onPublish() {
  ++moqPublishReceived_;
}

void MoQStatsCollector::onPublishOk() {
  ++moqPublishOkReceived_;
}

} // namespace openmoq::o_rly::stats
