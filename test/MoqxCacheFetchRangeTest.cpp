/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

// Fetch range accounting in the cache: what a fetch may record as known-empty,
// and which objects it keeps. Spec references are to draft-ietf-moq-transport-18:
// https://www.ietf.org/archive/id/draft-ietf-moq-transport-18.html

#include "MoqxCache.h"
#include "TestUtils.h"
#include <folly/coro/GtestHelpers.h>
#include <folly/portability/GMock.h>
#include <folly/portability/GTest.h>
#include <moxygen/test/Mocks.h>

using namespace testing;
namespace openmoq::moqx::test {
using namespace moxygen; // NOLINT: bring moxygen protocol types into scope

namespace {
const FullTrackName kTestTrackName{TrackNamespace{{"foo"}}, "bar"};

Fetch getFetch(AbsoluteLocation start, AbsoluteLocation end) {
  return Fetch{0, kTestTrackName, start, end, kDefaultPriority, GroupOrder::Default};
}
} // namespace

class MoqxCacheFetchRangeTest : public ::testing::Test {
protected:
  void SetUp() override {
    cache_.clear();
    cache_.setMaxCachedTracks(0);
    cache_.setMaxCachedGroupsPerTrack(0);
    cache_.setMaxCachedBytes(0);
    cache_.setMinEvictionBytes(0);
  }

  // Answers the next upstream fetch with FETCH_OK carrying `largest`, and
  // parks its consumer in upstreamFetchConsumer_ for the test to drive.
  void expectUpstreamFetch(AbsoluteLocation start, AbsoluteLocation end, AbsoluteLocation largest) {
    EXPECT_CALL(*upstream_, fetch(_, _))
        .WillOnce(
            [start, end, largest, this](Fetch fetch, std::shared_ptr<FetchConsumer> consumer) {
              auto* standalone = std::get_if<StandaloneFetch>(&fetch.args);
              EXPECT_EQ(standalone->start, start);
              EXPECT_EQ(standalone->end, end);
              upstreamFetchConsumer_ = std::move(consumer);
              return folly::coro::makeTask<Publisher::FetchResult>(
                  std::make_shared<moxygen::MockFetchHandle>(
                      FetchOk{0, GroupOrder::OldestFirst, false, largest, {}}
                  )
              );
            }
        )
        .RetiresOnSaturation();
  }

  // Expects object(group, objectID) on `consumer` for each of `objects`, in order.
  void expectObjects(
      std::shared_ptr<moxygen::MockFetchConsumer> consumer,
      uint64_t group,
      std::vector<uint64_t> objects
  ) {
    InSequence enforceOrder;
    for (auto objectID : objects) {
      EXPECT_CALL(*consumer, object(_, _, _, _, _, _, _))
          .WillOnce([group, objectID](auto g, auto, auto o, auto, const auto&, auto, auto) {
            EXPECT_EQ(g, group);
            EXPECT_EQ(o, objectID);
            return folly::unit;
          })
          .RetiresOnSaturation();
    }
  }

  MoqxCache cache_;
  std::shared_ptr<StrictMock<MockPublisher>> upstream_{std::make_shared<StrictMock<MockPublisher>>()
  };
  std::shared_ptr<moxygen::MockFetchConsumer> consumer_{
      std::make_shared<StrictMock<moxygen::MockFetchConsumer>>()
  };
  std::shared_ptr<moxygen::FetchConsumer> upstreamFetchConsumer_;
};

CO_TEST_F(MoqxCacheFetchRangeTest, RangePastFetchOkLargestStaysUnknown) {
  // A FIN-terminated fetch stream carrying no object means nothing exists in
  // the requested range, as far as the Largest Location in FETCH_OK (§10.12.3
  // https://www.ietf.org/archive/id/draft-ietf-moq-transport-18.html#name-fetch-handling).
  // The request here spans groups 0 and 1 against a Largest of {0,5}: group 1
  // lies past it, so a fetch of group 1 resolves against upstream.
  expectUpstreamFetch({0, 0}, {2, 0}, AbsoluteLocation{0, 5});
  EXPECT_CALL(*consumer_, endOfFetch()).Times(AnyNumber()).WillRepeatedly(Return(folly::unit));
  auto res = co_await cache_.fetch(getFetch({0, 0}, {2, 0}), consumer_, upstream_);
  EXPECT_TRUE(res.hasValue());
  upstreamFetchConsumer_->endOfFetch(); // zero objects delivered

  co_await folly::coro::co_reschedule_on_current_executor;

  upstreamFetchConsumer_.reset();
  expectUpstreamFetch({1, 0}, {1, 10}, AbsoluteLocation{1, 10});
  res = co_await cache_.fetch(getFetch({1, 0}, {1, 10}), consumer_, upstream_);
  EXPECT_TRUE(res.hasValue());
  if (!upstreamFetchConsumer_) {
    ADD_FAILURE() << "range past the FETCH_OK Largest Location was recorded as known-empty: "
                     "the second fetch answered from cache without re-probing upstream";
    co_return;
  }
  expectObjects(consumer_, 1, {0, 1});
  upstreamFetchConsumer_->object(1, 0, 0, makeBuf(100));
  upstreamFetchConsumer_->object(1, 0, 1, makeBuf(100));
}

CO_TEST_F(MoqxCacheFetchRangeTest, ColdCacheWholeGroupFetchKeepsDeliveredObjects) {
  // An End Location object of 0 requests the entire group (§10.12.1
  // https://www.ietf.org/archive/id/draft-ietf-moq-transport-18.html#name-standalone-fetch).
  // The track is not in the cache when the fetch starts; every object the
  // fetch delivers is cached, so the second fetch is a full hit.
  expectUpstreamFetch({0, 0}, {0, 0}, AbsoluteLocation{0, 2});
  expectObjects(consumer_, 0, {0, 1, 2});
  auto res = co_await cache_.fetch(getFetch({0, 0}, {0, 0}), consumer_, upstream_);
  EXPECT_TRUE(res.hasValue());
  upstreamFetchConsumer_->object(0, 0, 0, makeBuf(100));
  upstreamFetchConsumer_->object(0, 0, 1, makeBuf(100));
  upstreamFetchConsumer_->object(0, 0, 2, makeBuf(100));

  co_await folly::coro::co_reschedule_on_current_executor;

  auto consumer2{std::make_shared<StrictMock<moxygen::MockFetchConsumer>>()};
  expectObjects(consumer2, 0, {0, 1, 2});
  res = co_await cache_.fetch(getFetch({0, 0}, {0, 3}), consumer2, upstream_);
  EXPECT_TRUE(res.hasValue());
}

} // namespace openmoq::moqx::test
