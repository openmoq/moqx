/*
 * Copyright (c) Meta Platforms, Inc. and affiliates.
 * Originally from github.com/facebookexperimental/moxygen.
 * See deps/moxygen/LICENSE for the original license terms.
 *
 * Copyright (c) OpenMOQ contributors.
 */

#include "MoqxRelayTestFixture.h"

namespace moxygen::test {

// Test: TrackStatus on non-existent track
TEST_F(MoQRelayTest, TrackStatusNonExistentTrack) {
  auto clientSession = createMockSession();

  // Request trackStatus for a track that doesn't exist
  TrackStatus trackStatus;
  trackStatus.fullTrackName = kTestTrackName;
  trackStatus.requestID = RequestID(1);

  withSessionContext(clientSession, [&]() {
    auto task = publisherInterface()->trackStatus(trackStatus);
    auto res = folly::coro::blockingWait(std::move(task), exec_.get());

    // Should return error indicating track not found
    EXPECT_FALSE(res.hasValue());
    EXPECT_EQ(res.error().errorCode, TrackStatusErrorCode::TRACK_NOT_EXIST);
    EXPECT_FALSE(res.error().reasonPhrase.empty());
  });

  removeSession(clientSession);
}

// Test: TrackStatus on existing track - returns forwarder state (no upstream
// call)
TEST_F(MoQRelayTest, TrackStatusSuccessfulForward) {
  auto publisherSession = createMockSession();
  auto clientSession = createMockSession();

  doPublish(publisherSession, kTestTrackName);

  auto consumer = createMockConsumer();
  subscribeToTrack(clientSession, kTestTrackName, consumer, RequestID(1));

  TrackStatus trackStatus;
  trackStatus.fullTrackName = kTestTrackName;
  trackStatus.requestID = RequestID(2);

  withSessionContext(clientSession, [&]() {
    auto task = publisherInterface()->trackStatus(trackStatus);
    auto res = folly::coro::blockingWait(std::move(task), exec_.get());

    // Should return status from local forwarder
    // Since no data was sent, statusCode should be TRACK_NOT_STARTED
    EXPECT_TRUE(res.hasValue());
    EXPECT_EQ(res.value().statusCode, TrackStatusCode::TRACK_NOT_STARTED);
    EXPECT_EQ(res.value().fullTrackName, kTestTrackName);
  });

  removeSession(clientSession);
  exec_->drive();
  removeSession(publisherSession);
}

// Test: TrackStatus using namespace prefix matching (no exact subscription)
// Verifies that when there's no exact subscription but a publisher has
// published a matching namespace prefix, the relay correctly routes
// TRACK_STATUS upstream using prefix matching
TEST_F(MoQRelayTest, TrackStatusViaPrefixMatching) {
  auto publisher = createMockSession();
  auto requester = createMockSession();

  // Publisher publishes namespace but NOT the specific track
  doPublishNamespace(publisher, kTestNamespace);

  // No exact subscription exists for kTestTrackName, so trackStatus should
  // use prefix matching to find the publisher

  // Mock the upstream trackStatus call
  TrackStatusOk statusOk;
  statusOk.requestID = RequestID(1);
  statusOk.trackAlias = TrackAlias(0);
  statusOk.largest = AbsoluteLocation{50, 25};

  EXPECT_CALL(*publisher, trackStatus(_)).WillOnce([statusOk](const auto& /*ts*/) {
    return folly::coro::makeTask<Publisher::TrackStatusResult>(statusOk);
  });

  // Execute trackStatus from requester's perspective
  TrackStatus trackStatus;
  trackStatus.requestID = RequestID(1);
  trackStatus.fullTrackName = kTestTrackName;

  withSessionContext(requester, [&]() {
    auto task = publisherInterface()->trackStatus(trackStatus);
    auto result = folly::coro::blockingWait(std::move(task), exec_.get());

    // Should successfully forward via prefix matching and return the result
    EXPECT_TRUE(result.hasValue()) << "TrackStatus via namespace prefix matching should succeed";
    EXPECT_EQ(result.value().requestID, RequestID(1));
    EXPECT_TRUE(result.value().largest.has_value());
    EXPECT_EQ(result.value().largest->group, 50);
    EXPECT_EQ(result.value().largest->object, 25);
  });

  removeSession(publisher);
  removeSession(requester);
}

} // namespace moxygen::test
