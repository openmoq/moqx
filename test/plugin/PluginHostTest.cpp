/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "plugin/Host.h"
#include "plugin/TestPlugin.h"

#include <folly/coro/BlockingWait.h>
#include <folly/io/async/EventBase.h>
#include <folly/json/json.h>
#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <moxygen/MoQTypes.h>

namespace openmoq::moqx::plugin::test {
namespace {

using ::testing::ElementsAre;
using ::testing::HasSubstr;
using ::testing::IsEmpty;

class PluginHostTest : public ::testing::Test {
protected:
  void SetUp() override {
    currentScript() = std::make_shared<Script>();
    script_ = currentScript();
  }

  // A gate wait detached by cancellation holds the event base until it opens.
  void TearDown() override {
    script_->admitGate.post();
    script_->authorizeGate.post();
    script_->snapshotGate.post();
    drain();
  }

  config::PluginEntryConfig entry(std::string id, int64_t rank = 0, bool listenerParams = false) {
    TestConfig cfg;
    cfg.name = id;
    cfg.listener_params = listenerParams;
    return config::PluginEntryConfig{
        .type = "test",
        .id = std::move(id),
        .rank = rank,
        .pluginType = type_,
        .config = std::make_shared<const TestConfig>(std::move(cfg)),
    };
  }

  // Process entries, then services, each with its own entries.
  std::shared_ptr<PluginHost> makeHost(
      std::vector<config::PluginEntryConfig> process,
      std::map<std::string, std::vector<config::PluginEntryConfig>> services,
      std::chrono::milliseconds admission = std::chrono::milliseconds(5000),
      std::chrono::milliseconds authorize = std::chrono::milliseconds(5000)
  ) {
    config::Config cfg;
    cfg.plugins = std::move(process);
    for (auto& [name, entries] : services) {
      config::ServiceConfig svc;
      svc.plugins = std::move(entries);
      svc.admissionTimeout = admission;
      svc.authorizeTimeout = authorize;
      cfg.services.emplace(name, std::move(svc));
    }
    auto host = PluginHost::create(cfg);
    EXPECT_TRUE(host.hasValue());
    auto created = host.value()->createServices([this](std::string_view) {
      return folly::Executor::KeepAlive<>(&evb_);
    });
    EXPECT_TRUE(created.hasValue()) << (created.hasError() ? created.error() : "");
    return host.value();
  }

  template <class T> T run(folly::coro::Task<T> task) {
    return folly::coro::blockingWait(folly::coro::co_withExecutor(&evb_, std::move(task)), &evb_);
  }

  // Starts `task` on the event base without waiting for it.
  template <class T> folly::SemiFuture<folly::lift_unit_t<T>> spawn(folly::coro::Task<T> task) {
    return folly::coro::co_withExecutor(&evb_, std::move(task)).start();
  }

  void drain() {
    for (int i = 0; i < 20; ++i) {
      evb_.loopOnce(EVLOOP_NONBLOCK);
    }
  }

  template <class T> T await(folly::SemiFuture<T> future) {
    return std::move(future).via(&evb_).getVia(&evb_);
  }

  std::shared_ptr<PluginSession> session(PluginHost& host, std::string service = "live") {
    return host.newSession(service, "example.com", "/", 18, folly::getKeepAliveToken(&evb_));
  }

  // A setup message to borrow parameters from.
  std::shared_ptr<moxygen::Parameters> params_ =
      std::make_shared<moxygen::Parameters>(moxygen::FrameType::CLIENT_SETUP);

  Result<folly::Unit> admit(PluginSession& s) { return run(s.admit(params_.get(), params_)); }

  RequestArgs request(std::string track = "t") {
    tracks_.push_back(std::make_shared<std::string>(std::move(track)));
    return RequestArgs{
        .kind = RequestKind::Subscribe,
        .ns = namespace_,
        .track = *tracks_.back(),
        .params = params_.get(),
        .peer = false,
        .keepAlive = params_,
    };
  }

  folly::EventBase evb_;
  std::shared_ptr<Script> script_;
  std::shared_ptr<const PluginType> type_ = testType();
  std::vector<std::string> namespace_{"live", "room"};
  std::vector<std::shared_ptr<std::string>> tracks_;
};

TEST_F(PluginHostTest, ChainIsProcessThenServiceEachByRankThenListOrder) {
  auto host = makeHost(
      {entry("p1", 5), entry("p2", 0)},
      {{"live", {entry("s1", 0), entry("s2", -1), entry("s3", 0)}}}
  );
  std::vector<std::string> ids;
  for (const auto& member : host->chain("live")->members()) {
    ids.push_back(member.id);
  }
  EXPECT_THAT(ids, ElementsAre("p2", "p1", "s2", "s1", "s3"));
}

TEST_F(PluginHostTest, ServiceWithoutPluginsHasNoChain) {
  auto host = makeHost({}, {{"live", {}}});
  EXPECT_EQ(host->chain("live"), nullptr);
  EXPECT_EQ(session(*host), nullptr);
}

TEST_F(PluginHostTest, AdmitStopsAtFirstErrorAndEndsTheAdmittedOnes) {
  auto host = makeHost({entry("a")}, {{"live", {entry("b"), entry("c")}}});
  script_->admitErrors.wlock()->emplace("b", Error{Error::Kind::Forbidden, "no"});
  auto s = session(*host);
  auto result = admit(*s);
  ASSERT_TRUE(result.hasError());
  EXPECT_EQ(result.error().kind, Error::Kind::Forbidden);
  EXPECT_EQ(result.error().reason, "no");
  EXPECT_THAT(script_->take(), ElementsAre("admit:a@live", "admit:b@live"));

  s->end();
  drain();
  EXPECT_THAT(script_->take(), ElementsAre("ended:a:a"));
}

TEST_F(PluginHostTest, AuthorizeSeesEachPluginsOwnState) {
  auto host = makeHost({entry("a")}, {{"live", {entry("b")}}});
  auto s = session(*host);
  ASSERT_TRUE(admit(*s).hasValue());
  ASSERT_TRUE(run(s->authorize(request("t1"), {})).hasValue());
  EXPECT_THAT(
      *script_->authorized.rlock(),
      ElementsAre("a:subscribe:t1:-:a", "b:subscribe:t1:-:b")
  );

  script_->authorizeErrors.wlock()->emplace("a", Error{Error::Kind::ExpiredToken, "old"});
  auto denied = run(s->authorize(request("t2"), {}));
  ASSERT_TRUE(denied.hasError());
  EXPECT_EQ(denied.error().kind, Error::Kind::ExpiredToken);
  // The chain stopped at a.
  EXPECT_EQ(script_->authorized.rlock()->size(), 3);
}

TEST_F(PluginHostTest, InlineChainIsReadyOnReturn) {
  auto host = makeHost({}, {{"live", {entry("a")}}});
  auto s = session(*host);
  ASSERT_TRUE(admit(*s).hasValue());

  // Started on the session executor's thread, as the PUBLISH path does.
  bool inlineReady = false;
  run(folly::coro::co_invoke([&]() -> folly::coro::Task<void> {
    auto started = s->startAuthorize(request(), {});
    inlineReady = started.result.isReady();
    co_return;
  }));
  EXPECT_TRUE(inlineReady);

  script_->suspendAuthorize = true;
  folly::Optional<PluginSession::StartedCheck> pending;
  run(folly::coro::co_invoke([&]() -> folly::coro::Task<void> {
    pending = s->startAuthorize(request(), {});
    co_return;
  }));
  EXPECT_FALSE(pending->result.isReady());
  script_->authorizeGate.post();
  EXPECT_TRUE(run(s->awaitAuthorize(std::move(*pending))).hasValue());
}

TEST_F(PluginHostTest, AdmissionBudgetExpiresAndLateSuccessStillEnds) {
  auto host = makeHost({}, {{"live", {entry("a")}}}, std::chrono::milliseconds(30));
  script_->suspendAdmit = true;
  script_->ignoreCancellation = true;
  auto s = session(*host);
  auto result = admit(*s);
  ASSERT_TRUE(result.hasError());
  EXPECT_EQ(result.error().kind, Error::Kind::Internal);
  EXPECT_THAT(result.error().reason, HasSubstr("timed out"));

  // sessionEnded waits for the check that outlived its budget.
  s->end();
  drain();
  EXPECT_THAT(script_->take(), ElementsAre("admit:a@live"));
  script_->admitGate.post();
  drain();
  EXPECT_THAT(script_->take(), ElementsAre("ended:a:a"));
}

TEST_F(PluginHostTest, AuthorizeBudgetExpires) {
  auto host = makeHost(
      {},
      {{"live", {entry("a")}}},
      std::chrono::milliseconds(5000),
      std::chrono::milliseconds(30)
  );
  auto s = session(*host);
  ASSERT_TRUE(admit(*s).hasValue());
  script_->suspendAuthorize = true;
  auto result = run(s->authorize(request(), {}));
  ASSERT_TRUE(result.hasError());
  EXPECT_EQ(result.error().kind, Error::Kind::Internal);
  EXPECT_THAT(result.error().reason, HasSubstr("timed out"));
}

TEST_F(PluginHostTest, SessionEndStopsASuspendedCheckBeforeDispatch) {
  auto host = makeHost({}, {{"live", {entry("a")}}});
  auto s = session(*host);
  ASSERT_TRUE(admit(*s).hasValue());
  script_->take();
  script_->suspendAuthorize = true;
  script_->ignoreCancellation = true;
  auto check = spawn(s->authorize(request(), {}));
  drain();
  EXPECT_FALSE(check.isReady());

  s->end();
  drain();
  // sessionEnded waits for the check.
  EXPECT_THAT(script_->take(), IsEmpty());
  script_->authorizeGate.post();
  auto result = await(std::move(check));
  ASSERT_TRUE(result.hasError());
  EXPECT_THAT(result.error().reason, HasSubstr("cancelled"));
  drain();
  EXPECT_THAT(script_->take(), ElementsAre("ended:a:a"));
}

TEST_F(PluginHostTest, RequestCancellationStopsTheCheck) {
  auto host = makeHost({}, {{"live", {entry("a")}}});
  auto s = session(*host);
  ASSERT_TRUE(admit(*s).hasValue());
  script_->suspendAuthorize = true;
  folly::CancellationSource cancel;
  auto check = spawn(s->authorize(request(), cancel.getToken()));
  drain();
  cancel.requestCancellation();
  auto result = await(std::move(check));
  ASSERT_TRUE(result.hasError());
  EXPECT_THAT(result.error().reason, HasSubstr("cancelled"));
}

TEST_F(PluginHostTest, RootsStartInConfigOrderAndRollBackOnFailure) {
  config::Config cfg;
  cfg.plugins = {entry("a"), entry("b"), entry("c")};
  auto host = PluginHost::create(cfg).value();
  script_->startThrows.wlock()->emplace("b", true);
  auto started = run(host->startRoots({}));
  ASSERT_TRUE(started.hasError());
  EXPECT_THAT(started.error(), HasSubstr("plugin 'b' start failed"));
  EXPECT_THAT(script_->take(), ElementsAre("start:a", "start:b", "stop:a"));
}

TEST_F(PluginHostTest, StopWaitsForSessionsThenStopsInReverse) {
  auto host = makeHost({entry("p")}, {{"live", {entry("s")}}});
  ASSERT_TRUE(run(host->startRoots({})).hasValue());
  ASSERT_TRUE(run(host->startServices({})).hasValue());
  EXPECT_THAT(script_->take(), ElementsAre("start:p", "start:s", "start:p@live", "start:s@live"));

  auto s = session(*host);
  ASSERT_TRUE(admit(*s).hasValue());
  script_->take();
  auto stopped = spawn(host->stop());
  drain();
  EXPECT_FALSE(stopped.isReady());
  // No new session once stop() has begun.
  EXPECT_EQ(session(*host), nullptr);

  s->end();
  await(std::move(stopped));
  EXPECT_THAT(
      script_->take(),
      ElementsAre("ended:p:p", "ended:s:s", "stop:s@live", "stop:p@live", "stop:s", "stop:p")
  );
}

TEST_F(PluginHostTest, ListenerParamsMergeAndRejectConflicts) {
  config::Config cfg;
  cfg.plugins = {entry("a", 0, true), entry("b", 0, true)};
  auto host = PluginHost::create(cfg).value();
  script_->listenerParams["a"] = {SetupParam{0x1000, uint64_t{1}}};
  script_->listenerParams["b"] = {SetupParam{0x1002, std::string("x")}};
  auto params = run(host->listenerParams("main"));
  ASSERT_TRUE(params.hasValue()) << params.error();
  ASSERT_EQ(params->size(), 2);
  EXPECT_EQ((*params)[1].key, 0x1002);

  script_->listenerParams["b"] = {SetupParam{0x1000, uint64_t{2}}};
  auto duplicate = run(host->listenerParams("main"));
  ASSERT_TRUE(duplicate.hasError());
  EXPECT_THAT(duplicate.error(), HasSubstr("plugin 'b' declares SETUP parameter 4096, already"));

  script_->listenerParams["b"] = {
      SetupParam{folly::to_underlying(moxygen::SetupKey::HOP_ID), uint64_t{2}}
  };
  auto reserved = run(host->listenerParams("main"));
  ASSERT_TRUE(reserved.hasError());
  EXPECT_THAT(reserved.error(), HasSubstr("which the host writes"));
}

TEST_F(PluginHostTest, UpstreamParamsComeFromTheServiceChain) {
  script_->hooks = HookSet(Hook::UpstreamSetupParams);
  auto host = makeHost({entry("p")}, {{"live", {entry("s")}}, {"other", {}}});
  script_->upstreamParams["p"] = {SetupParam{0x2000, uint64_t{1}}};
  script_->upstreamParams["s"] = {SetupParam{0x2002, uint64_t{2}}};
  auto params = run(host->upstreamParams("live", "moqt://up"));
  ASSERT_TRUE(params.hasValue()) << params.error();
  EXPECT_EQ(params->size(), 2);
  auto other = run(host->upstreamParams("other", "moqt://up"));
  ASSERT_TRUE(other.hasValue());
  EXPECT_EQ(other->size(), 1);
}

TEST_F(PluginHostTest, SnapshotsReportErrorsAndStalls) {
  script_->hooks = HookSet(Hook::StateSnapshot);
  auto host = makeHost({}, {{"live", {entry("ok"), entry("bad")}}});
  script_->snapshotThrows.wlock()->emplace("bad", "boom");
  auto snapshots = run(host->snapshots("live", std::chrono::milliseconds(1000)));
  ASSERT_EQ(snapshots.size(), 2);
  EXPECT_EQ(snapshots[0].id, "ok");
  EXPECT_EQ(folly::parseJson(snapshots[0].json)["id"], "ok");
  ASSERT_TRUE(snapshots[1].error.has_value());
  EXPECT_THAT(*snapshots[1].error, HasSubstr("boom"));

  script_->snapshotStalls = true;
  auto stalled = run(host->snapshots("live", std::chrono::milliseconds(20)));
  ASSERT_EQ(stalled.size(), 2);
  EXPECT_EQ(stalled[0].error, "snapshot timed out");
  script_->snapshotGate.post();
  drain();
}

} // namespace
} // namespace openmoq::moqx::plugin::test
