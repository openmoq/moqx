/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "config/ConfigDiff.h"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <string>
#include <vector>

using namespace openmoq::moqx::config;
using ::testing::ElementsAre;
using ::testing::IsEmpty;
using ::testing::Not;

namespace {

ListenerConfig makeListener(std::string name, QuicStack stack = QuicStack::Mvfst) {
  ListenerConfig listener{};
  listener.name = std::move(name);
  listener.quicStack = stack;
  return listener;
}

ServiceConfig makeService(std::vector<std::string> listeners, std::string authority = "a.test") {
  ServiceConfig svc{};
  svc.match.push_back(ServiceConfig::MatchEntry{
      .authority = ServiceConfig::MatchEntry::ExactAuthority{std::move(authority)},
      .path = ServiceConfig::MatchEntry::PrefixPath{"/"},
  });
  svc.listeners = std::move(listeners);
  svc.maxSubscriptionBufferKb = 1024;
  return svc;
}

ServiceConfig
makeExactPathService(std::vector<std::string> listeners, std::string authority, std::string path) {
  auto svc = makeService(std::move(listeners), std::move(authority));
  svc.match[0].path = ServiceConfig::MatchEntry::ExactPath{std::move(path)};
  return svc;
}

Config makeConfig(
    std::vector<std::string> listenerNames,
    std::vector<std::pair<std::string, ServiceConfig>> services
) {
  Config config{};
  config.relayID = "relay";
  config.cluster = ClusterConfig{.hopID = 7};
  for (auto& name : listenerNames) {
    config.listeners.push_back(makeListener(std::move(name)));
  }
  for (auto& [name, svc] : services) {
    config.services.emplace(std::move(name), std::move(svc));
  }
  return config;
}

ResolvedConfig makeCandidate(Config config) {
  return ResolvedConfig{
      .config = std::move(config),
      .warnings = {},
      .relayIdGenerated = false,
      .hopIdGenerated = false,
  };
}

std::vector<std::string> paths(const ReloadPlan& plan) {
  std::vector<std::string> out;
  for (const auto& change : plan.ignored) {
    out.push_back(change.path);
  }
  return out;
}

Config baseConfig() {
  return makeConfig({"public"}, {{"svc", makeService({"public"})}});
}

} // namespace

// --- Globals ---

TEST(PlanReload, IdenticalConfigPlansNothing) {
  auto plan = planReload(makeCandidate(baseConfig()), makeCandidate(baseConfig()));
  EXPECT_THAT(plan.ignored, IsEmpty());
  EXPECT_THAT(plan.addedServices, IsEmpty());
  EXPECT_TRUE(plan.effective.services.contains("svc"));
}

TEST(PlanReload, GeneratedIdsAreNotReported) {
  auto current = makeCandidate(baseConfig());
  current.relayIdGenerated = true;
  current.hopIdGenerated = true;
  auto candidate = makeCandidate(baseConfig());
  candidate.config.relayID = "random-relay";
  candidate.config.cluster->hopID = 999;
  candidate.relayIdGenerated = true;
  candidate.hopIdGenerated = true;

  EXPECT_THAT(planReload(current, candidate).ignored, IsEmpty());
}

TEST(PlanReload, ExplicitIdChangesAreReported) {
  auto candidate = makeCandidate(baseConfig());
  candidate.config.relayID = "other";
  candidate.config.cluster->hopID = 999;

  EXPECT_THAT(
      paths(planReload(makeCandidate(baseConfig()), candidate)),
      ElementsAre("relay_id", "cluster")
  );
}

TEST(PlanReload, RemovedExplicitIdsAreReported) {
  auto current = makeCandidate(baseConfig());
  auto candidate = makeCandidate(baseConfig());
  candidate.config.relayID = "random-relay";
  candidate.config.cluster->hopID = 999;
  candidate.relayIdGenerated = true;
  candidate.hopIdGenerated = true;

  EXPECT_THAT(paths(planReload(current, candidate)), ElementsAre("relay_id", "cluster"));
}

TEST(PlanReload, IdsGeneratedOnBothSidesAreNotReported) {
  auto current = makeCandidate(baseConfig());
  current.relayIdGenerated = true;
  current.hopIdGenerated = true;
  auto candidate = makeCandidate(baseConfig());
  candidate.config.relayID = "random-relay";
  candidate.config.cluster->hopID = 999;
  candidate.relayIdGenerated = true;
  candidate.hopIdGenerated = true;

  EXPECT_THAT(planReload(current, candidate).ignored, IsEmpty());
}

TEST(PlanReload, ClusterEnablementChangeReportedEvenWhenHopIdGenerated) {
  auto current = makeCandidate(baseConfig());
  current.hopIdGenerated = true;
  auto candidate = makeCandidate(baseConfig());
  candidate.config.cluster.reset();
  candidate.hopIdGenerated = true;

  EXPECT_THAT(paths(planReload(current, candidate)), ElementsAre("cluster"));
}

TEST(PlanReload, GlobalChangeIsIgnored) {
  auto candidate = makeCandidate(baseConfig());
  candidate.config.threads = 8;

  EXPECT_THAT(paths(planReload(makeCandidate(baseConfig()), candidate)), ElementsAre("threads"));
}

// --- Existing services ---

TEST(PlanReload, MapInsertionOrderIsIrrelevant) {
  auto current = makeConfig(
      {"public"},
      {{"a", makeService({"public"}, "a.test")}, {"b", makeService({"public"}, "b.test")}}
  );
  auto candidate = makeConfig(
      {"public"},
      {{"b", makeService({"public"}, "b.test")}, {"a", makeService({"public"}, "a.test")}}
  );

  EXPECT_THAT(
      planReload(makeCandidate(current), makeCandidate(std::move(candidate))).ignored,
      IsEmpty()
  );
}

TEST(PlanReload, RemovedServicesAreReportedInNameOrder) {
  auto current = makeConfig(
      {"public"},
      {{"b", makeService({"public"}, "b.test")}, {"a", makeService({"public"}, "a.test")}}
  );
  auto candidate = makeConfig({"public"}, {});

  EXPECT_THAT(
      paths(planReload(makeCandidate(current), makeCandidate(std::move(candidate)))),
      ElementsAre("services.a", "services.b")
  );
}

TEST(PlanReload, ChangedServiceIsReportedWithoutValues) {
  auto current = baseConfig();
  current.services.at("svc").auth.hmacKeys.push_back({.id = "k1", .secret = "old-secret"});
  auto candidate = current;
  candidate.services.at("svc").auth.hmacKeys[0].secret = "new-secret";

  auto plan = planReload(makeCandidate(current), makeCandidate(std::move(candidate)));

  ASSERT_THAT(paths(plan), ElementsAre("services.svc"));
  EXPECT_THAT(plan.ignored[0].reason, Not(testing::HasSubstr("secret")));
}

TEST(PlanReload, ListenerAddedInFileLeavesOmittedListenersServiceUnchanged) {
  auto candidate = makeConfig({"public", "extra"}, {{"svc", makeService({"public", "extra"})}});

  // Only the listeners section differs; the service resolved against the new file is not reported.
  EXPECT_THAT(
      paths(planReload(makeCandidate(baseConfig()), makeCandidate(std::move(candidate)))),
      ElementsAre("listeners")
  );
}

TEST(PlanReload, ChangedBindingsAreReportedSeparatelyFromOtherFields) {
  auto current = makeConfig({"a", "b"}, {{"svc", makeService({"a"})}});
  auto candidate = makeConfig({"a", "b"}, {{"svc", makeService({"a", "b"})}});

  EXPECT_THAT(
      paths(planReload(makeCandidate(current), makeCandidate(std::move(candidate)))),
      ElementsAre("services.svc.listeners")
  );
}

TEST(PlanReload, BindingsAndOtherFieldsBothReported) {
  auto current = makeConfig({"a", "b"}, {{"svc", makeService({"a"})}});
  auto candidate = makeConfig({"a", "b"}, {{"svc", makeService({"a", "b"})}});
  candidate.services.at("svc").maxSubscriptionBufferKb = 2048;

  EXPECT_THAT(
      paths(planReload(makeCandidate(current), makeCandidate(std::move(candidate)))),
      ElementsAre("services.svc.listeners", "services.svc")
  );
}

// --- New services ---

TEST(PlanReload, NewServiceIsAdded) {
  auto candidate = baseConfig();
  candidate.services.emplace("new", makeService({"public"}, "b.test"));

  auto plan = planReload(makeCandidate(baseConfig()), makeCandidate(std::move(candidate)));

  EXPECT_THAT(plan.ignored, IsEmpty());
  EXPECT_THAT(plan.addedServices, ElementsAre("new"));
  ASSERT_TRUE(plan.effective.services.contains("new"));
  EXPECT_TRUE(plan.effective.services.contains("svc"));
}

TEST(PlanReload, NewServiceAppliesNextToIgnoredChange) {
  auto candidate = baseConfig();
  candidate.threads = 8;
  candidate.services.emplace("new", makeService({"public"}, "b.test"));

  auto plan = planReload(makeCandidate(baseConfig()), makeCandidate(std::move(candidate)));

  EXPECT_THAT(paths(plan), ElementsAre("threads"));
  EXPECT_THAT(plan.addedServices, ElementsAre("new"));
}

TEST(PlanReload, NewServiceConflictingOnSharedListenerIsIgnored) {
  auto candidate = baseConfig();
  candidate.services.emplace("new", makeService({"public"}));

  auto plan = planReload(makeCandidate(baseConfig()), makeCandidate(std::move(candidate)));

  EXPECT_THAT(paths(plan), ElementsAre("services.new"));
  EXPECT_THAT(plan.addedServices, IsEmpty());
  EXPECT_FALSE(plan.effective.services.contains("new"));
  EXPECT_THAT(plan.ignored[0].reason, testing::HasSubstr("'svc'"));
}

TEST(PlanReload, SameRuleOnDifferentListenersIsNotAConflict) {
  auto current = makeConfig({"a", "b"}, {{"svc", makeService({"a"})}});
  auto candidate = current;
  candidate.services.emplace("new", makeService({"b"}));

  auto plan = planReload(makeCandidate(std::move(current)), makeCandidate(std::move(candidate)));

  EXPECT_THAT(plan.ignored, IsEmpty());
  EXPECT_THAT(plan.addedServices, ElementsAre("new"));
}

TEST(PlanReload, RenamedServiceCollidesWithItsOldRules) {
  auto candidate = makeConfig({"public"}, {{"renamed", makeService({"public"})}});

  auto plan = planReload(makeCandidate(baseConfig()), makeCandidate(std::move(candidate)));

  EXPECT_THAT(paths(plan), ElementsAre("services.svc", "services.renamed"));
  EXPECT_THAT(plan.addedServices, IsEmpty());
}

TEST(PlanReload, TwoNewServicesWithTheSameRuleKeepTheFirstByName) {
  auto candidate = baseConfig();
  candidate.services.emplace("b-new", makeService({"public"}, "n.test"));
  candidate.services.emplace("a-new", makeService({"public"}, "n.test"));

  auto plan = planReload(makeCandidate(baseConfig()), makeCandidate(std::move(candidate)));

  EXPECT_THAT(plan.addedServices, ElementsAre("a-new"));
  EXPECT_THAT(paths(plan), ElementsAre("services.b-new"));
}

TEST(PlanReload, NewServiceOnlyOnCandidateOnlyListenerIsIgnored) {
  auto candidate = makeConfig({"public", "extra"}, {{"svc", makeService({"public", "extra"})}});
  candidate.services.emplace("new", makeService({"extra"}, "b.test"));

  auto plan = planReload(makeCandidate(baseConfig()), makeCandidate(std::move(candidate)));

  EXPECT_THAT(plan.addedServices, IsEmpty());
  EXPECT_THAT(
      paths(plan),
      ElementsAre("listeners", "services.new.listeners[extra]", "services.new")
  );
}

TEST(PlanReload, NewServiceOnRunningAndCandidateOnlyListenersAppliesOnRunningOnly) {
  auto candidate = makeConfig({"public", "extra"}, {{"svc", makeService({"public", "extra"})}});
  candidate.services.emplace("new", makeService({"public", "extra"}, "b.test"));

  auto plan = planReload(makeCandidate(baseConfig()), makeCandidate(std::move(candidate)));

  EXPECT_THAT(plan.addedServices, ElementsAre("new"));
  EXPECT_THAT(paths(plan), ElementsAre("listeners", "services.new.listeners[extra]"));
  EXPECT_THAT(plan.effective.services.at("new").listeners, ElementsAre("public"));
}

namespace {

Config picoAndMvfstConfig() {
  Config config{};
  config.relayID = "relay";
  config.listeners.push_back(makeListener("quic"));
  config.listeners.push_back(makeListener("pico", QuicStack::Picoquic));
  config.services.emplace("svc", makeExactPathService({"quic", "pico"}, "a.test", "/live"));
  return config;
}

} // namespace

TEST(PlanReload, NewExactPathIsDroppedFromPicoListenerOnly) {
  auto candidate = picoAndMvfstConfig();
  candidate.services.emplace("new", makeExactPathService({"quic", "pico"}, "b.test", "/other"));

  auto plan = planReload(makeCandidate(picoAndMvfstConfig()), makeCandidate(std::move(candidate)));

  EXPECT_THAT(plan.addedServices, ElementsAre("new"));
  EXPECT_THAT(paths(plan), ElementsAre("services.new.listeners[pico]"));
  EXPECT_THAT(plan.effective.services.at("new").listeners, ElementsAre("quic"));
}

TEST(PlanReload, NewExactPathOnlyOnPicoListenerIsIgnoredWhole) {
  auto candidate = picoAndMvfstConfig();
  candidate.services.emplace("new", makeExactPathService({"pico"}, "b.test", "/other"));

  auto plan = planReload(makeCandidate(picoAndMvfstConfig()), makeCandidate(std::move(candidate)));

  EXPECT_THAT(plan.addedServices, IsEmpty());
  EXPECT_THAT(paths(plan), ElementsAre("services.new.listeners[pico]", "services.new"));
}

TEST(PlanReload, NewPrefixOnlyServiceIsDroppedFromPicoListenerOnly) {
  auto candidate = picoAndMvfstConfig();
  candidate.services.emplace("new", makeService({"quic", "pico"}, "b.test"));

  auto plan = planReload(makeCandidate(picoAndMvfstConfig()), makeCandidate(std::move(candidate)));

  EXPECT_THAT(plan.addedServices, ElementsAre("new"));
  EXPECT_THAT(paths(plan), ElementsAre("services.new.listeners[pico]"));
  EXPECT_THAT(plan.effective.services.at("new").listeners, ElementsAre("quic"));
}

TEST(PlanReload, NewPrefixOnlyServiceOnPicoListenerAloneIsIgnoredWhole) {
  auto candidate = picoAndMvfstConfig();
  candidate.services.emplace("new", makeService({"pico"}, "b.test"));

  auto plan = planReload(makeCandidate(picoAndMvfstConfig()), makeCandidate(std::move(candidate)));

  EXPECT_THAT(plan.addedServices, IsEmpty());
  EXPECT_THAT(paths(plan), ElementsAre("services.new.listeners[pico]", "services.new"));
}

TEST(PlanReload, NewAuthorityOnExistingPicoPathIsAdded) {
  auto candidate = picoAndMvfstConfig();
  candidate.services.emplace("new", makeExactPathService({"pico"}, "b.test", "/live"));

  auto plan = planReload(makeCandidate(picoAndMvfstConfig()), makeCandidate(std::move(candidate)));

  EXPECT_THAT(plan.ignored, IsEmpty());
  EXPECT_THAT(plan.addedServices, ElementsAre("new"));
}

namespace {

Config withSinks(std::vector<std::string> sinkNames) {
  auto config = baseConfig();
  config.logging = LoggingConfig{.access = AccessLogConfig{}};
  for (auto& name : sinkNames) {
    config.logging->access->sinks.push_back(AccessLogSinkConfig{.name = std::move(name)});
  }
  return config;
}

ServiceConfig serviceSelecting(std::string authority, std::vector<std::string> sinkNames) {
  auto svc = makeService({"public"}, std::move(authority));
  for (auto& name : sinkNames) {
    svc.accessLog.sinks.push_back(AccessLogSinkSelectionConfig{.name = std::move(name)});
  }
  return svc;
}

} // namespace

TEST(PlanReload, NewServiceSelectingSinkMissingFromRunningConfigIsIgnored) {
  auto candidate = withSinks({"a", "b"});
  candidate.services.emplace("new", serviceSelecting("b.test", {"b"}));

  auto plan = planReload(makeCandidate(withSinks({"a"})), makeCandidate(std::move(candidate)));

  EXPECT_THAT(plan.addedServices, IsEmpty());
  EXPECT_THAT(paths(plan), ElementsAre("logging", "services.new"));
  EXPECT_FALSE(plan.effective.services.contains("new"));
}

TEST(PlanReload, NewServiceSelectingSinkWithNoRunningAccessLogIsIgnored) {
  auto candidate = withSinks({"a"});
  candidate.services.emplace("new", serviceSelecting("b.test", {"a"}));

  auto plan = planReload(makeCandidate(baseConfig()), makeCandidate(std::move(candidate)));

  EXPECT_THAT(plan.addedServices, IsEmpty());
  EXPECT_THAT(paths(plan), ElementsAre("logging", "services.new"));
}

TEST(PlanReload, NewServiceSelectingRunningSinksIsAdded) {
  auto candidate = withSinks({"a", "b"});
  candidate.services.emplace("new", serviceSelecting("b.test", {"a"}));

  auto plan = planReload(makeCandidate(withSinks({"a"})), makeCandidate(std::move(candidate)));

  EXPECT_THAT(plan.addedServices, ElementsAre("new"));
  EXPECT_THAT(paths(plan), ElementsAre("logging"));
}

TEST(PlanReload, SkippedSinkServiceDoesNotBlockLaterServiceOnSameRoute) {
  auto candidate = withSinks({"a", "b"});
  candidate.services.emplace("a-new", serviceSelecting("n.test", {"b"}));
  candidate.services.emplace("b-new", serviceSelecting("n.test", {"a"}));

  auto plan = planReload(makeCandidate(withSinks({"a"})), makeCandidate(std::move(candidate)));

  EXPECT_THAT(plan.addedServices, ElementsAre("b-new"));
  EXPECT_THAT(paths(plan), ElementsAre("logging", "services.a-new"));
}
