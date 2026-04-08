#include "stats/StatsRegistry.h"

#include <algorithm>
#include <folly/Conv.h>
#include <folly/io/Cursor.h>
#include <folly/io/IOBuf.h>
#include <folly/io/IOBufQueue.h>

#include <folly/coro/Collect.h>
#include <folly/coro/Task.h>
#include <folly/experimental/coro/Task.h>
#include <folly/logging/xlog.h>

namespace openmoq::moqx::stats {

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
  app("# HELP moqx_" #name "_total\n"                                                              \
      "# TYPE moqx_" #name "_total counter\n"                                                      \
      "moqx_" #name "_total ");                                                                    \
  appNum(snap.name);                                                                               \
  app("\n\n");
  STATS_COUNTER_FIELDS(EMIT_COUNTER)
#undef EMIT_COUNTER

  // --- Gauges ---
#define EMIT_GAUGE(type, name)                                                                     \
  app("# HELP moqx_" #name "\n"                                                                    \
      "# TYPE moqx_" #name " gauge\n"                                                              \
      "moqx_" #name " ");                                                                          \
  appNum(snap.name);                                                                               \
  app("\n\n");
  STATS_GAUGE_FIELDS(EMIT_GAUGE)
#undef EMIT_GAUGE

  // --- Histograms ---
#define EMIT_HISTOGRAM(name, bounds)                                                               \
  app("# HELP moqx_" #name "_microseconds\n"                                                       \
      "# TYPE moqx_" #name "_microseconds histogram\n");                                           \
  {                                                                                                \
    const auto& bvals = (bounds);                                                                  \
    const auto& bcounts = snap.name##Buckets;                                                      \
    for (size_t i = 0; i < bvals.size(); ++i) {                                                    \
      app("moqx_" #name "_microseconds_bucket{le=\"");                                             \
      appNum(bvals[i]);                                                                            \
      app("\"} ");                                                                                 \
      appNum(bcounts[i]);                                                                          \
      app("\n");                                                                                   \
    }                                                                                              \
    app("moqx_" #name "_microseconds_bucket{le=\"+Inf\"} ");                                       \
    appNum(bcounts.back());                                                                        \
    app("\n");                                                                                     \
  }                                                                                                \
  app("moqx_" #name "_microseconds_sum ");                                                         \
  appNum(snap.name##Sum);                                                                          \
  app("\n"                                                                                         \
      "moqx_" #name "_microseconds_count ");                                                       \
  appNum(snap.name##Count);                                                                        \
  app("\n\n");
  STATS_HISTOGRAM_FIELDS(EMIT_HISTOGRAM)
#undef EMIT_HISTOGRAM

  // --- Per-RequestErrorCode breakdowns ---
  // Each field emits one labelled counter series:
  //   moqx_<name>_by_code_total{code="<label>"}
#define EMIT_ERROR_COUNTER(name)                                                                   \
  app("# HELP moqx_" #name "_by_code_total"                                                        \
      " Error count broken down by RequestErrorCode\n"                                             \
      "# TYPE moqx_" #name "_by_code_total counter\n");                                            \
  for (size_t i = 0; i < kRequestErrorCodeCount; ++i) {                                            \
    app("moqx_" #name "_by_code_total{code=\"");                                                   \
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
  collectors_.emplace_back(collector);
  XLOG(DBG1) << "StatsRegistry: registered collector (total=" << collectors_.size() << ")";
}

folly::coro::Task<StatsSnapshot> StatsRegistry::aggregateAsync() {
  static auto snapshotTask = [](std::shared_ptr<StatsCollectorBase> c
                             ) -> folly::coro::Task<StatsSnapshot> { co_return c->snapshot(); };

  // collectors_ is written only during startup; by the time aggregateAsync()
  // is called it is effectively read-only.
  std::vector<std::shared_ptr<StatsCollectorBase>> live;
  live.reserve(collectors_.size());
  size_t write = 0;
  for (size_t i = 0; i < collectors_.size(); ++i) {
    auto s = collectors_[i].lock();
    if (s) { // if collector is alive, keep it and move to next write slot
      live.push_back(std::move(s));
      collectors_[write++] = collectors_[i];
    }
  }
  collectors_.resize(write);

  std::vector<folly::coro::TaskWithExecutor<StatsSnapshot>> tasks;
  tasks.reserve(live.size());
  for (const auto& c : live) {
    auto* exec = c->owningExecutor();
    if (!exec) {
      continue; // executor not yet bound; skip until next poll
    }
    tasks.push_back(folly::coro::co_withExecutor(exec, snapshotTask(c)));
  }
  auto results = co_await folly::coro::collectAllRange(std::move(tasks));
  StatsSnapshot combined;
  for (auto& snap : results) {
    combined += snap;
  }
  co_return combined;
}

} // namespace openmoq::moqx::stats
