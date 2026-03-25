#include <o_rly/stats/StatsRegistry.h>

#include <folly/Conv.h>
#include <folly/io/Cursor.h>
#include <folly/io/IOBuf.h>
#include <folly/io/IOBufQueue.h>

#include <folly/coro/Collect.h>
#include <folly/coro/Task.h>
#include <folly/experimental/coro/Task.h>
#include <folly/logging/xlog.h>

namespace openmoq::o_rly::stats {

StatsSnapshot& StatsSnapshot::operator+=(const StatsSnapshot& o) {
#define ADD_FIELD(type, name) name += o.name;
  STATS_COUNTER_FIELDS(ADD_FIELD)
  STATS_GAUGE_FIELDS(ADD_FIELD)
#undef ADD_FIELD

#define ADD_HISTOGRAM(name, bounds)                                                                \
  for (size_t i = 0; i < name##Buckets.size(); ++i) {                                              \
    name##Buckets[i] += o.name##Buckets[i];                                                        \
  }                                                                                                \
  name##Sum += o.name##Sum;                                                                        \
  name##Count += o.name##Count;
  STATS_HISTOGRAM_FIELDS(ADD_HISTOGRAM)
#undef ADD_HISTOGRAM

#define ADD_ERROR_ARRAY(name)                                                                      \
  for (size_t i = 0; i < kRequestErrorCodeCount; ++i) {                                            \
    name##ByCodes[i] += o.name##ByCodes[i];                                                        \
  }
  STATS_ERROR_COUNTER_FIELDS(ADD_ERROR_ARRAY)
#undef ADD_ERROR_ARRAY

  return *this;
}

// StatsSnapshot::formatPrometheus
//
// Prometheus exposition format v0.0.4:
//   https://prometheus.io/docs/instrumenting/exposition_formats/
//
// Counters  → _total suffix, TYPE counter
// Gauges    → no suffix,     TYPE gauge
// Histograms → _bucket{le=...}/_sum/_count, TYPE histogram

/* static */
std::unique_ptr<folly::IOBuf> StatsSnapshot::formatPrometheus(const StatsSnapshot& snap) {
  folly::IOBufQueue queue{folly::IOBufQueue::cacheChainLength()};
  folly::io::QueueAppender appender{&queue, 8192};

  auto app = [&appender](std::string_view s) {
    appender.push(reinterpret_cast<const uint8_t*>(s.data()), s.size());
  };
  auto appNum = [&app](auto v) { app(folly::to<std::string>(v)); };

  // --- Counters ---
#define EMIT_COUNTER(type, name)                                                                   \
  app("# HELP orly_" #name "_total\n"                                                              \
      "# TYPE orly_" #name "_total counter\n"                                                      \
      "orly_" #name "_total ");                                                                    \
  appNum(snap.name);                                                                               \
  app("\n\n");
  STATS_COUNTER_FIELDS(EMIT_COUNTER)
#undef EMIT_COUNTER

  // --- Gauges ---
#define EMIT_GAUGE(type, name)                                                                     \
  app("# HELP orly_" #name "\n"                                                                    \
      "# TYPE orly_" #name " gauge\n"                                                              \
      "orly_" #name " ");                                                                          \
  appNum(snap.name);                                                                               \
  app("\n\n");
  STATS_GAUGE_FIELDS(EMIT_GAUGE)
#undef EMIT_GAUGE

  // --- Histograms ---
#define EMIT_HISTOGRAM(name, bounds)                                                               \
  app("# HELP orly_" #name "_microseconds\n"                                                       \
      "# TYPE orly_" #name "_microseconds histogram\n");                                           \
  {                                                                                                \
    const auto& bvals = (bounds);                                                                  \
    const auto& bcounts = snap.name##Buckets;                                                      \
    for (size_t i = 0; i < bvals.size(); ++i) {                                                    \
      app("orly_" #name "_microseconds_bucket{le=\"");                                             \
      appNum(bvals[i]);                                                                            \
      app("\"} ");                                                                                 \
      appNum(bcounts[i]);                                                                          \
      app("\n");                                                                                   \
    }                                                                                              \
    app("orly_" #name "_microseconds_bucket{le=\"+Inf\"} ");                                       \
    appNum(bcounts.back());                                                                        \
    app("\n");                                                                                     \
  }                                                                                                \
  app("orly_" #name "_microseconds_sum ");                                                         \
  appNum(snap.name##Sum);                                                                          \
  app("\n"                                                                                         \
      "orly_" #name "_microseconds_count ");                                                       \
  appNum(snap.name##Count);                                                                        \
  app("\n\n");
  STATS_HISTOGRAM_FIELDS(EMIT_HISTOGRAM)
#undef EMIT_HISTOGRAM

  // --- Per-RequestErrorCode breakdowns ---
  // Each field emits one labelled counter series:
  //   orly_<name>_by_code_total{code="<label>"}
#define EMIT_ERROR_COUNTER(name)                                                                   \
  app("# HELP orly_" #name "_by_code_total"                                                        \
      " Error count broken down by RequestErrorCode\n"                                             \
      "# TYPE orly_" #name "_by_code_total counter\n");                                            \
  for (size_t i = 0; i < kRequestErrorCodeCount; ++i) {                                            \
    app("orly_" #name "_by_code_total{code=\"");                                                   \
    app(kRequestErrorCodeLabels[i]);                                                               \
    app("\"} ");                                                                                   \
    appNum(snap.name##ByCodes[i]);                                                                 \
    app("\n");                                                                                     \
  }                                                                                                \
  app("\n");
  STATS_ERROR_COUNTER_FIELDS(EMIT_ERROR_COUNTER)
#undef EMIT_ERROR_COUNTER

  return queue.move();
}

void StatsRegistry::registerCollector(std::shared_ptr<StatsCollectorBase> collector) {
  if (locked_) {
    XLOG(FATAL) << "StatsRegistry: registration attempted after lock()";
  }
  collectors_.push_back(std::move(collector));
  XLOG(DBG1) << "StatsRegistry: registered collector (total=" << collectors_.size() << ")";
}

void StatsRegistry::lock() {
  locked_ = true;
  XLOG(DBG1) << "StatsRegistry: locked registration";
}
void StatsRegistry::deregisterCollector(StatsCollectorBase* collector) {
  auto it = std::find_if(collectors_.begin(), collectors_.end(), [collector](const auto& ptr) {
    return ptr.get() == collector;
  });
  if (it != collectors_.end()) {
    collectors_.erase(it);
  }
  XLOG(DBG1) << "StatsRegistry: deregistered collector (total=" << collectors_.size() << ")";
}

folly::coro::Task<StatsSnapshot> StatsRegistry::aggregateAsync() {
  static auto snapshotTask = [](std::shared_ptr<StatsCollectorBase> c
                             ) -> folly::coro::Task<StatsSnapshot> { co_return c->snapshot(); };

  std::vector<std::shared_ptr<StatsCollectorBase>> copy = collectors_;
  std::vector<folly::coro::TaskWithExecutor<StatsSnapshot>> tasks;
  tasks.reserve(copy.size());
  for (const auto& c : copy) {
    tasks.push_back(folly::coro::co_withExecutor(c->owningExecutor(), snapshotTask(c)));
  }
  auto results = co_await folly::coro::collectAllRange(std::move(tasks));
  StatsSnapshot combined;
  for (auto& snap : results) {
    combined += snap;
  }
  co_return combined;
}

} // namespace openmoq::o_rly::stats
