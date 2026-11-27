/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "MoqxRelayContext.h"
#include "MoqxRelaySession.h"

#include <folly/coro/BlockingWait.h>
#include <folly/futures/Future.h>
#include <folly/io/async/ScopedEventBaseThread.h>
#include <folly/portability/GMock.h>
#include <folly/portability/GTest.h>
#include <moxygen/MoQTypes.h>
#include <moxygen/events/MoQFollyExecutorImpl.h>
#include <moxygen/test/MockMoQSession.h>

using namespace testing;
using namespace moxygen;
using namespace openmoq::moqx;

namespace {

using MatchEntry = config::ServiceConfig::MatchEntry;

constexpr std::string_view kListener = "main";

config::ServiceConfig
makeService(std::string authority, std::vector<std::string> listeners = {std::string(kListener)}) {
  return config::ServiceConfig{
      .match = {MatchEntry{
          .authority = MatchEntry::ExactAuthority{std::move(authority)},
          .path = MatchEntry::PrefixPath{"/"},
      }},
      .listeners = std::move(listeners),
      .cache = {100, 3},
  };
}

config::ServiceConfig makeAnyAuthorityService(std::vector<std::string> listeners) {
  return config::ServiceConfig{
      .match = {MatchEntry{
          .authority = MatchEntry::AnyAuthority{},
          .path = MatchEntry::PrefixPath{"/"},
      }},
      .listeners = std::move(listeners),
      .cache = {100, 3},
  };
}

// Rejects any session without a setup token, as UNAUTHORIZED.
config::AuthConfig requireSetupTokenAuth() {
  return config::AuthConfig{
      .enabled = true,
      .tokenType = 77,
      .hmacKeys = {config::AuthConfig::HmacKey{.id = "k1", .secret = "supersecretvalue"}},
      .requireSetupToken = true,
      .maxTokensPerMessage = 4,
  };
}

config::ServiceConfig makeServiceWithAuth(std::string authority, config::AuthConfig auth) {
  auto svc = makeService(std::move(authority));
  svc.auth = std::move(auth);
  return svc;
}

// Build a MockMoQSession with the given authority and path.
// Uses a shared executor so the session doesn't spin up its own thread.
std::shared_ptr<NiceMock<test::MockMoQSession>>
makeSession(std::shared_ptr<MoQExecutor> exec, std::string authority, std::string path = "") {
  auto session = std::make_shared<NiceMock<test::MockMoQSession>>(exec);
  session->setAuthority(std::move(authority));
  session->setPath(std::move(path));
  return session;
}

// A real MoqxRelaySession, so the service binding's dynamic_cast is exercised.
std::shared_ptr<MoqxRelaySession>
makeRelaySession(std::shared_ptr<MoQExecutor> exec, std::string authority) {
  auto session = std::make_shared<MoqxRelaySession>(
      std::string(kListener),
      folly::MaybeManagedPtr<proxygen::WebTransport>(nullptr),
      uint64_t{0},
      std::move(exec)
  );
  session->setAuthority(std::move(authority));
  return session;
}

class MoqxRelayContextTest : public ::testing::Test {
protected:
  void SetUp() override {
    folly::EventBase* evb = &evb_;
    exec_ = std::make_shared<MoQFollyExecutorImpl>(evb);
  }

  folly::EventBase evb_;
  std::shared_ptr<MoQExecutor> exec_;
  ClientSetup emptySetup_;
  uint64_t anyVersion_{0};
};

// --- validateAuthority ---

TEST_F(MoqxRelayContextTest, ValidateAuthority_Hit) {
  folly::F14FastMap<std::string, config::ServiceConfig> services = {
      {"svc", makeService("live.example.com")},
  };
  MoqxRelayContext ctx(services, "test-relay");

  auto session = makeSession(exec_, "live.example.com");
  auto result =
      ctx.validateAuthority(ctx.listenerMatcher(kListener), emptySetup_, anyVersion_, session);

  EXPECT_TRUE(result.hasValue());
}

TEST_F(MoqxRelayContextTest, ValidateAuthority_AppliesSubscriptionBufferLimit) {
  auto svc = makeService("live.example.com");
  svc.maxSubscriptionBufferKb = 64;
  MoqxRelayContext ctx({{"svc", svc}}, "test-relay");

  auto session = makeSession(exec_, "live.example.com");
  ASSERT_TRUE(
      ctx.validateAuthority(ctx.listenerMatcher(kListener), emptySetup_, anyVersion_, session)
          .hasValue()
  );
  EXPECT_EQ(session->getMoqSettings().bufferingThresholds.perSubscription, 64u * 1024);
}

TEST_F(MoqxRelayContextTest, ValidateAuthority_Miss) {
  folly::F14FastMap<std::string, config::ServiceConfig> services = {
      {"svc", makeService("live.example.com")},
  };
  MoqxRelayContext ctx(services, "test-relay");

  auto session = makeSession(exec_, "unknown.example.com");
  auto result =
      ctx.validateAuthority(ctx.listenerMatcher(kListener), emptySetup_, anyVersion_, session);

  ASSERT_FALSE(result.hasValue());
  EXPECT_EQ(result.error(), SessionCloseErrorCode::INVALID_AUTHORITY);
}

TEST_F(MoqxRelayContextTest, ValidateAuthority_EmptyAuthority_Miss) {
  folly::F14FastMap<std::string, config::ServiceConfig> services = {
      {"svc", makeService("live.example.com")},
  };
  MoqxRelayContext ctx(services, "test-relay");

  auto session = makeSession(exec_, "");
  auto result =
      ctx.validateAuthority(ctx.listenerMatcher(kListener), emptySetup_, anyVersion_, session);

  ASSERT_FALSE(result.hasValue());
  EXPECT_EQ(result.error(), SessionCloseErrorCode::INVALID_AUTHORITY);
}

// Two services with distinct authorities: each session is routed to the right
// one (and the wrong authority still misses).
TEST_F(MoqxRelayContextTest, ValidateAuthority_TwoServices) {
  folly::F14FastMap<std::string, config::ServiceConfig> services = {
      {"svc-a", makeService("a.example.com")},
      {"svc-b", makeService("b.example.com")},
  };
  MoqxRelayContext ctx(services, "test-relay");

  auto sessionA = makeSession(exec_, "a.example.com");
  auto sessionB = makeSession(exec_, "b.example.com");
  auto sessionC = makeSession(exec_, "c.example.com");

  EXPECT_TRUE(
      ctx.validateAuthority(ctx.listenerMatcher(kListener), emptySetup_, anyVersion_, sessionA)
          .hasValue()
  );
  EXPECT_TRUE(
      ctx.validateAuthority(ctx.listenerMatcher(kListener), emptySetup_, anyVersion_, sessionB)
          .hasValue()
  );

  auto miss =
      ctx.validateAuthority(ctx.listenerMatcher(kListener), emptySetup_, anyVersion_, sessionC);
  ASSERT_FALSE(miss.hasValue());
  EXPECT_EQ(miss.error(), SessionCloseErrorCode::INVALID_AUTHORITY);
}

// Path matching: sessions on the same authority but different paths route to
// the right service.
TEST_F(MoqxRelayContextTest, ValidateAuthority_PathRouting) {
  folly::F14FastMap<std::string, config::ServiceConfig> services = {
      {"relay",
       config::ServiceConfig{
           .match = {MatchEntry{
               .authority = MatchEntry::AnyAuthority{},
               .path = MatchEntry::ExactPath{"/moq-relay"},
           }},
           .listeners = {std::string(kListener)},
           .cache = {100, 3},
       }},
      {"live",
       config::ServiceConfig{
           .match = {MatchEntry{
               .authority = MatchEntry::AnyAuthority{},
               .path = MatchEntry::PrefixPath{"/live/"},
           }},
           .listeners = {std::string(kListener)},
           .cache = {100, 3},
       }},
  };
  MoqxRelayContext ctx(services, "test-relay");

  auto sessionRelay = makeSession(exec_, "any.host", "/moq-relay");
  auto sessionLive = makeSession(exec_, "any.host", "/live/stream");
  auto sessionOther = makeSession(exec_, "any.host", "/other");

  EXPECT_TRUE(
      ctx.validateAuthority(ctx.listenerMatcher(kListener), emptySetup_, anyVersion_, sessionRelay)
          .hasValue()
  );
  EXPECT_TRUE(
      ctx.validateAuthority(ctx.listenerMatcher(kListener), emptySetup_, anyVersion_, sessionLive)
          .hasValue()
  );

  auto miss =
      ctx.validateAuthority(ctx.listenerMatcher(kListener), emptySetup_, anyVersion_, sessionOther);
  ASSERT_FALSE(miss.hasValue());
  EXPECT_EQ(miss.error(), SessionCloseErrorCode::INVALID_AUTHORITY);
}

// --- validateAuthority: listener binding ---

// A service bound only to "internal" does not exist on "public".
TEST_F(MoqxRelayContextTest, ValidateAuthority_UnboundListenerMisses) {
  folly::F14FastMap<std::string, config::ServiceConfig> services = {
      {"ops", makeService("ops.example.com", {"internal"})},
      // Gives "public" a matcher, so the miss comes from the binding filter.
      {"live", makeService("live.example.com", {"public"})},
  };
  MoqxRelayContext ctx(services, "test-relay");

  auto onInternal = makeSession(exec_, "ops.example.com");
  EXPECT_TRUE(
      ctx.validateAuthority(ctx.listenerMatcher("internal"), emptySetup_, anyVersion_, onInternal)
          .hasValue()
  );

  auto onPublic = makeSession(exec_, "ops.example.com");
  auto miss =
      ctx.validateAuthority(ctx.listenerMatcher("public"), emptySetup_, anyVersion_, onPublic);
  ASSERT_FALSE(miss.hasValue());
  EXPECT_EQ(miss.error(), SessionCloseErrorCode::INVALID_AUTHORITY);
}

// "ops" does not exist on "public"; the catch-all bound there wins.
// "ops" requires a setup token and the catch-all does not: the result names the service.
TEST_F(MoqxRelayContextTest, ValidateAuthority_FallsThroughToBoundService) {
  auto ops = makeService("ops.example.com", {"internal"});
  ops.auth = requireSetupTokenAuth();
  folly::F14FastMap<std::string, config::ServiceConfig> services = {
      {"ops", std::move(ops)},
      {"public-catchall", makeAnyAuthorityService({"public"})},
  };
  MoqxRelayContext ctx(services, "test-relay");

  auto onPublic = makeSession(exec_, "ops.example.com");
  EXPECT_TRUE(
      ctx.validateAuthority(ctx.listenerMatcher("public"), emptySetup_, anyVersion_, onPublic)
          .hasValue()
  );

  auto onInternal = makeSession(exec_, "ops.example.com");
  auto toOps =
      ctx.validateAuthority(ctx.listenerMatcher("internal"), emptySetup_, anyVersion_, onInternal);
  ASSERT_FALSE(toOps.hasValue());
  EXPECT_EQ(toOps.error(), SessionCloseErrorCode::UNAUTHORIZED);
}

TEST_F(MoqxRelayContextTest, ValidateAuthority_ServiceOnTwoListeners) {
  folly::F14FastMap<std::string, config::ServiceConfig> services = {
      {"live", makeService("live.example.com", {"public", "internal"})},
  };
  MoqxRelayContext ctx(services, "test-relay");

  for (std::string_view listener : {"public", "internal"}) {
    auto session = makeSession(exec_, "live.example.com");
    EXPECT_TRUE(
        ctx.validateAuthority(ctx.listenerMatcher(listener), emptySetup_, anyVersion_, session)
            .hasValue()
    ) << listener;
  }
  auto onOther = makeSession(exec_, "live.example.com");
  auto miss =
      ctx.validateAuthority(ctx.listenerMatcher("other"), emptySetup_, anyVersion_, onOther);
  ASSERT_FALSE(miss.hasValue());
  EXPECT_EQ(miss.error(), SessionCloseErrorCode::INVALID_AUTHORITY);
}

// Identical rules on disjoint listeners: the listener picks the service.
// Only "ops" requires a setup token, so the result names the service.
TEST_F(MoqxRelayContextTest, ValidateAuthority_SameRuleOnDisjointListeners) {
  auto ops = makeAnyAuthorityService({"internal"});
  ops.auth = requireSetupTokenAuth();
  folly::F14FastMap<std::string, config::ServiceConfig> services = {
      {"ops", std::move(ops)},
      {"live", makeAnyAuthorityService({"public"})},
  };
  MoqxRelayContext ctx(services, "test-relay");

  auto onPublic = makeSession(exec_, "any.host");
  EXPECT_TRUE(
      ctx.validateAuthority(ctx.listenerMatcher("public"), emptySetup_, anyVersion_, onPublic)
          .hasValue()
  );

  auto onInternal = makeSession(exec_, "any.host");
  auto toOps =
      ctx.validateAuthority(ctx.listenerMatcher("internal"), emptySetup_, anyVersion_, onInternal);
  ASSERT_FALSE(toOps.hasValue());
  EXPECT_EQ(toOps.error(), SessionCloseErrorCode::UNAUTHORIZED);
}

TEST_F(MoqxRelayContextTest, ExactServicePathsAreScopedToListener) {
  folly::F14FastMap<std::string, config::ServiceConfig> services = {
      {"relay",
       config::ServiceConfig{
           .match = {MatchEntry{
               .authority = MatchEntry::AnyAuthority{},
               .path = MatchEntry::ExactPath{"/moq-relay"},
           }},
           .listeners = {"public", "internal"},
           .cache = {100, 3},
       }},
      {"ops",
       config::ServiceConfig{
           .match = {MatchEntry{
               .authority = MatchEntry::AnyAuthority{},
               .path = MatchEntry::ExactPath{"/ops"},
           }},
           .listeners = {"internal"},
           .cache = {100, 3},
       }},
  };
  MoqxRelayContext ctx(services, "test-relay");

  EXPECT_THAT(ctx.listenerMatcher("public").exactPaths(), UnorderedElementsAre("/moq-relay"));
  EXPECT_THAT(
      ctx.listenerMatcher("internal").exactPaths(),
      UnorderedElementsAre("/moq-relay", "/ops")
  );
  EXPECT_THAT(ctx.listenerMatcher("other").exactPaths(), IsEmpty());
}

// --- installServiceViews ---

namespace {
config::ServiceConfig makeExactPathService(std::string path) {
  return config::ServiceConfig{
      .match = {MatchEntry{
          .authority = MatchEntry::AnyAuthority{},
          .path = MatchEntry::ExactPath{std::move(path)},
      }},
      .listeners = {std::string(kListener)},
      .cache = {100, 3},
  };
}

template <typename F> auto runOn(folly::ScopedEventBaseThread& thread, F&& fn) {
  return folly::via(thread.getEventBase(), std::forward<F>(fn)).get();
}
} // namespace

TEST_F(MoqxRelayContextTest, InstallServiceViews_IoThreadResolvesAndRoutes) {
  MoqxRelayContext ctx({{"svc", makeService("live.example.com")}}, "test-relay");
  folly::ScopedEventBaseThread io;
  ctx.installServiceViews({folly::getKeepAliveToken(io.getEventBase())});

  auto matched = runOn(io, [&]() -> std::optional<std::string> {
    auto name = ctx.listenerMatcher(kListener).match("live.example.com", "/");
    return name ? std::optional<std::string>(*name) : std::nullopt;
  });
  EXPECT_EQ(matched, std::optional<std::string>("svc"));

  auto accepted = runOn(io, [&] {
    auto workerExec = std::make_shared<MoQFollyExecutorImpl>(io.getEventBase());
    auto session = makeSession(workerExec, "live.example.com");
    return ctx.validateAuthority(ctx.listenerMatcher(kListener), emptySetup_, anyVersion_, session)
        .hasValue();
  });
  EXPECT_TRUE(accepted);
}

TEST_F(MoqxRelayContextTest, InstallServiceViews_AdminThreadResolves) {
  MoqxRelayContext ctx({{"relay", makeExactPathService("/moq-relay")}}, "test-relay");
  folly::ScopedEventBaseThread admin;
  ctx.installServiceViews({folly::getKeepAliveToken(admin.getEventBase())});

  auto paths = runOn(admin, [&] { return ctx.listenerMatcher(kListener).exactPaths(); });
  EXPECT_THAT(paths, UnorderedElementsAre("/moq-relay"));
}

// A thread that was not given a view matches nothing, even after others were.
TEST_F(MoqxRelayContextTest, InstallServiceViews_UninstalledThreadMatchesNothing) {
  MoqxRelayContext ctx({{"relay", makeExactPathService("/moq-relay")}}, "test-relay");
  folly::ScopedEventBaseThread installed;
  folly::ScopedEventBaseThread other;
  ctx.installServiceViews({folly::getKeepAliveToken(installed.getEventBase())});

  auto matched = runOn(other, [&] {
    return ctx.listenerMatcher(kListener).match("any.host", "/moq-relay").has_value();
  });
  EXPECT_FALSE(matched);
  EXPECT_THAT(runOn(other, [&] { return ctx.listenerMatcher(kListener).exactPaths(); }), IsEmpty());
}

TEST_F(MoqxRelayContextTest, InstallServiceViews_EveryTargetGetsAView) {
  MoqxRelayContext ctx({{"svc", makeService("live.example.com")}}, "test-relay");
  folly::ScopedEventBaseThread io1;
  folly::ScopedEventBaseThread io2;
  folly::ScopedEventBaseThread admin;
  ctx.installServiceViews(
      {folly::getKeepAliveToken(io1.getEventBase()),
       folly::getKeepAliveToken(io2.getEventBase()),
       folly::getKeepAliveToken(admin.getEventBase())}
  );

  for (auto* thread : {&io1, &io2, &admin}) {
    EXPECT_TRUE(runOn(*thread, [&] {
      return ctx.listenerMatcher(kListener).match("live.example.com", "/").has_value();
    }));
  }
}

// ready() is per calling thread: it needs initUpstreams and that thread's view.
TEST_F(MoqxRelayContextTest, Ready_IsPerThread) {
  MoqxRelayContext ctx({{"svc", makeService("live.example.com")}}, "test-relay");
  folly::ScopedEventBaseThread worker;
  folly::ScopedEventBaseThread other;
  EXPECT_FALSE(ctx.ready());

  ctx.initUpstreams(worker.getEventBase());
  EXPECT_TRUE(ctx.ready());
  EXPECT_FALSE(runOn(worker, [&] { return ctx.ready(); }));

  ctx.installServiceViews({folly::getKeepAliveToken(worker.getEventBase())});
  EXPECT_TRUE(runOn(worker, [&] { return ctx.ready(); }));
  EXPECT_FALSE(runOn(other, [&] { return ctx.ready(); }));
}

// Blocking on the loop we are running on would deadlock.
TEST_F(MoqxRelayContextTest, InstallServiceViews_RejectsCallingFromTargetThread) {
  MoqxRelayContext ctx({{"svc", makeService("live.example.com")}}, "test-relay");
  EXPECT_DEATH(
      {
        folly::ScopedEventBaseThread io;
        runOn(io, [&] { ctx.installServiceViews({folly::getKeepAliveToken(io.getEventBase())}); });
      },
      "must not be called from a target EventBase's own thread"
  );
}

// --- validateAuthority: anonymous claim ---

TEST_F(
    MoqxRelayContextTest,
    ValidateAuthority_AnonymousClaimAllowsConnectWithoutSetupTokenWhenNotRequired
) {
  folly::F14FastMap<std::string, config::ServiceConfig> services = {
      {"svc",
       makeServiceWithAuth(
           "live.example.com",
           config::AuthConfig{
               .enabled = true,
               .tokenType = 77,
               .hmacKeys = {config::AuthConfig::HmacKey{.id = "k1", .secret = "supersecretvalue"}},
               .requireSetupToken = false,
               .anonymousClaim =
                   {config::AuthConfig::AnonymousScope{.actions = {auth::Action::Subscribe}}},
               .maxTokensPerMessage = 4,
           }
       )},
  };
  MoqxRelayContext ctx(services, "test-relay");

  auto session = makeSession(exec_, "live.example.com");
  auto result =
      ctx.validateAuthority(ctx.listenerMatcher(kListener), emptySetup_, anyVersion_, session);

  EXPECT_TRUE(result.hasValue());
}

TEST_F(MoqxRelayContextTest, ValidateAuthority_AnonymousClaimDoesNotBypassRequiredSetupToken) {
  folly::F14FastMap<std::string, config::ServiceConfig> services = {
      {"svc",
       makeServiceWithAuth(
           "live.example.com",
           config::AuthConfig{
               .enabled = true,
               .tokenType = 77,
               .hmacKeys = {config::AuthConfig::HmacKey{.id = "k1", .secret = "supersecretvalue"}},
               .requireSetupToken = true,
               .anonymousClaim =
                   {config::AuthConfig::AnonymousScope{.actions = {auth::Action::Subscribe}}},
               .maxTokensPerMessage = 4,
           }
       )},
  };
  MoqxRelayContext ctx(services, "test-relay");

  auto session = makeSession(exec_, "live.example.com");
  auto result =
      ctx.validateAuthority(ctx.listenerMatcher(kListener), emptySetup_, anyVersion_, session);

  ASSERT_FALSE(result.hasValue());
  EXPECT_EQ(result.error(), SessionCloseErrorCode::UNAUTHORIZED);
}

// --- session-to-service binding ---

TEST_F(MoqxRelayContextTest, ValidateAuthority_BindsMatchedService) {
  MoqxRelayContext ctx(
      {{"svc-a", makeService("a.example.com")}, {"svc-b", makeService("b.example.com")}},
      "test-relay"
  );

  auto session = makeRelaySession(exec_, "b.example.com");
  ASSERT_TRUE(
      ctx.validateAuthority(ctx.listenerMatcher(kListener), emptySetup_, anyVersion_, session)
          .hasValue()
  );
  EXPECT_EQ(MoqxRelaySession::serviceOf(*session), "svc-b");
}

TEST_F(MoqxRelayContextTest, OnSessionEnd_KeepsBindingInsteadOfRematching) {
  MoqxRelayContext ctx(
      {{"svc-a", makeService("a.example.com")}, {"svc-b", makeService("b.example.com")}},
      "test-relay"
  );
  auto session = makeRelaySession(exec_, "a.example.com");
  ASSERT_TRUE(
      ctx.validateAuthority(ctx.listenerMatcher(kListener), emptySetup_, anyVersion_, session)
          .hasValue()
  );

  // Stands in for a service added after SETUP that would now match this session.
  session->setAuthority("b.example.com");
  ctx.onSessionEnd(session);

  EXPECT_EQ(MoqxRelaySession::serviceOf(*session), "svc-a");
}

TEST_F(MoqxRelayContextTest, ValidateAuthority_FailedSetupBindsNothing) {
  MoqxRelayContext ctx(
      {{"svc", makeServiceWithAuth("a.example.com", requireSetupTokenAuth())}},
      "test-relay"
  );

  auto session = makeRelaySession(exec_, "a.example.com");
  auto result =
      ctx.validateAuthority(ctx.listenerMatcher(kListener), emptySetup_, anyVersion_, session);

  ASSERT_FALSE(result.hasValue());
  EXPECT_EQ(result.error(), SessionCloseErrorCode::UNAUTHORIZED);
  EXPECT_TRUE(MoqxRelaySession::serviceOf(*session).empty());
  ctx.onSessionEnd(session);
}

TEST_F(MoqxRelayContextTest, ValidateAuthority_NoMatchBindsNothing) {
  MoqxRelayContext ctx({{"svc", makeService("a.example.com")}}, "test-relay");

  auto session = makeRelaySession(exec_, "unknown.example.com");
  auto result =
      ctx.validateAuthority(ctx.listenerMatcher(kListener), emptySetup_, anyVersion_, session);

  ASSERT_FALSE(result.hasValue());
  EXPECT_EQ(result.error(), SessionCloseErrorCode::INVALID_AUTHORITY);
  EXPECT_TRUE(MoqxRelaySession::serviceOf(*session).empty());
}

TEST_F(MoqxRelayContextTest, ValidateAuthority_RepeatDoesNotRebind) {
  MoqxRelayContext ctx(
      {{"svc-a", makeService("a.example.com")}, {"svc-b", makeService("b.example.com")}},
      "test-relay"
  );
  auto session = makeRelaySession(exec_, "a.example.com");
  ASSERT_TRUE(
      ctx.validateAuthority(ctx.listenerMatcher(kListener), emptySetup_, anyVersion_, session)
          .hasValue()
  );

  session->setAuthority("b.example.com");
  ASSERT_TRUE(
      ctx.validateAuthority(ctx.listenerMatcher(kListener), emptySetup_, anyVersion_, session)
          .hasValue()
  );
  EXPECT_EQ(MoqxRelaySession::serviceOf(*session), "svc-a");
}

// --- purgeCache ---

TEST_F(MoqxRelayContextTest, PurgeCache_TwoServicesRequireName) {
  folly::F14FastMap<std::string, config::ServiceConfig> services = {
      {"svc-a", makeService("a.example.com")},
      {"svc-b", makeService("b.example.com")},
  };
  MoqxRelayContext ctx(services, "test-relay");
  folly::ScopedEventBaseThread worker;
  ctx.initUpstreams(worker.getEventBase());
  FullTrackName ftn{TrackNamespace({"live"}), "video"};

  auto unnamed = folly::coro::blockingWait(ctx.purgeCache("", ftn, std::nullopt));
  ASSERT_TRUE(unnamed.hasError());
  EXPECT_EQ(unnamed.error(), MoqxRelayContext::PurgeError::ServiceRequired);

  auto unknown = folly::coro::blockingWait(ctx.purgeCache("svc-c", ftn, std::nullopt));
  ASSERT_TRUE(unknown.hasError());
  EXPECT_EQ(unknown.error(), MoqxRelayContext::PurgeError::UnknownService);

  auto named = folly::coro::blockingWait(ctx.purgeCache("svc-a", ftn, std::nullopt));
  ASSERT_TRUE(named.hasValue());
  EXPECT_EQ(*named, 0u);
}

// --- upstreams ---

TEST_F(MoqxRelayContextTest, UnreadableUpstreamCACertFailsStartup) {
  auto svc = makeService("a.example.com");
  svc.upstream = config::UpstreamConfig{
      .url = "moqt://origin.example.com:4433/",
      .tls = config::UpstreamTlsConfig{.caCertFile = "/nonexistent/ca.pem"},
  };
  MoqxRelayContext ctx({{"svc-a", svc}}, "test-relay");
  folly::ScopedEventBaseThread worker;
  EXPECT_THAT(
      [&] { ctx.initUpstreams(worker.getEventBase()); },
      testing::ThrowsMessage<std::runtime_error>(
          testing::HasSubstr("upstream.tls.ca_cert: cannot load /nonexistent/ca.pem")
      )
  );
}

config::UpstreamConfig makeUpstream(std::string url, config::UpstreamTlsConfig tls = {}) {
  return config::UpstreamConfig{.url = std::move(url), .tls = std::move(tls)};
}

TEST_F(MoqxRelayContextTest, BuildUpstreamVerifiersReportsABadCAInsteadOfThrowing) {
  auto svc = makeService("a.example.com");
  svc.upstreams = {
      makeUpstream("moqt://one.example.com:4433/", {.insecure = true}),
      makeUpstream("moqt://two.example.com:4433/", {.caCertFile = "/nonexistent/ca.pem"}),
      makeUpstream("moqt://three.example.com:4433/"),
  };
  folly::Expected<UpstreamVerifiers, std::string> result;
  EXPECT_NO_THROW(result = buildUpstreamVerifiers(svc));
  ASSERT_TRUE(result.hasError());
  EXPECT_THAT(result.error(), testing::HasSubstr("upstreams[1]"));
  EXPECT_THAT(result.error(), testing::HasSubstr("moqt://two.example.com:4433/"));
  EXPECT_THAT(result.error(), testing::HasSubstr("cannot load /nonexistent/ca.pem"));
}

TEST_F(MoqxRelayContextTest, BuildUpstreamVerifiersKeepsUpstreamOrderAndNullMeansSystemCAs) {
  auto svc = makeService("a.example.com");
  svc.upstreams = {
      makeUpstream("moqt://one.example.com:4433/", {.insecure = true}),
      makeUpstream("moqt://two.example.com:4433/"),
      makeUpstream("moqt://three.example.com:4433/", {.insecure = true}),
  };
  auto result = buildUpstreamVerifiers(svc);
  ASSERT_TRUE(result.hasValue());
  ASSERT_EQ(result->size(), 3u);
  EXPECT_NE((*result)[0], nullptr);
  EXPECT_EQ((*result)[1], nullptr);
  EXPECT_NE((*result)[2], nullptr);
}

TEST_F(MoqxRelayContextTest, BuildUpstreamVerifiersHandlesTheLegacySingleUpstreamAndNone) {
  auto svc = makeService("a.example.com");
  auto none = buildUpstreamVerifiers(svc);
  ASSERT_TRUE(none.hasValue());
  EXPECT_TRUE(none->empty());

  svc.upstream = makeUpstream("moqt://origin.example.com:4433/", {.insecure = true});
  auto single = buildUpstreamVerifiers(svc);
  ASSERT_TRUE(single.hasValue());
  ASSERT_EQ(single->size(), 1u);
  EXPECT_NE((*single)[0], nullptr);
}

} // namespace
