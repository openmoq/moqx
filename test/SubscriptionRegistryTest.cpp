/*
 * Copyright (c) OpenMOQ contributors.
 */

#include "SubscriptionRegistry.h"
#include "relay/TopNFilter.h"

#include <folly/coro/BlockingWait.h>
#include <folly/io/async/EventBase.h>
#include <folly/portability/GMock.h>
#include <folly/portability/GTest.h>
#include <moxygen/relay/MoQForwarder.h>

using namespace testing;
using namespace moxygen;
using namespace openmoq::moqx;

namespace {

const TrackNamespace kTestNs{{"test", "namespace"}};
const FullTrackName kFtn{kTestNs, "track1"};

// Minimal chain for subscribe-path tests (no TopNFilter needed).
SubscriptionRegistry::FilterChainResult subscribeChain(std::shared_ptr<MoQForwarder> f) {
  return {std::static_pointer_cast<TrackConsumer>(f), nullptr};
}

// Chain for publish-path tests: createFromPublish dereferences topNFilter.
SubscriptionRegistry::FilterChainResult publishChain(std::shared_ptr<MoQForwarder> f) {
  auto filter = std::make_shared<TopNFilter>(kFtn, nullptr);
  return {std::static_pointer_cast<TrackConsumer>(f), filter};
}

} // namespace

// === Subscribe path ===

TEST(SubscriptionRegistryTest, AwaitSubsequentSucceeds) {
  SubscriptionRegistry registry;
  auto first = std::get<SubscriptionRegistry::FirstSubscriber>(
      registry.getOrCreateFromSubscribe(kFtn, nullptr, subscribeChain)
  );
  auto task = std::get<folly::coro::Task<SubscriptionRegistry::SubsequentSubscriber>>(
      registry.getOrCreateFromSubscribe(kFtn, nullptr, subscribeChain)
  );

  EXPECT_TRUE(first.pending.complete(nullptr, RequestID(0), nullptr, nullptr));

  folly::EventBase evb;
  auto sub = folly::coro::blockingWait(std::move(task), &evb);
  EXPECT_EQ(sub.forwarder, first.forwarder);
}

// === UpstreamSubscribePending ===

TEST(SubscriptionRegistryTest, PendingDestructorRemovesEntry) {
  SubscriptionRegistry registry;
  {
    auto result = registry.getOrCreateFromSubscribe(kFtn, nullptr, subscribeChain);
    ASSERT_TRUE(std::holds_alternative<SubscriptionRegistry::FirstSubscriber>(result));
    // drops result without calling complete() → destructor calls failAndRemove
  }
  EXPECT_FALSE(registry.exists(kFtn));
}

TEST(SubscriptionRegistryTest, PendingDestructorFailsSubsequentSubscriber) {
  SubscriptionRegistry registry;
  auto first = std::get<SubscriptionRegistry::FirstSubscriber>(
      registry.getOrCreateFromSubscribe(kFtn, nullptr, subscribeChain)
  );
  auto task = std::get<folly::coro::Task<SubscriptionRegistry::SubsequentSubscriber>>(
      registry.getOrCreateFromSubscribe(kFtn, nullptr, subscribeChain)
  );

  // Move pending out and drop it — destructor fires failAndRemove, sets exception on promise.
  {
    auto drop = std::move(first.pending);
  }

  folly::EventBase evb;
  EXPECT_THROW(folly::coro::blockingWait(std::move(task), &evb), std::runtime_error);
}

TEST(SubscriptionRegistryTest, PendingCompleteReturnsFalseWhenEntryGone) {
  SubscriptionRegistry registry;
  auto first = std::get<SubscriptionRegistry::FirstSubscriber>(
      registry.getOrCreateFromSubscribe(kFtn, nullptr, subscribeChain)
  );

  registry.remove(kFtn); // simulate publisher replacing the entry mid-subscribe

  EXPECT_FALSE(first.pending.complete(nullptr, RequestID(0), nullptr, nullptr));
}

// Regression: awaitSubsequent must re-find after suspension; erased entry throws.
TEST(SubscriptionRegistryTest, AwaitSubsequentHandlesErasedEntry) {
  SubscriptionRegistry registry;

  auto token1 = registry.getOrCreateFromSubscribe(kFtn, nullptr, subscribeChain);
  ASSERT_TRUE(std::holds_alternative<SubscriptionRegistry::FirstSubscriber>(token1));
  auto& first = std::get<SubscriptionRegistry::FirstSubscriber>(token1);

  auto token2 = registry.getOrCreateFromSubscribe(
      kFtn,
      /*callback=*/nullptr,
      [](std::shared_ptr<MoQForwarder>) -> SubscriptionRegistry::FilterChainResult {
        return {nullptr, nullptr};
      }
  );
  ASSERT_TRUE(
      std::holds_alternative<folly::coro::Task<SubscriptionRegistry::SubsequentSubscriber>>(token2)
  );
  auto subsequentTask =
      std::move(std::get<folly::coro::Task<SubscriptionRegistry::SubsequentSubscriber>>(token2));

  // Upstream subscribe succeeds — fulfills the promise.
  EXPECT_TRUE(first.pending.complete(nullptr, RequestID(0), nullptr, nullptr));

  // Entry is erased before the subsequent coroutine resumes.
  registry.remove(kFtn);

  folly::EventBase evb;
  EXPECT_THROW(folly::coro::blockingWait(std::move(subsequentTask), &evb), std::runtime_error);
}

// === Publish path ===

TEST(SubscriptionRegistryTest, CreateFromPublishEvictsSubscribeEntry) {
  SubscriptionRegistry registry;
  auto first = std::get<SubscriptionRegistry::FirstSubscriber>(
      registry.getOrCreateFromSubscribe(kFtn, nullptr, subscribeChain)
  );
  auto originalForwarder = first.forwarder;
  first.pending.complete(nullptr, RequestID(0), nullptr, nullptr);

  auto newForwarder = std::make_shared<MoQForwarder>(kFtn, std::nullopt);
  auto entry = registry.createFromPublish(
      kFtn,
      newForwarder,
      nullptr,
      nullptr,
      RequestID(1),
      nullptr,
      publishChain
  );

  ASSERT_TRUE(entry.evicted.has_value());
  EXPECT_EQ(entry.evicted->forwarder, originalForwarder);
  EXPECT_EQ(registry.getForwarder(kFtn), newForwarder);
}

// === onPublisherTerminated ===

TEST(SubscriptionRegistryTest, OnPublisherTerminatedErasesEmptyEntry) {
  SubscriptionRegistry registry;
  auto forwarder = std::make_shared<MoQForwarder>(kFtn, std::nullopt);
  registry
      .createFromPublish(kFtn, forwarder, nullptr, nullptr, RequestID(0), nullptr, publishChain);

  auto result = registry.onPublisherTerminated(kFtn);
  EXPECT_EQ(result, nullptr);
  EXPECT_FALSE(registry.exists(kFtn));
}
