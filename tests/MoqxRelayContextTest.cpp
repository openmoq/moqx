#include <moqx/MoqxRelayContext.h>

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

config::ServiceConfig makeService(std::string authority) {
  return config::ServiceConfig{
      .match = {MatchEntry{
          .authority = MatchEntry::ExactAuthority{std::move(authority)},
          .path = MatchEntry::PrefixPath{"/"},
      }},
      .cache = {100, 3},
  };
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
  auto result = ctx.validateAuthority(emptySetup_, anyVersion_, session);

  EXPECT_TRUE(result.hasValue());
}

TEST_F(MoqxRelayContextTest, ValidateAuthority_Miss) {
  folly::F14FastMap<std::string, config::ServiceConfig> services = {
      {"svc", makeService("live.example.com")},
  };
  MoqxRelayContext ctx(services, "test-relay");

  auto session = makeSession(exec_, "unknown.example.com");
  auto result = ctx.validateAuthority(emptySetup_, anyVersion_, session);

  ASSERT_FALSE(result.hasValue());
  EXPECT_EQ(result.error(), SessionCloseErrorCode::INVALID_AUTHORITY);
}

TEST_F(MoqxRelayContextTest, ValidateAuthority_EmptyAuthority_Miss) {
  folly::F14FastMap<std::string, config::ServiceConfig> services = {
      {"svc", makeService("live.example.com")},
  };
  MoqxRelayContext ctx(services, "test-relay");

  auto session = makeSession(exec_, "");
  auto result = ctx.validateAuthority(emptySetup_, anyVersion_, session);

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

  EXPECT_TRUE(ctx.validateAuthority(emptySetup_, anyVersion_, sessionA).hasValue());
  EXPECT_TRUE(ctx.validateAuthority(emptySetup_, anyVersion_, sessionB).hasValue());

  auto miss = ctx.validateAuthority(emptySetup_, anyVersion_, sessionC);
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
           .cache = {100, 3},
       }},
      {"live",
       config::ServiceConfig{
           .match = {MatchEntry{
               .authority = MatchEntry::AnyAuthority{},
               .path = MatchEntry::PrefixPath{"/live/"},
           }},
           .cache = {100, 3},
       }},
  };
  MoqxRelayContext ctx(services, "test-relay");

  auto sessionRelay = makeSession(exec_, "any.host", "/moq-relay");
  auto sessionLive = makeSession(exec_, "any.host", "/live/stream");
  auto sessionOther = makeSession(exec_, "any.host", "/other");

  EXPECT_TRUE(ctx.validateAuthority(emptySetup_, anyVersion_, sessionRelay).hasValue());
  EXPECT_TRUE(ctx.validateAuthority(emptySetup_, anyVersion_, sessionLive).hasValue());

  auto miss = ctx.validateAuthority(emptySetup_, anyVersion_, sessionOther);
  ASSERT_FALSE(miss.hasValue());
  EXPECT_EQ(miss.error(), SessionCloseErrorCode::INVALID_AUTHORITY);
}

} // namespace
