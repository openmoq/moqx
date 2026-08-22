/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "admin/TrackMetricsHandler.h"

#include <algorithm>
#include <chrono>
#include <folly/CancellationToken.h>
#include <folly/Conv.h>
#include <folly/Format.h>
#include <folly/coro/Task.h>
#include <folly/coro/WithCancellation.h>
#include <folly/io/IOBuf.h>
#include <folly/io/IOBufQueue.h>
#include <folly/io/async/EventBaseManager.h>
#include <folly/logging/xlog.h>
#include <proxygen/httpserver/ResponseBuilder.h>
#include <proxygen/lib/http/HTTPMessage.h>
#include <vector>

#include "MoqxRelayContext.h"
#include "SafeTrackName.h"
#include "admin/AdminResponse.h"
#include "admin/AdminServer.h"
#include "stats/PrometheusFormat.h"

namespace openmoq::moqx::admin {

namespace {

// TrackClock is steady, so wall-clock output needs an anchor. One anchor per
// response keeps an unchanging timestamp from jittering between scrapes.
struct ClockAnchor {
  stats::TrackClock::time_point steady{stats::TrackClock::now()};
  std::chrono::system_clock::time_point wall{std::chrono::system_clock::now()};

  int64_t toUnixMillis(stats::TrackClock::time_point tp) const {
    auto at = wall - std::chrono::duration_cast<std::chrono::system_clock::duration>(steady - tp);
    return std::chrono::duration_cast<std::chrono::milliseconds>(at.time_since_epoch()).count();
  }
};

} // namespace

std::unique_ptr<folly::IOBuf>
formatTrackMetrics(const MoqxRelayContext::TrackMetricsResult& result, bool omitMetadata) {
  stats::PrometheusWriter out{omitMetadata};

  struct Counter {
    std::string_view name;
    std::string_view help;
    uint64_t (*value)(const stats::TrackCounters&);
  };

  static constexpr std::array<Counter, 10> kCounters = {{
      {"moqx_track_groups_received_total",
       "Groups ingested, counted on group-ID transition",
       [](const stats::TrackCounters& c) { return c.received.groups; }},
      {"moqx_track_subgroups_received_total",
       "Subgroups ingested",
       [](const stats::TrackCounters& c) { return c.received.subgroups; }},
      {"moqx_track_objects_received_total",
       "Objects ingested",
       [](const stats::TrackCounters& c) { return c.received.objects; }},
      {"moqx_track_datagrams_received_total",
       "Objects ingested as datagrams",
       [](const stats::TrackCounters& c) { return c.received.datagrams; }},
      {"moqx_track_bytes_received_total",
       "Object payload bytes ingested",
       [](const stats::TrackCounters& c) { return c.received.bytes; }},
      {"moqx_track_groups_sent_total",
       "Groups delivered, summed over subscribers",
       [](const stats::TrackCounters& c) { return c.sent.groups; }},
      {"moqx_track_subgroups_sent_total",
       "Subgroups delivered, summed over subscribers",
       [](const stats::TrackCounters& c) { return c.sent.subgroups; }},
      {"moqx_track_objects_sent_total",
       "Objects delivered, summed over subscribers",
       [](const stats::TrackCounters& c) { return c.sent.objects; }},
      {"moqx_track_datagrams_sent_total",
       "Objects delivered as datagrams, summed over subscribers",
       [](const stats::TrackCounters& c) { return c.sent.datagrams; }},
      {"moqx_track_bytes_sent_total",
       "Object payload bytes delivered, summed over subscribers",
       [](const stats::TrackCounters& c) { return c.sent.bytes; }},
  }};

  struct Timestamp {
    std::string_view name;
    std::string_view help;
    stats::TrackClock::time_point (*value)(const stats::TrackCounters&);
  };

  static constexpr std::array<Timestamp, 2> kTimestamps = {{
      {"moqx_track_publish_start_timestamp_seconds",
       "Unix time the relay first saw the track",
       [](const stats::TrackCounters& c) { return c.publishStart; }},
      {"moqx_track_last_object_timestamp_seconds",
       "Unix time of the most recent ingested object",
       [](const stats::TrackCounters& c) { return c.lastObject; }},
  }};

  // Service names come from config, so they only need Prometheus quoting; track
  // and namespace labels go through safeName() instead.
  std::vector<std::string> labels;
  labels.reserve(result.tracks.size());
  for (const auto& entry : result.tracks) {
    labels.push_back(folly::to<std::string>(
        "{service=\"",
        stats::escapeLabelValue(entry.service),
        "\",namespace=\"",
        safeName(entry.ftn.trackNamespace),
        "\",track=\"",
        safeName(entry.ftn.trackName),
        "\"} "
    ));
  }

  for (const auto& series : kCounters) {
    out.header(series.name, "counter", series.help);
    for (size_t i = 0; i < result.tracks.size(); ++i) {
      out.append(series.name);
      out.append(labels[i]);
      out.num(series.value(result.tracks[i].counters));
      out.append("\n");
    }
    out.append("\n");
  }

  const ClockAnchor anchor;
  for (const auto& series : kTimestamps) {
    out.header(series.name, "gauge", series.help);
    for (size_t i = 0; i < result.tracks.size(); ++i) {
      auto tp = series.value(result.tracks[i].counters);
      // A track with no objects yet has no meaningful timestamp; emitting 0
      // would read as 1970 in any time()-based panel.
      if (tp == stats::TrackClock::time_point{}) {
        continue;
      }
      // Prometheus values are float64 and time is always expressed in seconds,
      // so sub-second resolution is a fraction rather than a different unit.
      auto millis = anchor.toUnixMillis(tp);
      out.append(series.name);
      out.append(labels[i]);
      out.num(millis / 1000);
      out.append(".");
      out.append(folly::sformat("{:03d}", millis % 1000));
      out.append("\n");
    }
    out.append("\n");
  }

  out.header("moqx_track_subscribers", "gauge", "Current downstream subscribers");
  for (size_t i = 0; i < result.tracks.size(); ++i) {
    out.append("moqx_track_subscribers");
    out.append(labels[i]);
    out.num(result.tracks[i].counters.subscribers);
    out.append("\n");
  }
  out.append("\n");

  return out.move();
}

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
          folly::CancellationToken cancelToken,
          std::shared_ptr<EgressGate> /*egress*/
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

        // An empty prefix matches every namespace, as an empty service matches
        // every service; limit is what bounds the scrape either way.
        moxygen::TrackNamespace nsPrefix;
        auto nsParam = req->getDecodedQueryParam("namespace");
        if (!nsParam.empty()) {
          // Same form the labels are rendered in, so a scraped value can be
          // pasted straight back into a query.
          auto parsed = parseSafeNamespace(nsParam);
          if (!parsed) {
            sendError(downstream, 400, "namespace is not in the MOQT safe name form\n");
            return;
          }
          nsPrefix = std::move(*parsed);
        }

        size_t limit = limits.defaultLimit; // default is clamped to maxLimit in ConfigResolver.cpp
        if (req->hasQueryParam("limit")) {
          auto parsed = folly::tryTo<size_t>(req->getDecodedQueryParam("limit"));
          if (!parsed.hasValue() || *parsed == 0) {
            sendError(downstream, 400, "limit must be a positive integer\n");
            return;
          }
          if (*parsed > limits.maxLimit) {
            sendError(
                downstream,
                400,
                folly::to<std::string>(
                    "limit exceeds admin.track_metrics_endpoint_max_limit (",
                    limits.maxLimit,
                    ")\n"
                )
            );
            return;
          }
          limit = *parsed;
        }

        auto omitMetadata = boolQueryParam(*req, "omit_metadata", false);
        if (!omitMetadata) {
          sendError(downstream, 400, "omit_metadata must be one of 1, 0, true, false\n");
          return;
        }

        std::string service = req->getDecodedQueryParam("service");
        std::optional<std::string> track;
        if (req->hasQueryParam("track")) {
          track = parseSafeBytes(req->getDecodedQueryParam("track"));
          if (!track) {
            sendError(downstream, 400, "track is not in the MOQT safe name form\n");
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
                   size_t limit,
                   bool omitMetadata) -> folly::coro::Task<void> {
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
                      .body(formatTrackMetrics(result, omitMetadata))
                      .sendWithEOM();
                }(context,
                                      downstream,
                                      cancelToken,
                                      std::move(service),
                                      std::move(nsPrefix),
                                      std::move(track),
                                      limit,
                                      *omitMetadata)
            )
        )
            .start();
      }
  );
}

} // namespace openmoq::moqx::admin
