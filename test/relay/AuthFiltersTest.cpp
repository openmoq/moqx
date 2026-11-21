/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "relay/AuthFilters.h"

#include "MoqxRelay.h"
#include "UpstreamProvider.h"
#include "peering/MoqtRelayBackend.h"

#include "auth/AuthTokenIssuer.h"

#include <folly/coro/BlockingWait.h>
#include <folly/io/async/ScopedEventBaseThread.h>
#include <folly/portability/GMock.h>
#include <folly/portability/GTest.h>
#include <moxygen/test/Mocks.h>

#include <future>
#include <thread>

using testing::_;
using testing::NiceMock;
using namespace moxygen;
using namespace openmoq::moqx;
using namespace openmoq::moqx::auth;

namespace {

config::AuthConfig makeVerifierConfig() {
  return config::AuthConfig{
      .enabled = true,
      .tokenType = 77,
      .hmacKeys = {config::AuthConfig::HmacKey{.id = "k1", .secret = "secret"}},
      .requireSetupToken = true,
      .allowRequestTokenOverride = true,
      .maxTokensPerMessage = 4,
  };
}

Grants makeGrants(std::vector<Action> actions) {
  Grants grants;
  grants.expiresAt = std::chrono::system_clock::now() + std::chrono::hours(1);
  grants.scopes.push_back(
      Scope{.actions = std::move(actions), .namespaceMatches = {}, .trackMatches = {}}
  );
  return grants;
}

AuthToken makeSignedToken(std::vector<Action> actions) {
  return AuthToken{
      .tokenType = 77,
      .tokenValue = signGrants("k1", "secret", makeGrants(std::move(actions))),
      .alias = AuthToken::DontRegister,
  };
}

Parameters withAuthToken(FrameType frameType, AuthToken token) {
  Parameters params(frameType);
  auto ok = params.insertParam(
      Parameter(static_cast<uint64_t>(TrackRequestParamKey::AUTHORIZATION_TOKEN), std::move(token))
  );
  EXPECT_TRUE(ok.hasValue());
  return params;
}

class AuthFiltersTest : public ::testing::Test {
protected:
  void SetUp() override {
    verifier_ = std::make_shared<const AuthTokenVerifier>(makeVerifierConfig());
    publisherInner_ = std::make_shared<NiceMock<MockPublisher>>();
    subscriberInner_ = std::make_shared<NiceMock<MockSubscriber>>();
  }

  std::shared_ptr<AuthPublisherFilter> makePublisherFilter(std::vector<Grants> sessionGrants) {
    return std::make_shared<AuthPublisherFilter>(
        publisherInner_,
        verifier_,
        std::make_shared<const std::vector<Grants>>(std::move(sessionGrants)),
        /*peering=*/nullptr
    );
  }

  std::shared_ptr<AuthSubscriberFilter> makeSubscriberFilter(std::vector<Grants> sessionGrants) {
    return std::make_shared<AuthSubscriberFilter>(
        subscriberInner_,
        verifier_,
        std::make_shared<const std::vector<Grants>>(std::move(sessionGrants))
    );
  }

  std::shared_ptr<const AuthTokenVerifier> verifier_;
  std::shared_ptr<NiceMock<MockPublisher>> publisherInner_;
  std::shared_ptr<NiceMock<MockSubscriber>> subscriberInner_;
};

} // namespace

// A peer relay's relay token skips the grant checks, so an empty grant list does not block it.
TEST_F(AuthFiltersTest, PeerRelaySkipsGrantChecks) {
  auto relay = std::make_shared<MoqxRelay>(config::CacheConfig{}, "sg-sin-2-1");
  MoqtRelayBackend backend(relay, config::ServiceConfig{}, /*hopID=*/std::nullopt);
  AuthPublisherFilter filter(
      publisherInner_,
      verifier_,
      std::make_shared<const std::vector<Grants>>(),
      backend.reciprocalMoqtPeerHandler()
  );
  auto unsupported = [](RequestID id) {
    return folly::makeUnexpected(RequestError{id, RequestErrorCode::NOT_SUPPORTED, "inner"});
  };
  EXPECT_CALL(*publisherInner_, subscribeNamespace(_, _)).WillOnce([&](SubscribeNamespace s, auto) {
    return folly::coro::makeTask<Publisher::SubscribeNamespaceResult>(unsupported(s.requestID));
  });
  EXPECT_CALL(*publisherInner_, subscribeTracks(_, _)).WillOnce([&](SubscribeTracks s, auto) {
    return folly::coro::makeTask<Publisher::SubscribeTracksResult>(unsupported(s.requestID));
  });

  auto peerSubNs =
      folly::coro::blockingWait(filter.subscribeNamespace(makePeerSubNs("jp-osa-1"), nullptr));
  EXPECT_EQ(peerSubNs.error().errorCode, RequestErrorCode::NOT_SUPPORTED);
  auto peerSubTracks =
      folly::coro::blockingWait(filter.subscribeTracks(makePeerSubTracks("jp-osa-1"), nullptr));
  EXPECT_EQ(peerSubTracks.error().errorCode, RequestErrorCode::NOT_SUPPORTED);
  auto plainSubNs = folly::coro::blockingWait(filter.subscribeNamespace(makePeerSubNs(), nullptr));
  EXPECT_EQ(plainSubNs.error().errorCode, RequestErrorCode::UNAUTHORIZED);
}

// A relay that does not have a relay ID does not accept peers, so a relay token alone is not
// enough.
TEST_F(AuthFiltersTest, RelayWithoutAnIdChecksGrantsForRelayTokens) {
  auto relay = std::make_shared<MoqxRelay>(config::CacheConfig{}, /*relayID=*/"");
  MoqtRelayBackend backend(relay, config::ServiceConfig{}, /*hopID=*/std::nullopt);
  AuthPublisherFilter filter(
      publisherInner_,
      verifier_,
      std::make_shared<const std::vector<Grants>>(),
      backend.reciprocalMoqtPeerHandler()
  );
  EXPECT_CALL(*publisherInner_, subscribeNamespace(_, _)).Times(0);
  EXPECT_CALL(*publisherInner_, subscribeTracks(_, _)).Times(0);

  auto subNs =
      folly::coro::blockingWait(filter.subscribeNamespace(makePeerSubNs("jp-osa-1"), nullptr));
  EXPECT_EQ(subNs.error().errorCode, RequestErrorCode::UNAUTHORIZED);
  auto subTracks =
      folly::coro::blockingWait(filter.subscribeTracks(makePeerSubTracks("jp-osa-1"), nullptr));
  EXPECT_EQ(subTracks.error().errorCode, RequestErrorCode::UNAUTHORIZED);
}

// Two session-grants entries where only the second covers Subscribe: the
// filter must permit it (any pool element satisfying is enough).
TEST_F(AuthFiltersTest, C4mSubscribeTracksNeedsItsOwnAction) {
  SubscribeTracks request;
  request.requestID = RequestID(3);
  request.trackNamespacePrefix = TrackNamespace{{"live"}};
  EXPECT_CALL(*publisherInner_, subscribeTracks(_, _)).WillOnce([](SubscribeTracks s, auto) {
    return folly::coro::makeTask<Publisher::SubscribeTracksResult>(folly::makeUnexpected(
        SubscribeTracksError{s.requestID, RequestErrorCode::NOT_SUPPORTED, "inner"}
    ));
  });
  auto denied = folly::coro::blockingWait(
      makePublisherFilter({makeGrants({Action::SubscribeNamespace})})->subscribeTracks(request)
  );
  ASSERT_TRUE(denied.hasError());
  EXPECT_EQ(denied.error().errorCode, RequestErrorCode::UNAUTHORIZED);
  auto forwarded = folly::coro::blockingWait(
      makePublisherFilter({makeGrants({Action::SubscribeTracks})})->subscribeTracks(request)
  );
  ASSERT_TRUE(forwarded.hasError());
  EXPECT_EQ(forwarded.error().errorCode, RequestErrorCode::NOT_SUPPORTED);
}

TEST_F(AuthFiltersTest, C4mRequestUpdateNeedsIndependentGrantAndForwardsCancellation) {
  auto filter = makePublisherFilter({makeGrants({Action::Subscribe})});
  SubscribeOk ok;
  ok.requestID = RequestID(1);
  auto inner = std::make_shared<NiceMock<MockSubscriptionHandle>>(ok);
  EXPECT_CALL(*publisherInner_, subscribe(_, _)).WillOnce([inner](auto, auto) {
    return folly::coro::makeTask<Publisher::SubscribeResult>(
        std::shared_ptr<SubscriptionHandle>(inner)
    );
  });
  SubscribeRequest request;
  request.requestID = RequestID(1);
  request.fullTrackName = FullTrackName{TrackNamespace{{"live"}}, "video"};
  auto subscribed = folly::coro::blockingWait(filter->subscribe(request, nullptr));
  ASSERT_TRUE(subscribed.hasValue());
  EXPECT_CALL(*inner, requestUpdateCalled(_)).Times(0);
  RequestUpdate update;
  update.requestID = RequestID(2);
  auto denied = folly::coro::blockingWait(subscribed.value()->requestUpdate(update));
  ASSERT_TRUE(denied.hasError());
  EXPECT_EQ(denied.error().errorCode, RequestErrorCode::UNAUTHORIZED);
  EXPECT_EQ(denied.error().requestID, update.requestID);
  EXPECT_EQ(subscribed.value()->subscribeOk().requestID, ok.requestID);
  EXPECT_CALL(*inner, unsubscribe()).Times(1);
  subscribed.value()->unsubscribe();
}

TEST_F(AuthFiltersTest, C4mRequestUpdateAcceptsRequestTokenForOriginalTrack) {
  auto filter = makePublisherFilter({makeGrants({Action::Subscribe})});
  SubscribeOk ok;
  ok.requestID = RequestID(1);
  auto inner = std::make_shared<NiceMock<MockSubscriptionHandle>>(ok);
  EXPECT_CALL(*publisherInner_, subscribe(_, _)).WillOnce([inner](auto, auto) {
    return folly::coro::makeTask<Publisher::SubscribeResult>(
        std::shared_ptr<SubscriptionHandle>(inner)
    );
  });
  SubscribeRequest request;
  request.requestID = RequestID(1);
  request.fullTrackName = FullTrackName{TrackNamespace{{"live"}}, "video"};
  auto subscribed = folly::coro::blockingWait(filter->subscribe(request, nullptr));
  ASSERT_TRUE(subscribed.hasValue());
  filter.reset(); // The returned handle owns its verifier and target context.
  EXPECT_CALL(*inner, requestUpdateCalled(_)).Times(1);
  EXPECT_CALL(*inner, requestUpdateResult()).WillOnce([] {
    return folly::Expected<RequestOk, RequestError>(RequestOk{RequestID(2)});
  });
  RequestUpdate update;
  update.requestID = RequestID(2);
  auto grants = makeGrants({Action::RequestUpdate});
  grants.scopes[0].namespaceMatches = {MatchRule{.value = "live"}};
  grants.scopes[0].trackMatches = {MatchRule{.value = "other-video"}};
  grants.scopes[0].exactNamespace = true;
  auto scopedToken = [&] {
    return AuthToken{
        .tokenType = 77,
        .tokenValue = signGrants("k1", "secret", grants),
        .alias = AuthToken::DontRegister
    };
  };
  update.params = withAuthToken(FrameType::REQUEST_UPDATE, scopedToken());
  auto wrongTrack = folly::coro::blockingWait(subscribed.value()->requestUpdate(update));
  ASSERT_TRUE(wrongTrack.hasError());
  EXPECT_EQ(wrongTrack.error().errorCode, RequestErrorCode::UNAUTHORIZED);

  grants.scopes[0].trackMatches[0].value = "video";
  update.params = withAuthToken(FrameType::REQUEST_UPDATE, scopedToken());
  auto forwarded = folly::coro::blockingWait(subscribed.value()->requestUpdate(update));
  ASSERT_TRUE(forwarded.hasValue());
  EXPECT_EQ(forwarded.value().requestID, update.requestID);

  update.params.setMajorVersion(22);
  ASSERT_TRUE(update.params
                  .insertParam(Parameter(
                      static_cast<uint64_t>(TrackRequestParamKey::TRACK_NAMESPACE_PREFIX),
                      std::string("new-prefix")
                  ))
                  .hasValue());
  auto changedNamespace = folly::coro::blockingWait(subscribed.value()->requestUpdate(update));
  ASSERT_TRUE(changedNamespace.hasError());
  EXPECT_EQ(changedNamespace.error().errorCode, RequestErrorCode::UNAUTHORIZED);
}

TEST_F(AuthFiltersTest, C4mAllReturnedHandleKindsGateRequestUpdates) {
  auto filter = makePublisherFilter(
      {makeGrants({Action::Fetch, Action::SubscribeNamespace, Action::SubscribeTracks})}
  );
  auto checkUpdates = [](auto handle, auto inner) {
    RequestUpdate update;
    update.requestID = RequestID(99);
    auto result = folly::coro::blockingWait(handle->requestUpdate(update));
    ASSERT_TRUE(result.hasError());
    EXPECT_EQ(result.error().errorCode, RequestErrorCode::UNAUTHORIZED);
    EXPECT_EQ(result.error().requestID, update.requestID);
    EXPECT_CALL(*inner, requestUpdateCalled(_)).Times(1);
    EXPECT_CALL(*inner, requestUpdateResult()).WillOnce([] {
      return folly::Expected<RequestOk, RequestError>(RequestOk{RequestID(99)});
    });
    update.params =
        withAuthToken(FrameType::REQUEST_UPDATE, makeSignedToken({Action::RequestUpdate}));
    auto forwarded = folly::coro::blockingWait(handle->requestUpdate(update));
    ASSERT_TRUE(forwarded.hasValue());
    EXPECT_EQ(forwarded.value().requestID, update.requestID);
  };

  auto fetchHandle = std::make_shared<NiceMock<MockFetchHandle>>(FetchOk{});
  EXPECT_CALL(*fetchHandle, requestUpdateCalled(_)).Times(0);
  EXPECT_CALL(*publisherInner_, fetch(_, _)).WillOnce([fetchHandle](auto, auto) {
    return folly::coro::makeTask<Publisher::FetchResult>(
        std::shared_ptr<Publisher::FetchHandle>(fetchHandle)
    );
  });
  Fetch fetch;
  fetch.fullTrackName = FullTrackName{TrackNamespace{{"live"}}, "video"};
  auto fetched = folly::coro::blockingWait(filter->fetch(fetch, nullptr));
  ASSERT_TRUE(fetched.hasValue());
  EXPECT_EQ(fetched.value()->fetchOk().requestID, fetchHandle->fetchOk().requestID);
  checkUpdates(fetched.value(), fetchHandle);
  EXPECT_CALL(*fetchHandle, fetchCancel()).Times(1);
  fetched.value()->fetchCancel();

  auto nsHandle = std::make_shared<NiceMock<MockSubscribeNamespaceHandle>>(SubscribeNamespaceOk{});
  EXPECT_CALL(*nsHandle, requestUpdateCalled(_)).Times(0);
  EXPECT_CALL(*publisherInner_, subscribeNamespace(_, _)).WillOnce([nsHandle](auto, auto) {
    return folly::coro::makeTask<Publisher::SubscribeNamespaceResult>(
        std::shared_ptr<Publisher::SubscribeNamespaceHandle>(nsHandle)
    );
  });
  SubscribeNamespace subNs;
  subNs.trackNamespacePrefix = TrackNamespace{{"live"}};
  auto namespaces = folly::coro::blockingWait(filter->subscribeNamespace(subNs, nullptr));
  ASSERT_TRUE(namespaces.hasValue());
  EXPECT_EQ(
      namespaces.value()->subscribeNamespaceOk().requestID,
      nsHandle->subscribeNamespaceOk().requestID
  );
  checkUpdates(namespaces.value(), nsHandle);
  EXPECT_CALL(*nsHandle, unsubscribeNamespace()).Times(1);
  namespaces.value()->unsubscribeNamespace();

  auto tracksHandle = std::make_shared<NiceMock<MockSubscribeTracksHandle>>(SubscribeTracksOk{});
  EXPECT_CALL(*tracksHandle, requestUpdateCalled(_)).Times(0);
  EXPECT_CALL(*publisherInner_, subscribeTracks(_, _)).WillOnce([tracksHandle](auto, auto) {
    return folly::coro::makeTask<Publisher::SubscribeTracksResult>(
        std::shared_ptr<Publisher::SubscribeTracksHandle>(tracksHandle)
    );
  });
  SubscribeTracks subTracks;
  subTracks.trackNamespacePrefix = TrackNamespace{{"live"}};
  auto tracks = folly::coro::blockingWait(filter->subscribeTracks(subTracks));
  ASSERT_TRUE(tracks.hasValue());
  EXPECT_EQ(
      tracks.value()->subscribeTracksOk().requestID,
      tracksHandle->subscribeTracksOk().requestID
  );
  checkUpdates(tracks.value(), tracksHandle);
  EXPECT_CALL(*tracksHandle, unsubscribeTracks()).Times(1);
  tracks.value()->unsubscribeTracks();

  auto subscriber = makeSubscriberFilter({makeGrants({Action::PublishNamespace})});
  auto pubHandle = std::make_shared<NiceMock<MockPublishNamespaceHandle>>(PublishNamespaceOk{});
  EXPECT_CALL(*pubHandle, requestUpdateCalled(_)).Times(0);
  EXPECT_CALL(*subscriberInner_, publishNamespace(_, _)).WillOnce([pubHandle](auto, auto) {
    return folly::coro::makeTask<Subscriber::PublishNamespaceResult>(
        std::shared_ptr<Subscriber::PublishNamespaceHandle>(pubHandle)
    );
  });
  PublishNamespace publish;
  publish.trackNamespace = TrackNamespace{{"live"}};
  auto published = folly::coro::blockingWait(subscriber->publishNamespace(publish, nullptr));
  ASSERT_TRUE(published.hasValue());
  EXPECT_EQ(
      published.value()->publishNamespaceOk().requestID,
      pubHandle->publishNamespaceOk().requestID
  );
  checkUpdates(published.value(), pubHandle);
  EXPECT_CALL(*pubHandle, publishNamespaceDone()).Times(1);
  published.value()->publishNamespaceDone();
}

TEST_F(AuthFiltersTest, SubscribeSucceedsWhenAnySessionGrantCoversIt) {
  auto filter =
      makePublisherFilter({makeGrants({Action::Publish}), makeGrants({Action::Subscribe})});

  SubscribeOk ok;
  ok.requestID = RequestID(1);
  auto handle = std::make_shared<NiceMock<MockSubscriptionHandle>>(ok);
  EXPECT_CALL(*publisherInner_, subscribe(_, _))
      .WillOnce(
          [handle](SubscribeRequest, std::shared_ptr<TrackConsumer>)
              -> folly::coro::Task<Publisher::SubscribeResult> {
            co_return folly::makeExpected<SubscribeError>(std::shared_ptr<SubscriptionHandle>(handle
            ));
          }
      );

  SubscribeRequest sub;
  sub.requestID = RequestID(1);
  sub.fullTrackName = FullTrackName{TrackNamespace{{"live"}}, "video"};
  auto result = folly::coro::blockingWait(filter->subscribe(std::move(sub), nullptr));
  EXPECT_TRUE(result.hasValue());
}

TEST_F(AuthFiltersTest, SubscribeFailsWhenNoSessionGrantCoversIt) {
  auto filter = makePublisherFilter({makeGrants({Action::Publish})});

  SubscribeRequest sub;
  sub.requestID = RequestID(2);
  sub.fullTrackName = FullTrackName{TrackNamespace{{"live"}}, "video"};
  auto result = folly::coro::blockingWait(filter->subscribe(std::move(sub), nullptr));
  ASSERT_FALSE(result.hasValue());
  EXPECT_EQ(result.error().errorCode, SubscribeErrorCode::UNAUTHORIZED);
}

// A malformed per-request token must be dropped as a non-viable candidate,
// not block a still-valid session grant from permitting the request.
TEST_F(AuthFiltersTest, SubscribeGarbageRequestTokenDoesNotBlockSessionGrant) {
  auto filter = makePublisherFilter({makeGrants({Action::Subscribe})});

  SubscribeOk ok;
  ok.requestID = RequestID(3);
  auto handle = std::make_shared<NiceMock<MockSubscriptionHandle>>(ok);
  EXPECT_CALL(*publisherInner_, subscribe(_, _))
      .WillOnce(
          [handle](SubscribeRequest, std::shared_ptr<TrackConsumer>)
              -> folly::coro::Task<Publisher::SubscribeResult> {
            co_return folly::makeExpected<SubscribeError>(std::shared_ptr<SubscriptionHandle>(handle
            ));
          }
      );

  SubscribeRequest sub;
  sub.requestID = RequestID(3);
  sub.fullTrackName = FullTrackName{TrackNamespace{{"live"}}, "video"};
  sub.params = withAuthToken(
      FrameType::SUBSCRIBE,
      AuthToken{
          .tokenType = 77,
          .tokenValue = std::string("\xde\xad\xbe\xef", 4),
          .alias = AuthToken::DontRegister
      }
  );
  auto result = folly::coro::blockingWait(filter->subscribe(std::move(sub), nullptr));
  EXPECT_TRUE(result.hasValue());
}

TEST_F(AuthFiltersTest, PublishRejectedWhenNoSessionGrantCoversIt) {
  auto filter = makeSubscriberFilter({makeGrants({Action::Subscribe})});

  PublishRequest pub;
  pub.requestID = RequestID(4);
  pub.fullTrackName = FullTrackName{TrackNamespace{{"live"}}, "video"};
  auto result = filter->publish(std::move(pub), nullptr);
  ASSERT_FALSE(result.hasValue());
  EXPECT_EQ(result.error().errorCode, PublishErrorCode::UNAUTHORIZED);
}

// A per-request token additively unlocks Publish even though the session
// grants alone (Subscribe only) don't cover it.
TEST_F(AuthFiltersTest, PublishSucceedsWithRequestTokenWhenSessionGrantsDoNotCoverIt) {
  auto filter = makeSubscriberFilter({makeGrants({Action::Subscribe})});

  EXPECT_CALL(*subscriberInner_, publish(_, _))
      .WillOnce(
          [](PublishRequest pub, std::shared_ptr<SubscriptionHandle>) -> Subscriber::PublishResult {
            PublishOk ok;
            ok.requestID = pub.requestID;
            return Subscriber::PublishConsumerAndReplyTask{
                .consumer = nullptr,
                .reply =
                    folly::coro::makeTask<folly::Expected<PublishOk, PublishError>>(std::move(ok)),
            };
          }
      );

  PublishRequest pub;
  pub.requestID = RequestID(5);
  pub.fullTrackName = FullTrackName{TrackNamespace{{"live"}}, "video"};
  pub.params = withAuthToken(FrameType::PUBLISH, makeSignedToken({Action::Publish}));
  auto result = filter->publish(std::move(pub), nullptr);
  EXPECT_TRUE(result.hasValue());
}

// --- Anonymous claim: end-to-end through the auth filters ---

// A service-wide floor lets anyone subscribe/fetch without a token, while
// publish still requires one -- even with sessionGrants empty (as if
// require_setup_token: false and no setup token was ever presented).
TEST(
    AuthFiltersAnonymousClaimTest,
    SubscribeAndFetchAllowedAnonymouslyWhilePublishStillNeedsToken
) {
  auto config = makeVerifierConfig();
  config.anonymousClaim = {
      config::AuthConfig::AnonymousScope{.actions = {Action::Subscribe, Action::Fetch}},
  };
  auto verifier = std::make_shared<const AuthTokenVerifier>(config);
  auto publisherInner = std::make_shared<NiceMock<MockPublisher>>();
  auto subscriberInner = std::make_shared<NiceMock<MockSubscriber>>();
  auto emptySessionGrants = std::make_shared<const std::vector<Grants>>();

  AuthPublisherFilter pubFilter(publisherInner, verifier, emptySessionGrants, /*peering=*/nullptr);
  AuthSubscriberFilter subFilter(subscriberInner, verifier, emptySessionGrants);

  // Subscribe: anonymous claim covers it.
  SubscribeOk subOk;
  subOk.requestID = RequestID(1);
  auto subHandle = std::make_shared<NiceMock<MockSubscriptionHandle>>(subOk);
  EXPECT_CALL(*publisherInner, subscribe(_, _))
      .WillOnce(
          [subHandle](SubscribeRequest, std::shared_ptr<TrackConsumer>)
              -> folly::coro::Task<Publisher::SubscribeResult> {
            co_return folly::makeExpected<SubscribeError>(
                std::shared_ptr<SubscriptionHandle>(subHandle)
            );
          }
      );
  SubscribeRequest sub;
  sub.requestID = RequestID(1);
  sub.fullTrackName = FullTrackName{TrackNamespace{{"live"}}, "video"};
  EXPECT_TRUE(folly::coro::blockingWait(pubFilter.subscribe(std::move(sub), nullptr)).hasValue());

  // Fetch: anonymous claim covers it.
  EXPECT_CALL(*publisherInner, fetch(_, _))
      .WillOnce(
          [](Fetch f, std::shared_ptr<FetchConsumer>) -> folly::coro::Task<Publisher::FetchResult> {
            auto handle =
                std::make_shared<NiceMock<MockFetchHandle>>(FetchOk{.requestID = f.requestID});
            co_return folly::makeExpected<FetchError>(std::shared_ptr<Publisher::FetchHandle>(handle
            ));
          }
      );
  Fetch fetch(
      RequestID(2),
      FullTrackName{TrackNamespace{{"live"}}, "video"},
      AbsoluteLocation{0, 0},
      AbsoluteLocation{1, 0}
  );
  EXPECT_TRUE(folly::coro::blockingWait(pubFilter.fetch(std::move(fetch), nullptr)).hasValue());

  // Publish: the anonymous claim doesn't cover it -- rejected without a token.
  PublishRequest pubNoToken;
  pubNoToken.requestID = RequestID(3);
  pubNoToken.fullTrackName = FullTrackName{TrackNamespace{{"live"}}, "video"};
  auto noTokenResult = subFilter.publish(std::move(pubNoToken), nullptr);
  ASSERT_FALSE(noTokenResult.hasValue());
  EXPECT_EQ(noTokenResult.error().errorCode, PublishErrorCode::UNAUTHORIZED);

  // Publish: a per-request token additively unlocks it.
  EXPECT_CALL(*subscriberInner, publish(_, _))
      .WillOnce(
          [](PublishRequest p, std::shared_ptr<SubscriptionHandle>) -> Subscriber::PublishResult {
            PublishOk ok;
            ok.requestID = p.requestID;
            return Subscriber::PublishConsumerAndReplyTask{
                .consumer = nullptr,
                .reply =
                    folly::coro::makeTask<folly::Expected<PublishOk, PublishError>>(std::move(ok)),
            };
          }
      );
  PublishRequest pubWithToken;
  pubWithToken.requestID = RequestID(4);
  pubWithToken.fullTrackName = FullTrackName{TrackNamespace{{"live"}}, "video"};
  pubWithToken.params = withAuthToken(FrameType::PUBLISH, makeSignedToken({Action::Publish}));
  EXPECT_TRUE(subFilter.publish(std::move(pubWithToken), nullptr).hasValue());
}

// ---- moqt-reval: an expiring token ends only the action it authorized ----

namespace {
Grants revalidatedGrants(std::vector<Action> actions) {
  auto grants = makeGrants(std::move(actions));
  grants.expiresAt = std::chrono::system_clock::now() + std::chrono::seconds(2);
  grants.revalidateEvery = std::chrono::seconds(1);
  return grants;
}

constexpr auto kRevalWait = std::chrono::seconds(8);
} // namespace

TEST_F(AuthFiltersTest, RevalidationEndsTheExpiredSubscriptionOnly) {
  auto filter = makePublisherFilter({revalidatedGrants({Action::Subscribe})});
  SubscribeOk ok;
  ok.requestID = RequestID(1);
  auto inner = std::make_shared<NiceMock<MockSubscriptionHandle>>(ok);
  auto consumer = std::make_shared<NiceMock<MockTrackConsumer>>();
  EXPECT_CALL(*publisherInner_, subscribe(_, _)).WillOnce([inner](auto, auto) {
    return folly::coro::makeTask<Publisher::SubscribeResult>(
        std::shared_ptr<SubscriptionHandle>(inner)
    );
  });
  std::promise<void> unsubscribed;
  std::promise<void> publishDone;
  EXPECT_CALL(*inner, unsubscribe()).Times(1).WillOnce([&] { unsubscribed.set_value(); });
  EXPECT_CALL(*consumer, publishDone(_)).WillOnce([&](PublishDone done) {
    EXPECT_EQ(done.statusCode, PublishDoneStatusCode::UNAUTHORIZED);
    EXPECT_EQ(done.requestID, RequestID(1));
    publishDone.set_value();
    return folly::Expected<folly::Unit, MoQPublishError>(folly::unit);
  });

  folly::ScopedEventBaseThread eventBase;
  SubscribeRequest request;
  request.requestID = RequestID(1);
  request.fullTrackName = FullTrackName{TrackNamespace{{"live"}}, "video"};
  auto subscribed = folly::coro::blockingWait(
      folly::coro::co_withExecutor(eventBase.getEventBase(), filter->subscribe(request, consumer))
  );
  ASSERT_TRUE(subscribed.hasValue());
  EXPECT_EQ(publishDone.get_future().wait_for(kRevalWait), std::future_status::ready);
  EXPECT_EQ(unsubscribed.get_future().wait_for(kRevalWait), std::future_status::ready);
  // The session's own cleanup afterwards must not tear the subscription down a second time.
  subscribed.value()->unsubscribe();
}

TEST_F(AuthFiltersTest, RevalidationStopsWhenTheSubscriptionEndsFirst) {
  auto filter = makePublisherFilter({revalidatedGrants({Action::Subscribe})});
  SubscribeOk ok;
  ok.requestID = RequestID(1);
  auto inner = std::make_shared<NiceMock<MockSubscriptionHandle>>(ok);
  auto consumer = std::make_shared<NiceMock<MockTrackConsumer>>();
  EXPECT_CALL(*publisherInner_, subscribe(_, _)).WillOnce([inner](auto, auto) {
    return folly::coro::makeTask<Publisher::SubscribeResult>(
        std::shared_ptr<SubscriptionHandle>(inner)
    );
  });
  EXPECT_CALL(*inner, unsubscribe()).Times(1);
  EXPECT_CALL(*consumer, publishDone(_)).Times(0);

  folly::ScopedEventBaseThread eventBase;
  SubscribeRequest request;
  request.requestID = RequestID(1);
  request.fullTrackName = FullTrackName{TrackNamespace{{"live"}}, "video"};
  auto subscribed = folly::coro::blockingWait(
      folly::coro::co_withExecutor(eventBase.getEventBase(), filter->subscribe(request, consumer))
  );
  ASSERT_TRUE(subscribed.hasValue());
  subscribed.value()->unsubscribe();                    // ends normally, before the token expires
  std::this_thread::sleep_for(std::chrono::seconds(4)); // past the token expiry
}

TEST_F(AuthFiltersTest, RevalidationEndsTheExpiredNamespacePublication) {
  auto subscriber = makeSubscriberFilter({revalidatedGrants({Action::PublishNamespace})});
  auto pubHandle = std::make_shared<NiceMock<MockPublishNamespaceHandle>>(PublishNamespaceOk{});
  auto callback = std::make_shared<NiceMock<MockPublishNamespaceCallback>>();
  EXPECT_CALL(*subscriberInner_, publishNamespace(_, _)).WillOnce([pubHandle](auto, auto) {
    return folly::coro::makeTask<Subscriber::PublishNamespaceResult>(
        std::shared_ptr<Subscriber::PublishNamespaceHandle>(pubHandle)
    );
  });
  std::promise<void> cancelled;
  std::promise<void> done;
  // The route is dropped before the publisher is told: cancelling makes the session discard its
  // handle, after which the relay's route could no longer be removed.
  testing::InSequence order;
  EXPECT_CALL(*pubHandle, publishNamespaceDone()).Times(1).WillOnce([&] { done.set_value(); });
  EXPECT_CALL(*callback, publishNamespaceCancel(RequestErrorCode::EXPIRED_AUTH_TOKEN, _))
      .WillOnce([&](auto, auto) { cancelled.set_value(); });

  folly::ScopedEventBaseThread eventBase;
  PublishNamespace publish;
  publish.requestID = RequestID(5);
  publish.trackNamespace = TrackNamespace{{"live"}};
  auto published = folly::coro::blockingWait(folly::coro::co_withExecutor(
      eventBase.getEventBase(),
      subscriber->publishNamespace(publish, callback)
  ));
  ASSERT_TRUE(published.hasValue());
  EXPECT_EQ(cancelled.get_future().wait_for(kRevalWait), std::future_status::ready);
  EXPECT_EQ(done.get_future().wait_for(kRevalWait), std::future_status::ready);
  published.value()->publishNamespaceDone(); // not repeated
}

TEST_F(AuthFiltersTest, RevalidationEndsTheExpiredFetch) {
  auto filter = makePublisherFilter({revalidatedGrants({Action::Fetch})});
  FetchOk ok;
  ok.requestID = RequestID(2);
  auto inner = std::make_shared<NiceMock<MockFetchHandle>>(ok);
  auto consumer = std::make_shared<NiceMock<MockFetchConsumer>>();
  EXPECT_CALL(*publisherInner_, fetch(_, _)).WillOnce([inner](auto, auto) {
    return folly::coro::makeTask<Publisher::FetchResult>(
        std::shared_ptr<Publisher::FetchHandle>(inner)
    );
  });
  std::promise<void> cancelled;
  std::promise<void> reset;
  EXPECT_CALL(*inner, fetchCancel()).Times(1).WillOnce([&] { cancelled.set_value(); });
  EXPECT_CALL(*consumer, reset(ResetStreamErrorCode::EXPIRED_AUTH_TOKEN)).WillOnce([&](auto) {
    reset.set_value();
  });

  folly::ScopedEventBaseThread eventBase;
  Fetch fetch;
  fetch.requestID = RequestID(2);
  fetch.fullTrackName = FullTrackName{TrackNamespace{{"live"}}, "video"};
  auto fetched = folly::coro::blockingWait(
      folly::coro::co_withExecutor(eventBase.getEventBase(), filter->fetch(fetch, consumer))
  );
  ASSERT_TRUE(fetched.hasValue());
  EXPECT_EQ(cancelled.get_future().wait_for(kRevalWait), std::future_status::ready);
  EXPECT_EQ(reset.get_future().wait_for(kRevalWait), std::future_status::ready);
}

TEST_F(AuthFiltersTest, RevalidationEndsTheExpiredPublish) {
  auto subscriber = makeSubscriberFilter({revalidatedGrants({Action::Publish})});
  auto consumer = std::make_shared<NiceMock<MockTrackConsumer>>();
  SubscribeOk subscribeOk;
  auto handle = std::make_shared<NiceMock<MockSubscriptionHandle>>(subscribeOk);
  EXPECT_CALL(*subscriberInner_, publish(_, _))
      .WillOnce([consumer](PublishRequest pub, auto) -> Subscriber::PublishResult {
        PublishOk ok;
        ok.requestID = pub.requestID;
        return Subscriber::PublishConsumerAndReplyTask{
            .consumer = consumer,
            .reply = folly::coro::makeTask<folly::Expected<PublishOk, PublishError>>(std::move(ok)),
            .consumerReady = true,
        };
      });
  std::promise<void> unsubscribed;
  EXPECT_CALL(*handle, unsubscribe()).Times(1).WillOnce([&] { unsubscribed.set_value(); });

  PublishRequest pub;
  pub.requestID = RequestID(9);
  pub.fullTrackName = FullTrackName{TrackNamespace{{"live"}}, "video"};
  auto result = subscriber->publish(pub, handle);
  ASSERT_TRUE(result.hasValue());
  folly::ScopedEventBaseThread eventBase;
  // The session awaits the reply on its executor, which starts the revalidation timer.
  auto reply = folly::coro::blockingWait(
      folly::coro::co_withExecutor(eventBase.getEventBase(), std::move(result.value().reply))
  );
  ASSERT_TRUE(reply.hasValue());
  EXPECT_EQ(unsubscribed.get_future().wait_for(kRevalWait), std::future_status::ready);
}

TEST_F(AuthFiltersTest, RevalidationIgnoresARefusedPublish) {
  auto subscriber = makeSubscriberFilter({revalidatedGrants({Action::Publish})});
  auto consumer = std::make_shared<NiceMock<MockTrackConsumer>>();
  SubscribeOk subscribeOk;
  auto handle = std::make_shared<NiceMock<MockSubscriptionHandle>>(subscribeOk);
  EXPECT_CALL(*subscriberInner_, publish(_, _))
      .WillOnce([consumer](PublishRequest pub, auto) -> Subscriber::PublishResult {
        return Subscriber::PublishConsumerAndReplyTask{
            .consumer = consumer,
            .reply = folly::coro::makeTask<folly::Expected<PublishOk, PublishError>>(
                folly::makeUnexpected(
                    PublishError{pub.requestID, PublishErrorCode::UNAUTHORIZED, "refused"}
                )
            ),
        };
      });
  EXPECT_CALL(*handle, unsubscribe()).Times(0);

  PublishRequest pub;
  pub.requestID = RequestID(9);
  pub.fullTrackName = FullTrackName{TrackNamespace{{"live"}}, "video"};
  auto result = subscriber->publish(pub, handle);
  ASSERT_TRUE(result.hasValue());
  folly::ScopedEventBaseThread eventBase;
  auto reply = folly::coro::blockingWait(
      folly::coro::co_withExecutor(eventBase.getEventBase(), std::move(result.value().reply))
  );
  ASSERT_TRUE(reply.hasError());
  std::this_thread::sleep_for(std::chrono::seconds(4)); // past the token expiry
}

// ---- error codes for denied requests ----

namespace {
AuthToken makeExpiredToken(std::vector<Action> actions) {
  auto grants = makeGrants(std::move(actions));
  grants.expiresAt = std::chrono::system_clock::now() - std::chrono::seconds(30);
  return AuthToken{
      .tokenType = 77,
      .tokenValue = signGrants("k1", "secret", grants),
      .alias = AuthToken::DontRegister,
  };
}

AuthToken makeBadSignatureToken(std::vector<Action> actions) {
  return AuthToken{
      .tokenType = 77,
      .tokenValue = signGrants("k1", "another-secret", makeGrants(std::move(actions))),
      .alias = AuthToken::DontRegister,
  };
}
} // namespace

// An expired token is reported as EXPIRED_AUTH_TOKEN on every request type; any other
// failure to authorize stays UNAUTHORIZED. Request error codes are not converted per draft.
TEST_F(AuthFiltersTest, ExpiredRequestTokenIsReportedAsExpiredAuthToken) {
  auto publisher = makePublisherFilter({});
  auto subscriber = makeSubscriberFilter({});
  const auto track = FullTrackName{TrackNamespace{{"live"}}, "video"};
  const auto expired = [](FrameType frame, Action action) {
    return withAuthToken(frame, makeExpiredToken({action}));
  };
  const auto badSignature = [](FrameType frame, Action action) {
    return withAuthToken(frame, makeBadSignatureToken({action}));
  };

  SubscribeRequest sub;
  sub.requestID = RequestID(1);
  sub.fullTrackName = track;
  sub.params = expired(FrameType::SUBSCRIBE, Action::Subscribe);
  EXPECT_EQ(
      folly::coro::blockingWait(publisher->subscribe(sub, nullptr)).error().errorCode,
      RequestErrorCode::EXPIRED_AUTH_TOKEN
  );
  sub.params = badSignature(FrameType::SUBSCRIBE, Action::Subscribe);
  EXPECT_EQ(
      folly::coro::blockingWait(publisher->subscribe(sub, nullptr)).error().errorCode,
      RequestErrorCode::UNAUTHORIZED
  );
  sub.params = Parameters(FrameType::SUBSCRIBE); // no token at all
  EXPECT_EQ(
      folly::coro::blockingWait(publisher->subscribe(sub, nullptr)).error().errorCode,
      RequestErrorCode::UNAUTHORIZED
  );

  Fetch fetch;
  fetch.requestID = RequestID(2);
  fetch.fullTrackName = track;
  fetch.params = expired(FrameType::FETCH, Action::Fetch);
  EXPECT_EQ(
      folly::coro::blockingWait(publisher->fetch(fetch, nullptr)).error().errorCode,
      RequestErrorCode::EXPIRED_AUTH_TOKEN
  );

  SubscribeNamespace subNs;
  subNs.requestID = RequestID(3);
  subNs.trackNamespacePrefix = TrackNamespace{{"live"}};
  subNs.params = expired(FrameType::SUBSCRIBE_NAMESPACE, Action::SubscribeNamespace);
  EXPECT_EQ(
      folly::coro::blockingWait(publisher->subscribeNamespace(subNs, nullptr)).error().errorCode,
      RequestErrorCode::EXPIRED_AUTH_TOKEN
  );

  SubscribeTracks subTracks;
  subTracks.requestID = RequestID(4);
  subTracks.trackNamespacePrefix = TrackNamespace{{"live"}};
  subTracks.params = expired(FrameType::SUBSCRIBE_TRACKS, Action::SubscribeTracks);
  EXPECT_EQ(
      folly::coro::blockingWait(publisher->subscribeTracks(subTracks)).error().errorCode,
      RequestErrorCode::EXPIRED_AUTH_TOKEN
  );

  TrackStatus status;
  status.requestID = RequestID(5);
  status.fullTrackName = track;
  status.params = expired(FrameType::TRACK_STATUS, Action::TrackStatus);
  EXPECT_EQ(
      folly::coro::blockingWait(publisher->trackStatus(status)).error().errorCode,
      RequestErrorCode::EXPIRED_AUTH_TOKEN
  );

  PublishRequest pub;
  pub.requestID = RequestID(6);
  pub.fullTrackName = track;
  pub.params = expired(FrameType::PUBLISH, Action::Publish);
  EXPECT_EQ(
      subscriber->publish(pub, nullptr).error().errorCode,
      RequestErrorCode::EXPIRED_AUTH_TOKEN
  );

  PublishNamespace pubNs;
  pubNs.requestID = RequestID(7);
  pubNs.trackNamespace = TrackNamespace{{"live"}};
  pubNs.params = expired(FrameType::PUBLISH_NAMESPACE, Action::PublishNamespace);
  EXPECT_EQ(
      folly::coro::blockingWait(subscriber->publishNamespace(pubNs, nullptr)).error().errorCode,
      RequestErrorCode::EXPIRED_AUTH_TOKEN
  );
}

// A token that cannot be decoded is MALFORMED_AUTH_TOKEN; one that decodes but is wrongly signed
// or carries a restriction the relay does not support is still UNAUTHORIZED.
TEST_F(AuthFiltersTest, UndecodableRequestTokenIsReportedAsMalformedAuthToken) {
  auto publisher = makePublisherFilter({});
  SubscribeRequest sub;
  sub.requestID = RequestID(1);
  sub.fullTrackName = FullTrackName{TrackNamespace{{"live"}}, "video"};
  sub.params = withAuthToken(
      FrameType::SUBSCRIBE,
      AuthToken{.tokenType = 77, .tokenValue = "not a cose token", .alias = AuthToken::DontRegister}
  );
  EXPECT_EQ(
      folly::coro::blockingWait(publisher->subscribe(sub, nullptr)).error().errorCode,
      RequestErrorCode::MALFORMED_AUTH_TOKEN
  );
  sub.params = withAuthToken(FrameType::SUBSCRIBE, makeBadSignatureToken({Action::Subscribe}));
  EXPECT_EQ(
      folly::coro::blockingWait(publisher->subscribe(sub, nullptr)).error().errorCode,
      RequestErrorCode::UNAUTHORIZED
  );
  // aud with no configured audience is an unsupported restriction, not a serialization error.
  auto grants = makeGrants({Action::Subscribe});
  grants.audiences = {"somewhere"};
  sub.params = withAuthToken(
      FrameType::SUBSCRIBE,
      AuthToken{
          .tokenType = 77,
          .tokenValue = signGrants("k1", "secret", grants),
          .alias = AuthToken::DontRegister
      }
  );
  EXPECT_EQ(
      folly::coro::blockingWait(publisher->subscribe(sub, nullptr)).error().errorCode,
      RequestErrorCode::UNAUTHORIZED
  );
}

// Grants that came from a setup token and have since expired are not "a token in the message",
// so a request relying on them is a plain UNAUTHORIZED.
TEST_F(AuthFiltersTest, ExpiredSessionGrantsAreAPlainUnauthorized) {
  auto grants = makeGrants({Action::Subscribe});
  grants.expiresAt = std::chrono::system_clock::now() - std::chrono::seconds(30);
  auto publisher = makePublisherFilter({grants});
  SubscribeRequest sub;
  sub.requestID = RequestID(1);
  sub.fullTrackName = FullTrackName{TrackNamespace{{"live"}}, "video"};
  EXPECT_EQ(
      folly::coro::blockingWait(publisher->subscribe(sub, nullptr)).error().errorCode,
      RequestErrorCode::UNAUTHORIZED
  );
}

namespace {
class MarkerData : public folly::RequestData {
public:
  bool hasCallback() override { return false; }
};
constexpr auto kMarker = "moqx-revalidation-test";
} // namespace

// moxygen finds the session that owns a handle through the request context of the message being
// handled, so ending an action from a timer has to run inside the context that started it.
TEST_F(AuthFiltersTest, RevalidationEndsTheActionInsideTheRequestContextThatStartedIt) {
  auto filter = makePublisherFilter({revalidatedGrants({Action::Subscribe})});
  SubscribeOk ok;
  ok.requestID = RequestID(1);
  auto inner = std::make_shared<NiceMock<MockSubscriptionHandle>>(ok);
  auto consumer = std::make_shared<NiceMock<MockTrackConsumer>>();
  EXPECT_CALL(*publisherInner_, subscribe(_, _)).WillOnce([inner](auto, auto) {
    return folly::coro::makeTask<Publisher::SubscribeResult>(
        std::shared_ptr<SubscriptionHandle>(inner)
    );
  });
  std::promise<bool> sawMarker;
  EXPECT_CALL(*inner, unsubscribe()).WillOnce([&] {
    auto* context = folly::RequestContext::try_get();
    sawMarker.set_value(context != nullptr && context->hasContextData(kMarker));
  });

  folly::ScopedEventBaseThread eventBase;
  SubscribeRequest request;
  request.requestID = RequestID(1);
  request.fullTrackName = FullTrackName{TrackNamespace{{"live"}}, "video"};
  auto context = std::make_shared<folly::RequestContext>();
  context->setContextData(kMarker, std::make_unique<MarkerData>());
  folly::Try<Publisher::SubscribeResult> subscribed;
  {
    folly::RequestContextScopeGuard guard(context);
    subscribed = folly::coro::blockingWait(folly::coro::co_awaitTry(
        folly::coro::co_withExecutor(eventBase.getEventBase(), filter->subscribe(request, consumer))
    ));
  }
  ASSERT_TRUE(subscribed.hasValue());
  auto future = sawMarker.get_future();
  ASSERT_EQ(future.wait_for(kRevalWait), std::future_status::ready);
  EXPECT_TRUE(future.get());
}
