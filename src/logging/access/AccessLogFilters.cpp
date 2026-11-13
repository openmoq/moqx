/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "logging/access/AccessLogFilters.h"

#include <type_traits>

#include <folly/coro/Task.h>
#include <folly/logging/xlog.h>
#include <moxygen/MoQSession.h>

#include "logging/access/AccessLogHandles.h"

namespace openmoq::moqx::logging {

namespace {

template <typename MakeLogRecord>
std::shared_ptr<AccessLogRequest> startRequest(
    const std::shared_ptr<AccessLogSessionContext>& context,
    AccessLogEvent startEvent,
    AccessLogDir dir,
    MakeLogRecord makeLogRecord
) {
  if (!context->active()) {
    return nullptr;
  }
  return std::make_shared<AccessLogRequest>(context, startEvent, dir, makeLogRecord());
}

// The session assigns a tx request's ID, so only an rx request has one up front.
AccessLogRecord
requestLogRecord(const moxygen::FullTrackName& ftn, moxygen::RequestID id, AccessLogDir dir) {
  AccessLogRecord record;
  record.ftn = std::make_shared<const moxygen::FullTrackName>(ftn);
  if (dir == AccessLogDir::Rx) {
    record.requestID = id.value;
  }
  return record;
}

AccessLogRecord
namespaceLogRecord(const moxygen::TrackNamespace& ns, moxygen::RequestID id, AccessLogDir dir) {
  auto record = requestLogRecord(moxygen::FullTrackName{ns, {}}, id, dir);
  record.namespaceOnly = true;
  return record;
}

AccessLogRecord subscribeLogRecord(const moxygen::SubscribeRequest& sub, AccessLogDir dir) {
  auto record = requestLogRecord(sub.fullTrackName, sub.requestID, dir);
  record.priority = sub.priority;
  record.groupOrder = sub.groupOrder;
  record.forward = sub.forward;
  return record;
}

AccessLogRecord fetchLogRecord(const moxygen::Fetch& fetch, AccessLogDir dir) {
  auto record = requestLogRecord(fetch.fullTrackName, fetch.requestID, dir);
  record.priority = fetch.priority;
  record.groupOrder = fetch.groupOrder;
  auto [standalone, joining] = moxygen::fetchType(fetch);
  if (standalone) {
    record.fetchType = "standalone";
    record.start = standalone->start;
    record.end = standalone->end;
  } else if (joining) {
    record.fetchType =
        joining->fetchType == moxygen::FetchType::RELATIVE_JOINING ? "relative" : "absolute";
  }
  return record;
}

AccessLogRecord publishLogRecord(const moxygen::PublishRequest& pub, AccessLogDir dir) {
  auto record = requestLogRecord(pub.fullTrackName, pub.requestID, dir);
  record.alias = pub.trackAlias.value;
  record.groupOrder = pub.groupOrder;
  record.forward = pub.forward;
  record.largest = pub.largest;
  return record;
}

constexpr auto kUnwrapped = [](auto value) { return value; };

// Writes the transaction line at the reply and wraps a successful reply's handle. Only a
// tx request reads its ID from the reply. An rx request's ID comes from the request.
template <AccessLogDir kDir, typename T, typename E, typename RequestIDFromReply, typename Wrap>
folly::coro::Task<folly::Expected<T, E>> logReply(
    folly::coro::Task<folly::Expected<T, E>> task,
    std::shared_ptr<AccessLogRequest> logRequest,
    RequestIDFromReply requestIDFromReply,
    Wrap wrap
) {
  auto result = co_await folly::coro::co_awaitTry(std::move(task));
  if (result.hasException()) {
    logRequest->aborted();
    co_yield folly::coro::co_error(std::move(result.exception()));
  }
  if (result->hasValue()) {
    if constexpr (kDir == AccessLogDir::Tx) {
      logRequest->ok(requestIDFromReply(result->value()));
    } else {
      logRequest->ok(std::nullopt);
    }
    result->value() = wrap(std::move(result->value()));
  } else {
    auto requestID =
        kDir == AccessLogDir::Tx ? txRequestID(result->error().requestID) : std::nullopt;
    logRequest->error(requestID, result->error());
  }
  co_return std::move(*result);
}

template <AccessLogDir kDir, typename T, typename E, typename RequestIDFromReply, typename Wrap>
folly::coro::Task<folly::Expected<T, E>> maybeLogReply(
    folly::coro::Task<folly::Expected<T, E>> task,
    std::shared_ptr<AccessLogRequest> logRequest,
    RequestIDFromReply requestIDFromReply,
    Wrap wrap
) {
  if (!logRequest) {
    return task;
  }
  return logReply<kDir>(
      std::move(task),
      std::move(logRequest),
      std::move(requestIDFromReply),
      std::move(wrap)
  );
}

// The session keeps what the relay hands it on a tx request, and what the relay returns
// for an rx request.
template <AccessLogDir kDir> constexpr bool kSessionHoldsArgument = kDir == AccessLogDir::Tx;
template <AccessLogDir kDir> constexpr bool kSessionHoldsReply = kDir == AccessLogDir::Rx;

template <AccessLogDir kDir> class LoggedPublisher : public moxygen::Publisher {
public:
  LoggedPublisher(
      std::shared_ptr<moxygen::Publisher> inner,
      std::shared_ptr<AccessLogSessionContext> context
  )
      : inner_(std::move(inner)), context_(std::move(context)) {}

  folly::coro::Task<TrackStatusResult> trackStatus(moxygen::TrackStatus req) override {
    auto logRequest = startRequest(context_, AccessLogEvent::TRACK_STATUS, kDir, [&] {
      return requestLogRecord(req.fullTrackName, req.requestID, kDir);
    });
    return maybeLogReply<kDir>(
        inner_->trackStatus(std::move(req)),
        std::move(logRequest),
        [](const moxygen::TrackStatusOk& ok) { return txRequestID(ok.requestID); },
        kUnwrapped
    );
  }

  folly::coro::Task<SubscribeResult> subscribe(
      moxygen::SubscribeRequest sub,
      std::shared_ptr<moxygen::TrackConsumer> consumer
  ) override {
    auto logRequest = startRequest(context_, AccessLogEvent::SUBSCRIBE, kDir, [&] {
      return subscribeLogRecord(sub, kDir);
    });
    if (logRequest) {
      consumer = logged(std::move(consumer), logRequest, kSessionHoldsArgument<kDir>);
    }
    return maybeLogReply<kDir>(
        inner_->subscribe(std::move(sub), std::move(consumer)),
        logRequest,
        [](const std::shared_ptr<moxygen::SubscriptionHandle>& handle) {
          return handle->hasSubscribeOk() ? txRequestID(handle->subscribeOk().requestID)
                                          : std::nullopt;
        },
        [logRequest](std::shared_ptr<moxygen::SubscriptionHandle> handle) {
          return logged(std::move(handle), logRequest, kSessionHoldsReply<kDir>);
        }
    );
  }

  folly::coro::Task<FetchResult>
  fetch(moxygen::Fetch fetch, std::shared_ptr<moxygen::FetchConsumer> consumer) override {
    auto logRequest = startRequest(context_, AccessLogEvent::FETCH, kDir, [&] {
      return fetchLogRecord(fetch, kDir);
    });
    if (logRequest) {
      consumer = logged(std::move(consumer), logRequest, kSessionHoldsArgument<kDir>);
    }
    return maybeLogReply<kDir>(
        inner_->fetch(std::move(fetch), std::move(consumer)),
        logRequest,
        [](const std::shared_ptr<FetchHandle>& handle) {
          return txRequestID(handle->fetchOk().requestID);
        },
        [logRequest](std::shared_ptr<FetchHandle> handle) {
          return logged(std::move(handle), logRequest, kSessionHoldsReply<kDir>);
        }
    );
  }

  folly::coro::Task<SubscribeNamespaceResult> subscribeNamespace(
      moxygen::SubscribeNamespace subNs,
      std::shared_ptr<NamespacePublishHandle> handle
  ) override {
    auto logRequest = startRequest(context_, AccessLogEvent::SUBSCRIBE_NAMESPACE, kDir, [&] {
      return namespaceLogRecord(subNs.trackNamespacePrefix, subNs.requestID, kDir);
    });
    if (logRequest) {
      handle = logged(std::move(handle), logRequest, kSessionHoldsArgument<kDir>);
    }
    return maybeLogReply<kDir>(
        inner_->subscribeNamespace(std::move(subNs), std::move(handle)),
        logRequest,
        [](const std::shared_ptr<SubscribeNamespaceHandle>& handle) {
          return txRequestID(handle->subscribeNamespaceOk().requestID);
        },
        [logRequest](std::shared_ptr<SubscribeNamespaceHandle> handle) {
          return logged(std::move(handle), logRequest, kSessionHoldsReply<kDir>);
        }
    );
  }

  folly::coro::Task<SubscribeTracksResult> subscribeTracks(
      moxygen::SubscribeTracks subTracks,
      std::shared_ptr<PublishBlockedHandle> publishBlockedHandle
  ) override {
    auto logRequest = startRequest(context_, AccessLogEvent::SUBSCRIBE_TRACKS, kDir, [&] {
      return namespaceLogRecord(subTracks.trackNamespacePrefix, subTracks.requestID, kDir);
    });
    if (logRequest) {
      publishBlockedHandle =
          logged(std::move(publishBlockedHandle), logRequest, kSessionHoldsArgument<kDir>);
    }
    return maybeLogReply<kDir>(
        inner_->subscribeTracks(std::move(subTracks), std::move(publishBlockedHandle)),
        logRequest,
        [](const std::shared_ptr<SubscribeTracksHandle>& handle) {
          return txRequestID(handle->subscribeTracksOk().requestID);
        },
        [logRequest](std::shared_ptr<SubscribeTracksHandle> handle) {
          return logged(std::move(handle), logRequest, kSessionHoldsReply<kDir>);
        }
    );
  }

  void goaway(moxygen::Goaway goaway) override {
    context_->write(AccessLogEvent::GOAWAY, false, [&](AccessLogRecord& record) {
      record.dir = kDir;
      record.goawayUri = goaway.newSessionUri;
    });
    inner_->goaway(std::move(goaway));
  }

private:
  std::shared_ptr<moxygen::Publisher> inner_;
  std::shared_ptr<AccessLogSessionContext> context_;
};

template <AccessLogDir kDir> class LoggedSubscriber : public moxygen::Subscriber {
public:
  LoggedSubscriber(
      std::shared_ptr<moxygen::Subscriber> inner,
      std::shared_ptr<AccessLogSessionContext> context
  )
      : inner_(std::move(inner)), context_(std::move(context)) {}

  folly::coro::Task<PublishNamespaceResult> publishNamespace(
      moxygen::PublishNamespace pubNs,
      std::shared_ptr<PublishNamespaceCallback> callback
  ) override {
    auto logRequest = startRequest(context_, AccessLogEvent::PUBLISH_NAMESPACE, kDir, [&] {
      return namespaceLogRecord(pubNs.trackNamespace, pubNs.requestID, kDir);
    });
    if (logRequest) {
      callback = logged(std::move(callback), logRequest, kSessionHoldsArgument<kDir>);
    }
    return maybeLogReply<kDir>(
        inner_->publishNamespace(std::move(pubNs), std::move(callback)),
        logRequest,
        [](const std::shared_ptr<PublishNamespaceHandle>& handle) {
          return txRequestID(handle->publishNamespaceOk().requestID);
        },
        [logRequest](std::shared_ptr<PublishNamespaceHandle> handle) {
          return logged(std::move(handle), logRequest, kSessionHoldsReply<kDir>);
        }
    );
  }

  // A PUBLISH replies with its consumer and a reply task.
  PublishResult publish(
      moxygen::PublishRequest pub,
      std::shared_ptr<moxygen::Publisher::SubscriptionHandle> handle
  ) override {
    auto logRequest = startRequest(context_, AccessLogEvent::PUBLISH, kDir, [&] {
      return publishLogRecord(pub, kDir);
    });
    if (!logRequest) {
      return inner_->publish(std::move(pub), std::move(handle));
    }
    handle = logged(std::move(handle), logRequest, kSessionHoldsArgument<kDir>);
    auto result = inner_->publish(std::move(pub), std::move(handle));
    if (result.hasError()) {
      auto requestID =
          kDir == AccessLogDir::Tx ? txRequestID(result.error().requestID) : std::nullopt;
      logRequest->error(requestID, result.error());
      return result;
    }
    result->consumer = logged(std::move(result->consumer), logRequest, kSessionHoldsReply<kDir>);
    result->reply = logPublishReply(std::move(result->reply), logRequest);
    return result;
  }

  void goaway(moxygen::Goaway goaway) override { inner_->goaway(std::move(goaway)); }

private:
  std::shared_ptr<moxygen::Subscriber> inner_;
  std::shared_ptr<AccessLogSessionContext> context_;
};

} // namespace

std::shared_ptr<moxygen::Publisher> logPublisher(
    std::shared_ptr<moxygen::Publisher> inner,
    std::shared_ptr<AccessLogSessionContext> context,
    AccessLogDir dir
) {
  if (!context || !inner || context->filteredOut()) {
    return inner;
  }
  if (dir == AccessLogDir::Rx) {
    return std::make_shared<LoggedPublisher<AccessLogDir::Rx>>(
        std::move(inner),
        std::move(context)
    );
  }
  return std::make_shared<LoggedPublisher<AccessLogDir::Tx>>(std::move(inner), std::move(context));
}

std::shared_ptr<moxygen::Subscriber> logSubscriber(
    std::shared_ptr<moxygen::Subscriber> inner,
    std::shared_ptr<AccessLogSessionContext> context,
    AccessLogDir dir
) {
  if (!context || !inner || context->filteredOut()) {
    return inner;
  }
  if (dir == AccessLogDir::Rx) {
    return std::make_shared<LoggedSubscriber<AccessLogDir::Rx>>(
        std::move(inner),
        std::move(context)
    );
  }
  return std::make_shared<LoggedSubscriber<AccessLogDir::Tx>>(std::move(inner), std::move(context));
}

} // namespace openmoq::moqx::logging
