/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

// cat4moq-auth through the plugin host and the request filters, as a session
// sees it.

#include "plugins/cat4moq-auth/Cat4moqAuth.h"
#include "auth/AuthTokenIssuer.h"
#include "config/ConfigSerializer.h"
#include "config/loader/ConfigResolver.h"
#include "plugin/Host.h"
#include "relay/PluginFilters.h"

#include <folly/coro/BlockingWait.h>
#include <folly/io/async/EventBase.h>
#include <folly/portability/GMock.h>
#include <folly/portability/GTest.h>
#include <moxygen/test/Mocks.h>
#include <rfl/yaml.hpp>

namespace openmoq::moqx {
namespace {

using namespace moxygen;
using auth::Action;
using ::testing::_;
using ::testing::HasSubstr;
using ::testing::NiceMock;

constexpr uint64_t kTokenType = 77;
// The relay peering token's type (UpstreamProvider.cpp).
constexpr uint64_t kRelayTokenType = 0x1B2C'3D4E'5F6A'7B8CULL;

auth::Grants
grantsFor(std::vector<Action> actions, std::chrono::seconds ttl = std::chrono::hours(1)) {
  auth::Grants grants;
  grants.expiresAt = std::chrono::system_clock::now() + ttl;
  grants.scopes.push_back(auth::Scope{.actions = std::move(actions)});
  return grants;
}

AuthToken tokenFor(const auth::Grants& grants, uint64_t type = kTokenType) {
  return AuthToken{
      .tokenType = type,
      .tokenValue = auth::signGrants("k1", "secret-secret-1", grants),
      .alias = AuthToken::DontRegister,
  };
}

Parameters withTokens(FrameType frameType, std::vector<AuthToken> tokens) {
  Parameters params(frameType);
  for (auto& token : tokens) {
    EXPECT_TRUE(params
                    .insertParam(Parameter(
                        folly::to_underlying(TrackRequestParamKey::AUTHORIZATION_TOKEN),
                        std::move(token)
                    ))
                    .hasValue());
  }
  return params;
}

plugins::cat4moq::Config baseConfig() {
  plugins::cat4moq::Config config;
  config.token_type = kTokenType;
  config.hmac_keys = {{.id = "k1", .secret = "secret-secret-1"}};
  config.max_tokens_per_message = 4;
  return config;
}

class Cat4moqAuthTest : public ::testing::Test {
protected:
  void start(const plugins::cat4moq::Config& pluginConfig) {
    config::Config cfg;
    config::ServiceConfig svc;
    svc.plugins = {config::PluginEntryConfig{
        .type = std::string(plugins::cat4moq::kType),
        .id = "auth",
        .pluginType = plugins::cat4moq::type(),
        .config = std::make_shared<const plugins::cat4moq::Config>(pluginConfig),
    }};
    cfg.services.emplace("live", std::move(svc));
    host_ = plugin::PluginHost::create(cfg).value();
    ASSERT_TRUE(host_
                    ->createServices([this](std::string_view) {
                      return folly::Executor::KeepAlive<>(&evb_);
                    })
                    .hasValue());
  }

  template <class T> T run(folly::coro::Task<T> task) {
    return folly::coro::blockingWait(folly::coro::co_withExecutor(&evb_, std::move(task)), &evb_);
  }

  // Admits a session presenting `setupTokens`.
  plugin::Result<folly::Unit> admit(std::vector<AuthToken> setupTokens) {
    session_ = host_->newSession("live", "example.com", "/", 18, folly::getKeepAliveToken(&evb_));
    auto setup = std::make_shared<Parameters>(withTokens(FrameType::CLIENT_SETUP, setupTokens));
    auto result = run(session_->admit(setup.get(), setup));
    pub_ = std::make_shared<PluginPublisherFilter>(publisher_, session_, peering_);
    sub_ = std::make_shared<PluginSubscriberFilter>(subscriber_, session_, peering_);
    return result;
  }

  void expectSubscribeReachesRelay() {
    EXPECT_CALL(*publisher_, subscribe(_, _))
        .WillOnce(
            [](SubscribeRequest req,
               std::shared_ptr<TrackConsumer>) -> folly::coro::Task<Publisher::SubscribeResult> {
              SubscribeOk ok;
              ok.requestID = req.requestID;
              co_return std::make_shared<NiceMock<MockSubscriptionHandle>>(ok);
            }
        );
  }

  void expectPublishReachesRelay() {
    EXPECT_CALL(*subscriber_, publish(_, _))
        .WillOnce(
            [](PublishRequest req,
               std::shared_ptr<SubscriptionHandle>) -> Subscriber::PublishResult {
              PublishOk ok;
              ok.requestID = req.requestID;
              return Subscriber::PublishConsumerAndReplyTask{
                  .consumer = nullptr,
                  .reply =
                      folly::coro::makeTask<folly::Expected<PublishOk, PublishError>>(std::move(ok)
                      ),
              };
            }
        );
  }

  Publisher::SubscribeResult subscribe(Parameters params = Parameters(FrameType::SUBSCRIBE)) {
    SubscribeRequest req;
    req.requestID = RequestID(1);
    req.fullTrackName = FullTrackName{TrackNamespace{{"live"}}, "video"};
    req.params = std::move(params);
    return run(pub_->subscribe(std::move(req), nullptr));
  }

  Subscriber::PublishResult publish(Parameters params = Parameters(FrameType::PUBLISH)) {
    PublishRequest req;
    req.requestID = RequestID(2);
    req.fullTrackName = FullTrackName{TrackNamespace{{"live"}}, "video"};
    req.params = std::move(params);
    return sub_->publish(std::move(req), nullptr);
  }

  folly::EventBase evb_;
  std::shared_ptr<plugin::PluginHost> host_;
  std::shared_ptr<plugin::PluginSession> session_;
  bool peering_{false};
  std::shared_ptr<NiceMock<MockPublisher>> publisher_ = std::make_shared<NiceMock<MockPublisher>>();
  std::shared_ptr<NiceMock<MockSubscriber>> subscriber_ =
      std::make_shared<NiceMock<MockSubscriber>>();
  std::shared_ptr<PluginPublisherFilter> pub_;
  std::shared_ptr<PluginSubscriberFilter> sub_;
};

TEST_F(Cat4moqAuthTest, SubscribeSucceedsWhenAnySessionGrantCoversIt) {
  start(baseConfig());
  ASSERT_TRUE(admit({tokenFor(grantsFor({Action::ClientSetup, Action::Publish})),
                     tokenFor(grantsFor({Action::Subscribe}))})
                  .hasValue());
  expectSubscribeReachesRelay();
  EXPECT_TRUE(subscribe().hasValue());
}

TEST_F(Cat4moqAuthTest, SubscribeFailsWhenNoSessionGrantCoversIt) {
  start(baseConfig());
  ASSERT_TRUE(admit({tokenFor(grantsFor({Action::ClientSetup, Action::Publish}))}).hasValue());
  auto result = subscribe();
  ASSERT_TRUE(result.hasError());
  EXPECT_EQ(result.error().errorCode, SubscribeErrorCode::UNAUTHORIZED);
  EXPECT_EQ(result.error().reasonPhrase, "authorization token does not permit action");
}

// A malformed request token is dropped as a candidate; it does not block a
// session grant that permits the request.
TEST_F(Cat4moqAuthTest, GarbageRequestTokenDoesNotBlockSessionGrant) {
  start(baseConfig());
  ASSERT_TRUE(admit({tokenFor(grantsFor({Action::ClientSetup, Action::Subscribe}))}).hasValue());
  expectSubscribeReachesRelay();
  auto garbage = AuthToken{
      .tokenType = kTokenType,
      .tokenValue = std::string("\xde\xad\xbe\xef", 4),
      .alias = AuthToken::DontRegister,
  };
  EXPECT_TRUE(subscribe(withTokens(FrameType::SUBSCRIBE, {garbage})).hasValue());
}

TEST_F(Cat4moqAuthTest, PublishNeedsAGrantOrARequestToken) {
  start(baseConfig());
  ASSERT_TRUE(admit({tokenFor(grantsFor({Action::ClientSetup, Action::Subscribe}))}).hasValue());
  auto rejected = publish();
  ASSERT_TRUE(rejected.hasError());
  EXPECT_EQ(rejected.error().errorCode, PublishErrorCode::UNAUTHORIZED);

  expectPublishReachesRelay();
  EXPECT_TRUE(
      publish(withTokens(FrameType::PUBLISH, {tokenFor(grantsFor({Action::Publish}))})).hasValue()
  );
}

// The anonymous claim is a floor for every request, without any setup token.
TEST_F(Cat4moqAuthTest, AnonymousClaimCoversSubscribeButNotPublish) {
  auto config = baseConfig();
  config.require_setup_token = false;
  config.anonymous_claim = std::vector<plugins::cat4moq::Config::AnonymousScope>{
      {.actions = {"subscribe", "fetch"}},
  };
  start(config);
  ASSERT_TRUE(admit({}).hasValue());
  expectSubscribeReachesRelay();
  EXPECT_TRUE(subscribe().hasValue());
  auto rejected = publish();
  ASSERT_TRUE(rejected.hasError());
  EXPECT_EQ(rejected.error().errorCode, PublishErrorCode::UNAUTHORIZED);
}

TEST_F(Cat4moqAuthTest, AnonymousClaimDoesNotBypassARequiredSetupToken) {
  auto config = baseConfig();
  config.anonymous_claim = std::vector<plugins::cat4moq::Config::AnonymousScope>{
      {.actions = {"subscribe"}},
  };
  start(config);
  auto result = admit({});
  ASSERT_TRUE(result.hasError());
  EXPECT_EQ(result.error().kind, plugin::Error::Kind::Unauthorized);
  EXPECT_EQ(toCloseCode(result.error().kind), SessionCloseErrorCode::UNAUTHORIZED);
}

TEST_F(Cat4moqAuthTest, SetupRejectionsKeepTheirCloseCodes) {
  start(baseConfig());
  auto expired = admit({tokenFor(grantsFor({Action::ClientSetup}, std::chrono::seconds(-10)))});
  ASSERT_TRUE(expired.hasError());
  EXPECT_EQ(toCloseCode(expired.error().kind), SessionCloseErrorCode::EXPIRED_AUTH_TOKEN);

  auto malformed = admit({AuthToken{
      .tokenType = kTokenType,
      .tokenValue = "",
      .alias = AuthToken::DontRegister,
  }});
  ASSERT_TRUE(malformed.hasError());
  EXPECT_EQ(toCloseCode(malformed.error().kind), SessionCloseErrorCode::MALFORMED_AUTH_TOKEN);

  auto tooMany = admit(std::vector<AuthToken>(5, tokenFor(grantsFor({Action::ClientSetup}))));
  ASSERT_TRUE(tooMany.hasError());
  EXPECT_EQ(toCloseCode(tooMany.error().kind), SessionCloseErrorCode::UNAUTHORIZED);
}

// Peer relays carry the relay token; only namespace interest skips grants.
TEST_F(Cat4moqAuthTest, PeerBypassCoversOnlyNamespaceInterestWithPeering) {
  peering_ = true;
  start(baseConfig());
  ASSERT_TRUE(admit({tokenFor(grantsFor({Action::ClientSetup}))}).hasValue());
  auto relayToken = AuthToken{
      .tokenType = kRelayTokenType,
      .tokenValue = "peer-relay",
      .alias = AuthToken::DontRegister,
  };

  EXPECT_CALL(*publisher_, subscribeNamespace(_, _))
      .WillOnce(
          [](SubscribeNamespace req, std::shared_ptr<Publisher::NamespacePublishHandle>)
              -> folly::coro::Task<Publisher::SubscribeNamespaceResult> {
            co_return folly::makeUnexpected(SubscribeNamespaceError{
                req.requestID,
                SubscribeNamespaceErrorCode::INTERNAL_ERROR,
                "reached the relay"
            });
          }
      );
  SubscribeNamespace subNs;
  subNs.requestID = RequestID(3);
  subNs.trackNamespacePrefix = TrackNamespace{{"live"}};
  subNs.params = withTokens(FrameType::SUBSCRIBE_NAMESPACE, {relayToken});
  auto bypassed = run(pub_->subscribeNamespace(subNs, nullptr));
  ASSERT_TRUE(bypassed.hasError());
  EXPECT_EQ(bypassed.error().reasonPhrase, "reached the relay");

  // A SUBSCRIBE with the relay token is still checked.
  auto subscribed = subscribe(withTokens(FrameType::SUBSCRIBE, {relayToken}));
  ASSERT_TRUE(subscribed.hasError());
  EXPECT_EQ(subscribed.error().errorCode, SubscribeErrorCode::UNAUTHORIZED);
}

TEST_F(Cat4moqAuthTest, NoPeerBypassWithoutPeering) {
  start(baseConfig());
  ASSERT_TRUE(admit({tokenFor(grantsFor({Action::ClientSetup}))}).hasValue());
  SubscribeNamespace subNs;
  subNs.requestID = RequestID(3);
  subNs.trackNamespacePrefix = TrackNamespace{{"live"}};
  subNs.params = withTokens(
      FrameType::SUBSCRIBE_NAMESPACE,
      {AuthToken{.tokenType = kRelayTokenType, .tokenValue = "peer-relay"}}
  );
  auto result = run(pub_->subscribeNamespace(subNs, nullptr));
  ASSERT_TRUE(result.hasError());
  EXPECT_EQ(result.error().errorCode, SubscribeNamespaceErrorCode::UNAUTHORIZED);
}

// --- Configuration ---

constexpr std::string_view kYaml = R"(
listeners:
  - name: main
    udp: {socket: {address: "::", port: 9668}}
    tls: {insecure: true}
    endpoint: "/moq-relay"
services:
  live:
    match:
      - authority: {any: true}
        path: {prefix: "/"}
    cache: {enabled: false, max_tracks: 1, max_groups_per_track: 1}
    max_subscription_buffer_kb: 1024
)";

folly::Expected<config::ResolvedConfig, std::string> resolveYaml(std::string extra) {
  auto parsed = rfl::yaml::read<config::ParsedConfig>(std::string(kYaml) + extra);
  if (!parsed) {
    return folly::makeUnexpected("parse: " + parsed.error().what());
  }
  return config::resolveConfig(*parsed);
}

constexpr std::string_view kEntry = R"(    plugins:
      - type: cat4moq-auth
        config:
          token_type: 77
          hmac_keys: [{id: k1, secret: secret-secret-1}]
)";

TEST(Cat4moqAuthConfigTest, MaxTokensComesFromTheEntryOrServiceDefaults) {
  auto missing = resolveYaml(std::string(kEntry));
  ASSERT_TRUE(missing.hasError());
  EXPECT_THAT(missing.error(), HasSubstr("max_tokens_per_message is required"));

  auto inherited = resolveYaml(
      std::string(kEntry) + "service_defaults:\n  plugins:\n    cat4moq-auth: "
                            "{max_tokens_per_message: 3}\n"
  );
  ASSERT_TRUE(inherited.hasValue()) << inherited.error();
  const auto& entry = inherited->config.services.at("live").plugins.at(0);
  auto resolved =
      plugins::cat4moq::resolve(*static_cast<const plugins::cat4moq::Config*>(entry.config.get()));
  ASSERT_TRUE(resolved.hasValue());
  EXPECT_EQ(resolved->maxTokensPerMessage, 3);
}

TEST(Cat4moqAuthConfigTest, ReportsEveryProblem) {
  auto config = baseConfig();
  config.hmac_keys = {{.id = "", .secret = ""}};
  config.token_type = uint64_t{1} << 62;
  config.max_tokens_per_message = 0;
  config.anonymous_claim = std::vector<plugins::cat4moq::Config::AnonymousScope>{
      {.actions = {"client_setup", "nope"}},
      {.actions = {}},
  };
  auto resolved = plugins::cat4moq::resolve(config);
  ASSERT_TRUE(resolved.hasError());
  for (const auto* problem :
       {"hmac_keys[0].id must be non-empty",
        "hmac_keys[0].secret must be non-empty",
        "token_type must fit in a QUIC varint",
        "max_tokens_per_message must be >= 1",
        "anonymous_claim[0].actions: 'client_setup' is not allowed here",
        "anonymous_claim[0].actions: 'nope' is not a recognized CAT4MOQ action",
        "anonymous_claim[1].actions must be non-empty"}) {
    EXPECT_THAT(resolved.error(), HasSubstr(problem));
  }
}

TEST(Cat4moqAuthConfigTest, DuplicateKeyIdsAndMissingKeys) {
  auto config = baseConfig();
  config.hmac_keys = {{.id = "a", .secret = "s1"}, {.id = "a", .secret = "s2"}};
  auto duplicate = plugins::cat4moq::resolve(config);
  ASSERT_TRUE(duplicate.hasError());
  EXPECT_THAT(duplicate.error(), HasSubstr("hmac_keys[1].id duplicates another key"));
  config.hmac_keys.reset();
  auto missing = plugins::cat4moq::resolve(config);
  ASSERT_TRUE(missing.hasError());
  EXPECT_THAT(missing.error(), HasSubstr("hmac_keys is required"));
}

TEST(Cat4moqAuthConfigTest, ServiceScopeOnly) {
  auto res = resolveYaml(
      "plugins:\n  - type: cat4moq-auth\n    config: {max_tokens_per_message: 1, hmac_keys: "
      "[{id: k, secret: s}]}\n"
  );
  ASSERT_TRUE(res.hasError());
  EXPECT_THAT(res.error(), HasSubstr("plugins[0] (cat4moq-auth): not allowed at process scope"));
}

TEST(Cat4moqAuthConfigTest, LegacyAuthBlocksFailEvenWhenNotStrict) {
  auto service = resolveYaml("    auth: {enabled: true}\n");
  ASSERT_TRUE(service.hasError());
  EXPECT_THAT(service.error(), HasSubstr("Service 'live': auth is no longer supported"));
  auto defaults = resolveYaml("service_defaults:\n  auth: {max_tokens_per_message: 4}\n");
  ASSERT_TRUE(defaults.hasError());
  EXPECT_THAT(defaults.error(), HasSubstr("service_defaults.plugins.cat4moq-auth"));
}

TEST(Cat4moqAuthConfigTest, SerializerRedactsSecrets) {
  auto res = resolveYaml(std::string(kEntry) + "          max_tokens_per_message: 2\n");
  ASSERT_TRUE(res.hasValue()) << res.error();
  struct Sink : config::ConfigSink {
    void beginObject(std::string_view) override {}
    void endObject() override {}
    void beginArray(std::string_view) override {}
    void endArray() override {}
    void stringField(std::string_view key, std::string_view value) override {
      out += std::string(key) + "=" + std::string(value) + ";";
    }
    void boolField(std::string_view, bool) override {}
    void intField(std::string_view, int64_t) override {}
    void uintField(std::string_view, uint64_t) override {}
    void doubleField(std::string_view, double) override {}
    void nullField(std::string_view) override {}
    std::string out;
  } sink;
  config::serializeConfig(res->config, sink);
  EXPECT_THAT(sink.out, HasSubstr("id=k1;secret=<redacted>;"));
  EXPECT_THAT(sink.out, ::testing::Not(HasSubstr("secret-secret-1")));
}

} // namespace
} // namespace openmoq::moqx
