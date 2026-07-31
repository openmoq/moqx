/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "admin/TrackMetricsHandler.h"

#include <algorithm>
#include <chrono>
#include <vector>
#include <folly/CancellationToken.h>
#include <folly/Conv.h>
#include <folly/coro/Task.h>
#include <folly/coro/WithCancellation.h>
#include <folly/io/IOBuf.h>
#include <folly/io/IOBufQueue.h>
#include <folly/io/async/EventBaseManager.h>
#include <folly/logging/xlog.h>
#include <proxygen/httpserver/ResponseBuilder.h>
#include <proxygen/lib/http/HTTPMessage.h>

#include "MoqxRelayContext.h"
#include "SafeTrackName.h"
#include "admin/AdminServer.h"

namespace openmoq::moqx::admin {

namespace {

// Service names come from config, so they only need Prometheus quoting; track
// and namespace labels go through safeName() instead.
std::string escapeLabel(std::string_view value) {
  std::string out;
  out.reserve(value.size());
  for (char c : value) {
    switch (c) {
    case '\\':
      out += "\\\\";
      break;
    case '"':
      out += "\\\"";
      break;
    case '\n':
      out += "\\n";
      break;
    default:
      out += c;
    }
  }
  return out;
}

// TrackClock is steady, so wall-clock output needs an anchor. One anchor per
// response keeps an unchanging timestamp from jittering between scrapes.
struct ClockAnchor {
  stats::TrackClock::time_point steady{stats::TrackClock::now()};
  std::chrono::system_clock::time_point wall{std::chrono::system_clock::now()};

  uint64_t toUnixSeconds(stats::TrackClock::time_point tp) const {
    auto at = wall - std::chrono::duration_cast<std::chrono::system_clock::duration>(steady - tp);
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::seconds>(at.time_since_epoch()).count()
    );
  }
};

std::unique_ptr<folly::IOBuf> formatPrometheus(const MoqxRelayContext::TrackMetricsResult& result) {
  folly::IOBufQueue queue{folly::IOBufQueue::cacheChainLength()};
  folly::io::QueueAppender appender{&queue, 8192};

  auto app = [&appender](std::string_view s) {
    appender.push(reinterpret_cast<const uint8_t*>(s.data()), s.size());
  };
  auto appNum = [&app](auto v) { app(folly::to<std::string>(v)); };

  struct Series {
    std::string_view name;
    std::string_view type;
    std::string_view help;
    uint64_t (*value)(const stats::TrackCounters&, const ClockAnchor&);
  };

  const ClockAnchor anchor;

  static constexpr std::array<Series, 12> kSeries = {{
      {"moqx_track_groups_received_total", "counter", "Groups ingested, counted on group-ID transition", [](const stats::TrackCounters& c, const ClockAnchor&) { return c.groupsReceived; }},
      {"moqx_track_subgroups_received_total", "counter", "Subgroups ingested", [](const stats::TrackCounters& c, const ClockAnchor&) { return c.subgroupsReceived; }},
      {"moqx_track_objects_received_total", "counter", "Objects ingested", [](const stats::TrackCounters& c, const ClockAnchor&) { return c.objectsReceived; }},
      {"moqx_track_datagrams_received_total", "counter", "Objects ingested as datagrams", [](const stats::TrackCounters& c, const ClockAnchor&) { return c.datagramsReceived; }},
      {"moqx_track_bytes_received_total", "counter", "Object payload bytes ingested", [](const stats::TrackCounters& c, const ClockAnchor&) { return c.bytesReceived; }},
      {"moqx_track_groups_sent_total", "counter", "Groups delivered, summed over subscribers", [](const stats::TrackCounters& c, const ClockAnchor&) { return c.groupsSent; }},
      {"moqx_track_subgroups_sent_total", "counter", "Subgroups delivered, summed over subscribers", [](const stats::TrackCounters& c, const ClockAnchor&) { return c.subgroupsSent; }},
      {"moqx_track_objects_sent_total", "counter", "Objects delivered, summed over subscribers", [](const stats::TrackCounters& c, const ClockAnchor&) { return c.objectsSent; }},
      {"moqx_track_datagrams_sent_total", "counter", "Objects delivered as datagrams, summed over subscribers", [](const stats::TrackCounters& c, const ClockAnchor&) { return c.datagramsSent; }},
      {"moqx_track_bytes_sent_total", "counter", "Object payload bytes delivered, summed over subscribers", [](const stats::TrackCounters& c, const ClockAnchor&) { return c.bytesSent; }},
      {"moqx_track_publish_start_timestamp_seconds", "gauge", "Unix time the relay first saw the track", [](const stats::TrackCounters& c, const ClockAnchor& a) { return a.toUnixSeconds(c.publishStart); }},
      {"moqx_track_last_object_timestamp_seconds", "gauge", "Unix time of the most recent ingested object", [](const stats::TrackCounters& c, const ClockAnchor& a) { return a.toUnixSeconds(c.lastObject); }},
  }};

  std::vector<std::string> labels;
  labels.reserve(result.tracks.size());
  for (const auto& entry : result.tracks) {
    labels.push_back(folly::to<std::string>(
        "{service=\"",
        escapeLabel(entry.service),
        "\",namespace=\"",
        safeName(entry.ftn.trackNamespace),
        "\",track=\"",
        safeName(entry.ftn.trackName),
        "\"} "
    ));
  }

  for (const auto& series : kSeries) {
    app("# HELP ");
    app(series.name);
    app(" ");
    app(series.help);
    app("\n# TYPE ");
    app(series.name);
    app(" ");
    app(series.type);
    app("\n");
    for (size_t i = 0; i < result.tracks.size(); ++i) {
      const auto& entry = result.tracks[i];
      // A track with no objects yet has no meaningful timestamp; emitting 0
      // would read as 1970 in any time()-based panel.
      if (series.type == "gauge" && series.value(entry.counters, anchor) == 0) {
        continue;
      }
      app(series.name);
      app(labels[i]);
      appNum(series.value(entry.counters, anchor));
      app("\n");
    }
    app("\n");
  }

  app("# HELP moqx_track_subscribers Current downstream subscribers\n"
      "# TYPE moqx_track_subscribers gauge\n");
  for (size_t i = 0; i < result.tracks.size(); ++i) {
    app("moqx_track_subscribers");
    app(labels[i]);
    appNum(result.tracks[i].counters.subscribers);
    app("\n");
  }
  app("\n");

  return queue.move();
}

void sendError(proxygen::ResponseHandler* downstream, int status, const std::string& message) {
  proxygen::ResponseBuilder(downstream)
      .status(status, proxygen::HTTPMessage::getDefaultReason(status))
      .header("Content-Type", "text/plain; charset=utf-8")
      .body(folly::IOBuf::copyBuffer(message))
      .sendWithEOM();
}

} // namespace

void registerTrackMetricsRoute(
    AdminServer& adminServer,
    std::shared_ptr<MoqxRelayContext> context,
    TrackMetricsLimits limits
) {
  adminServer.addRoute(
      "GET",
      "/metrics/track",
      [context = std::move(context), limits](
          std::unique_ptr<proxygen::HTTPMessage> req,
          std::unique_ptr<folly::IOBuf> /*body*/,
          proxygen::ResponseHandler* downstream,
          folly::CancellationToken cancelToken
      ) {
        // Without this the response is an empty scrape, which reads as "no
        // live tracks" rather than "counting is off".
        if (!limits.enabled) {
          sendError(
              downstream,
              503,
              "per-track metrics are disabled (admin.track_metrics_enabled)\n"
          );
          return;
        }

        auto nsParam = req->getDecodedQueryParam("namespace");
        if (nsParam.empty()) {
          sendError(downstream, 400, "namespace parameter is required\n");
          return;
        }
        // Same form the labels are rendered in, so a scraped value can be
        // pasted straight back into a query.
        auto nsPrefix = parseSafeNamespace(nsParam);
        if (!nsPrefix) {
          sendError(downstream, 400, "namespace is not in the MoQT safe name form\n");
          return;
        }

        size_t limit = std::max<size_t>(1, std::min(limits.defaultLimit, limits.maxLimit));
        if (req->hasQueryParam("limit")) {
          auto parsed = folly::tryTo<size_t>(req->getDecodedQueryParam("limit"));
          if (!parsed.hasValue() || *parsed == 0) {
            sendError(downstream, 400, "limit must be a positive integer\n");
            return;
          }
          limit = std::min(*parsed, limits.maxLimit);
        }

        std::string service = req->getDecodedQueryParam("service");
        std::optional<std::string> track;
        if (req->hasQueryParam("track")) {
          track = parseSafeBytes(req->getDecodedQueryParam("track"));
          if (!track) {
            sendError(downstream, 400, "track is not in the MoQT safe name form\n");
            return;
          }
        }

        folly::coro::co_withCancellation(
            cancelToken,
            folly::coro::co_withExecutor(
                folly::EventBaseManager::get()->getEventBase(),
                [](auto ctx,
                   auto* ds,
                   auto token,
                   std::string service,
                   moxygen::TrackNamespace ns,
                   std::optional<std::string> track,
                   size_t limit) -> folly::coro::Task<void> {
                  if (token.isCancellationRequested()) {
                    co_return;
                  }
                  MoqxRelayContext::TrackMetricsResult result;
                  try {
                    result = co_await ctx->aggregateTrackMetrics(
                        std::move(service),
                        std::move(ns),
                        std::move(track),
                        limit
                    );
                  } catch (const std::exception& e) {
                    XLOG(ERR) << "TrackMetricsHandler: aggregateTrackMetrics threw: " << e.what();
                    if (!token.isCancellationRequested()) {
                      sendError(ds, 500, "internal error\n");
                    }
                    co_return;
                  }
                  if (token.isCancellationRequested()) {
                    co_return;
                  }
                  // Truncating would hand Prometheus an arbitrary, unstable
                  // subset of series, so reject instead.
                  if (result.matched > limit) {
                    sendError(
                        ds,
                        400,
                        folly::to<std::string>(
                            "matched ",
                            result.matched,
                            " tracks, exceeds limit=",
                            limit,
                            "; narrow the namespace or raise limit\n"
                        )
                    );
                    co_return;
                  }
                  proxygen::ResponseBuilder(ds)
                      .status(200, proxygen::HTTPMessage::getDefaultReason(200))
                      .header("Content-Type", "text/plain; version=0.0.4; charset=utf-8")
                      .body(formatPrometheus(result))
                      .sendWithEOM();
                }(context,
                  downstream,
                  cancelToken,
                  std::move(service),
                  std::move(*nsPrefix),
                  std::move(track),
                  limit)
            )
        )
            .start();
      }
  );
}

} // namespace openmoq::moqx::admin
