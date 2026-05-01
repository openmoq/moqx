/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * Originally from github.com/facebookexperimental/moxygen.
 * See deps/moxygen/LICENSE for the original license terms.
 *
 * Copyright (c) OpenMOQ contributors.
 */

// Shared test fixture for MoqxRelay and NamespaceTree tests.
// Only include this from test .cpp files — it uses `using namespace` at file scope.

#pragma once

#include "MoqxRelay.h"
#include "TestUtils.h"
#include <folly/coro/BlockingWait.h>
#include <folly/io/async/EventBase.h>
#include <folly/portability/GMock.h>
#include <folly/portability/GTest.h>
#include <moxygen/MoQTrackProperties.h>
#include <moxygen/events/MoQFollyExecutorImpl.h>
#include <moxygen/relay/MoQForwarder.h>
#include <moxygen/test/MockMoQSession.h>
#include <moxygen/test/Mocks.h>

// NOLINTBEGIN(google-build-using-namespace)
using namespace testing;
using namespace moxygen;
using namespace openmoq::moqx;
// NOLINTEND(google-build-using-namespace)

namespace moxygen::test {

using openmoq::moqx::test::makeBuf;

inline const TrackNamespace kTestNamespace{{"test", "namespace"}};
inline const TrackNamespace kAllowedPrefix{{"test"}};
inline const FullTrackName kTestTrackName{kTestNamespace, "track1"};

// TestMoQExecutor that can be driven for tests
class TestMoQExecutor : public MoQFollyExecutorImpl, public folly::DrivableExecutor {
public:
  explicit TestMoQExecutor();
  ~TestMoQExecutor() override;

  void add(folly::Func func) override;
  void drive() override;
  void driveFor(int n);

private:
  folly::EventBase evb_;
};

// Test fixture for MoqxRelay and NamespaceTree tests.
class MoQRelayTest : public ::testing::Test {
protected:
  void SetUp() override;
  void TearDown() override;

  std::shared_ptr<MockMoQSession> createMockSession();
  std::shared_ptr<Publisher::SubscriptionHandle> createMockSubscriptionHandle();

  void removeSession(std::shared_ptr<MoQSession> sess);

  std::shared_ptr<MockTrackConsumer> createMockConsumer();

  std::shared_ptr<SubscriptionHandle> subscribeToTrack(
      std::shared_ptr<MoQSession> session,
      const FullTrackName& trackName,
      std::shared_ptr<TrackConsumer> consumer,
      RequestID requestID = RequestID(0),
      bool addToState = true,
      folly::Optional<SubscribeErrorCode> expectedError = folly::none
  );

  template <typename Func>
  auto withSessionContext(std::shared_ptr<MoQSession> session, Func&& func) -> decltype(func()) {
    folly::RequestContextScopeGuard guard;
    folly::RequestContext::get()->setContextData(
        sessionRequestToken(),
        std::make_unique<MoQSession::MoQSessionRequestData>(std::move(session))
    );
    return func();
  }

  static const folly::RequestToken& sessionRequestToken();

  struct MockSessionState {
    std::shared_ptr<MoQSession> session;
    std::vector<std::shared_ptr<TrackConsumer>> publishConsumers;
    std::vector<std::shared_ptr<Subscriber::PublishNamespaceHandle>> publishNamespaceHandles;
    std::vector<std::shared_ptr<Publisher::SubscribeNamespaceHandle>> subscribeNamespaceHandles;
    std::vector<std::shared_ptr<Publisher::SubscriptionHandle>> subscribeHandles;

    void cleanup();
  };

  std::map<MoQSession*, std::shared_ptr<MockSessionState>> mockSessions_;

  std::shared_ptr<MockSessionState> getOrCreateMockState(std::shared_ptr<MoQSession> session);
  void cleanupMockSession(std::shared_ptr<MoQSession> session);

  std::shared_ptr<Subscriber::PublishNamespaceHandle> doPublishNamespace(
      std::shared_ptr<MoQSession> session,
      const TrackNamespace& ns,
      bool addToState = true
  );

  std::shared_ptr<TrackConsumer> doPublish(
      std::shared_ptr<MoQSession> session,
      const FullTrackName& trackName,
      bool addToState = true
  );

  std::shared_ptr<Publisher::SubscribeNamespaceHandle> doSubscribeNamespace(
      std::shared_ptr<MoQSession> session,
      const TrackNamespace& nsPrefix,
      bool addToState = true
  );

  std::shared_ptr<MockSubgroupConsumer> createMockSubgroupConsumer();

  std::shared_ptr<TrackConsumer> doPublishWithHandle(
      std::shared_ptr<MoQSession> session,
      const FullTrackName& trackName,
      std::shared_ptr<Publisher::SubscriptionHandle> handle
  );

  std::shared_ptr<Publisher::SubscribeNamespaceHandle> doSubscribeNamespaceWithForward(
      std::shared_ptr<MoQSession> session,
      const TrackNamespace& nsPrefix,
      bool forward
  );

  void setupPublishSucceeds(std::shared_ptr<MockMoQSession> session);

  std::shared_ptr<NiceMock<MockSubscriptionHandle>> makePublishHandle();

  std::shared_ptr<TestMoQExecutor> exec_;
  std::shared_ptr<MoqxRelay> relay_;
};

} // namespace moxygen::test
