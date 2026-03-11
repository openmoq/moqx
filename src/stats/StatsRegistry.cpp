#include <o_rly/stats/StatsRegistry.h>

#include <sstream>
#include <string_view>

#include <folly/experimental/coro/Collect.h>
#include <folly/experimental/coro/Task.h>
#include <folly/logging/xlog.h>

namespace openmoq::o_rly::stats {


StatsSnapshot& StatsSnapshot::operator+=(const StatsSnapshot& o) {
#define ADD_FIELD(type, name) name += o.name;
  STATS_COUNTER_FIELDS(ADD_FIELD)
  STATS_GAUGE_FIELDS(ADD_FIELD)
  // Note: STATS_GLOBAL_GAUGE_FIELDS are NOT added — they are assigned, not summed
#undef ADD_FIELD

#define ADD_HISTOGRAM(name, bounds)                                       \
  for (size_t i = 0; i < name##Buckets.size(); ++i) {                    \
    name##Buckets[i] += o.name##Buckets[i];                               \
  }                                                                       \
  name##Sum += o.name##Sum;                                               \
  name##Count += o.name##Count;
  STATS_HISTOGRAM_FIELDS(ADD_HISTOGRAM)
#undef ADD_HISTOGRAM

  return *this;
}

// ---------------------------------------------------------------------------
// StatsSnapshot::formatPrometheus
//
// Prometheus exposition format v0.0.4:
//   https://prometheus.io/docs/instrumenting/exposition_formats/
//
// Counters  → _total suffix, TYPE counter
// Gauges    → no suffix,     TYPE gauge
// Histograms → _bucket{le=...}/_sum/_count, TYPE histogram
// ---------------------------------------------------------------------------

/* static */
std::string StatsSnapshot::formatPrometheus(const StatsSnapshot& snap) {
  std::ostringstream out;

  // --- Counters --------------------------------------------------------------
#define EMIT_COUNTER(type, name)                                \
  out << "# HELP orly_" #name "_total\n"                       \
      << "# TYPE orly_" #name "_total counter\n"               \
      << "orly_" #name "_total " << snap.name << "\n\n";
  STATS_COUNTER_FIELDS(EMIT_COUNTER)
#undef EMIT_COUNTER

  // --- Gauges ----------------------------------------------------------------
#define EMIT_GAUGE(type, name)                                  \
  out << "# HELP orly_" #name "\n"                             \
      << "# TYPE orly_" #name " gauge\n"                       \
      << "orly_" #name " " << snap.name << "\n\n";
  STATS_GAUGE_FIELDS(EMIT_GAUGE)
#undef EMIT_GAUGE

  // --- Global Gauges ---------------------------------------------------------
#define EMIT_GLOBAL_GAUGE(type, name)                           \
  out << "# HELP orly_" #name "\n"                             \
      << "# TYPE orly_" #name " gauge\n"                       \
      << "orly_" #name " " << snap.name << "\n\n";
  STATS_GLOBAL_GAUGE_FIELDS(EMIT_GLOBAL_GAUGE)
#undef EMIT_GLOBAL_GAUGE

  // --- Histograms ------------------------------------------------------------
  // TODO: emit per-boundary _bucket{le="..."} lines by iterating kBoundsRef
  //       alongside the name##Buckets array.
  // For now emit just _sum, _count, and +Inf bucket as a minimal scaffold.
#define EMIT_HISTOGRAM(name, bounds)                                              \
  out << "# HELP orly_" #name "_microseconds\n"                                  \
      << "# TYPE orly_" #name "_microseconds histogram\n";                       \
  {                                                                               \
    const auto& bvals = (bounds);                                                 \
    const auto& bcounts = snap.name##Buckets;                                    \
    for (size_t i = 0; i < bvals.size(); ++i) {                                  \
      out << "orly_" #name "_microseconds_bucket{le=\"" << bvals[i]              \
          << "\"} " << bcounts[i] << "\n";                                        \
    }                                                                             \
    out << "orly_" #name "_microseconds_bucket{le=\"+Inf\"} "                    \
        << bcounts.back() << "\n";                                                \
  }                                                                               \
  out << "orly_" #name "_microseconds_sum "   << snap.name##Sum   << "\n"        \
      << "orly_" #name "_microseconds_count " << snap.name##Count << "\n\n";
  STATS_HISTOGRAM_FIELDS(EMIT_HISTOGRAM)
#undef EMIT_HISTOGRAM

  return out.str();
}


void StatsRegistry::registerCollector(
    std::shared_ptr<StatsCollectorBase> collector) {
  std::lock_guard lock(mu_);
  collectors_.push_back(std::move(collector));
  XLOG(DBG1) << "StatsRegistry: registered collector (total="
             << collectors_.size() << ")";
}

void StatsRegistry::deregisterCollector(StatsCollectorBase* collector) {
  std::lock_guard lock(mu_);
  auto it = std::find_if(
      collectors_.begin(),
      collectors_.end(),
      [collector](const auto& ptr) { return ptr.get() == collector; });
  if (it != collectors_.end()) {
    collectors_.erase(it);
  }
  XLOG(DBG1) << "StatsRegistry: deregistered collector (total="
             << collectors_.size() << ")";
}

void StatsRegistry::onNewSession() {
  std::lock_guard lock(mu_);
  ++moqActiveSessions_;
}

void StatsRegistry::onTerminateSession() {
  std::lock_guard lock(mu_);
  --moqActiveSessions_;
}

folly::coro::Task<StatsSnapshot> StatsRegistry::aggregateAsync() {
  // --- Copy collector list and global gauges, release lock ---
  std::vector<std::shared_ptr<StatsCollectorBase>> collectorsCopy;
  StatsSnapshot combined;
  {
    std::lock_guard lock(mu_);
    collectorsCopy = collectors_;
    // Copy global gauges directly into the result snapshot
#define COPY_GLOBAL_GAUGE(type, name) combined.name = name##_;
    STATS_GLOBAL_GAUGE_FIELDS(COPY_GLOBAL_GAUGE)
#undef COPY_GLOBAL_GAUGE
  }

  // --- Aggregate collectors ---
  // TODO: replace with folly::coro::collectAll to schedule each snapshot()
  // on its owningExecutor() in parallel, instead of sequentially here when multi-threaded
  // relay is introduced.
  for (const auto& c : collectorsCopy) {
    combined += c->snapshot();
  }

  co_return combined;
}

} // namespace openmoq::o_rly::stats
