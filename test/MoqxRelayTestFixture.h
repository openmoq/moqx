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
#include "relay/PublisherCrossExecFilter.h"
#include "relay/SubscriberCrossExecFilter.h"
#include <folly/coro/BlockingWait.h>
#include <folly/io/async/EventBase.h>
#include <folly/io/async/ScopedEventBaseThread.h>
#include <folly/portability/GMock.h>
#include <folly/portability/GTest.h>
#include <moxygen/MoQTrackProperties.h>
#include <moxygen/events/MoQFollyExecutorImpl.h>
#include <moxygen/test/MockMoQSession.h>
#include <moxygen/test/Mocks.h>

// NOLINTBEGIN(google-build-using-namespace)
using namespace testing;
using namespace moxygen;
using namespace openmoq::moqx;
// NOLINTEND(google-build-using-namespace)

namespace moxygen::test {

enum class RelayMode {
  SingleThread,
  MultiThread,
  // Future: Mode3 — add here, then add a branch in SetUp() and INSTANTIATE entry
};

inline void PrintTo(RelayMode mode, std::ostream* os) {
  switch (mode) {
  case RelayMode::SingleThread:
    *os << "SingleThread";
    return;
  case RelayMode::MultiThread:
    *os << "MultiThread";
    return;
  }
}

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

  void setRelayEvb(folly::EventBase* evb) { relayEvb_ = evb; }

private:
  folly::EventBase evb_;
  folly::EventBase* relayEvb_{nullptr};
};

// Test fixture for MoqxRelay and NamespaceTree tests.
class MoQRelayTest : public ::testing::TestWithParam<RelayMode> {
protected:
  virtual RelayMode relayMode() const { return GetParam(); }

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

  template <typename Func> void verifyOnRelayExec(Func&& func) {
    if (relayEvb_) {
      relayEvb_->runInEventBaseThreadAndWait(std::forward<Func>(func));
    } else {
      func();
    }
  }

  void driveIfMultiThread() {
    if (relayEvb_) {
      exec_->drive();
    }
  }

  // Drive both executors until `done()` is true or maxIters is reached. Each
  // exec_->drive() flushes exec_ and (in MT/LocalForwarderMT modes) synchronizes
  // with relayEvb_ via runInEventBaseThreadAndWait, so this deterministically
  // advances the relay's async cascades (e.g. forwardChanged/NGR requestUpdate)
  // instead of guessing a fixed drive count. Returns done() — false means it
  // timed out. In SingleThread mode drive() degrades to a single loopOnce.
  template <typename Pred> bool driveUntil(Pred&& done, int maxIters = 500) {
    // SingleThread mode is fully synchronous: there is no relay thread to await,
    // so at most one loopOnce can make new progress. Cap iterations at 1 to
    // avoid spinning loopOnce maxIters times when a predicate stays false.
    int iters = relayEvb_ ? maxIters : 1;
    for (int i = 0; i < iters && !done(); ++i) {
      exec_->drive();
    }
    return done();
  }

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

  // Rebuild relay_ (with the MT-mode relay exec) and its cross-exec filters.
  // Use this instead of constructing relay_ directly in tests that need a custom relay.
  void resetRelay(config::CacheConfig cache, const std::string& relayID = "");

  // In MT mode: cross-exec filter wrappers that route test calls through relayExec_.
  // In ST mode: null — accessors fall back to relay_ directly.
  std::shared_ptr<moxygen::Publisher> publisherInterface_;
  std::shared_ptr<moxygen::Subscriber> subscriberInterface_;

  moxygen::Publisher* publisherInterface() {
    return publisherInterface_ ? publisherInterface_.get() : relay_.get();
  }
  moxygen::Subscriber* subscriberInterface() {
    return subscriberInterface_ ? subscriberInterface_.get() : relay_.get();
  }

  std::shared_ptr<TestMoQExecutor> exec_;
  std::shared_ptr<MoqxRelay> relay_;
  std::unique_ptr<folly::ScopedEventBaseThread> relayThread_;
  folly::EventBase* relayEvb_{nullptr};
};

} // namespace moxygen::test
