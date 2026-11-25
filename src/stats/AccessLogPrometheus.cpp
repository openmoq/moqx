/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "stats/AccessLogPrometheus.h"

#include <string>
#include <utility>
#include <vector>

#include "stats/PrometheusFormat.h"

namespace openmoq::moqx::stats {

namespace {

// Collects the stats so that each series can be written with all of its samples together.
class Collector : public logging::AccessLogStatsVisitor {
public:
  void onSink(std::string_view name, const logging::AccessLogSinkStats& stats) override {
    sinks.emplace_back(std::string(name), stats);
  }
  void onQueue(size_t queueDepth, uint64_t queueDropped, double oldestAgeSeconds) override {
    depth = queueDepth;
    dropped = queueDropped;
    oldestAge = oldestAgeSeconds;
  }

  std::vector<std::pair<std::string, logging::AccessLogSinkStats>> sinks;
  size_t depth{0};
  uint64_t dropped{0};
  double oldestAge{0};
};

} // namespace

folly::coro::Task<std::unique_ptr<folly::IOBuf>>
accessLogPrometheusText(logging::MoqxAccessLog& accessLog, bool omitMetadata) {
  Collector c;
  co_await accessLog.visitStats(c);

  PrometheusWriter w(omitMetadata);
  auto perSink = [&](std::string_view name, std::string_view help, auto field) {
    w.header(name, "counter", help);
    for (const auto& [sink, stats] : c.sinks) {
      w.append(name);
      w.append("{sink=\"");
      w.append(escapeLabelValue(sink));
      w.append("\"} ");
      w.num(field(stats));
      w.append("\n");
    }
  };
  auto scalar = [&](std::string_view name, std::string_view type, std::string_view help, auto v) {
    w.header(name, type, help);
    w.append(name);
    w.append(" ");
    w.num(v);
    w.append("\n");
  };
  perSink("moqx_access_log_records_total", "Lines the sink wrote.", [](const auto& s) {
    return s.records;
  });
  perSink(
      "moqx_access_log_dropped_total",
      "Lines the sink encoded and then lost to its retry backlog or shutdown.",
      [](const auto& s) { return s.dropped; }
  );
  perSink("moqx_access_log_write_errors_total", "Failed writes.", [](const auto& s) {
    return s.writeErrors;
  });
  scalar(
      "moqx_access_log_queue_dropped_total",
      "counter",
      "Records dropped before any sink saw them: the queue was full or the writer had stopped.",
      c.dropped
  );
  scalar("moqx_access_log_queue_depth", "gauge", "Records waiting for the writer.", c.depth);
  scalar(
      "moqx_access_log_oldest_record_age_seconds",
      "gauge",
      "Age of the oldest record waiting for the writer.",
      c.oldestAge
  );
  co_return w.move();
}

} // namespace openmoq::moqx::stats
