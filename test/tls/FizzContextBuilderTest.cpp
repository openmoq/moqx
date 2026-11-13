/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "tls/FizzContextBuilder.h"

#include "MoqxQmuxRelayServer.h"
#include "config/Pkcs12.h"
#include "config/Pkcs12TestUtils.h"

#include <folly/executors/IOThreadPoolExecutor.h>
#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace openmoq::moqx::tls::test {
namespace {

using ::testing::HasSubstr;

// CN of the cert built by makeSelfSignedPkcs12Der.
constexpr const char* kTestIdentity = "moqx-test";

const std::vector<std::string> kAlpns{"moqt-16"};

config::TlsMaterial makeMaterial() {
  auto der = config::test::makeSelfSignedPkcs12Der("pw");
  auto material = config::transcodePkcs12(der, "pw");
  if (material.hasError()) {
    throw std::runtime_error(material.error());
  }
  return std::move(*material);
}

TEST(FizzContextBuilder, BuildsFromPkcs12Material) {
  auto ctx = buildFizzServerContext(
      config::TlsMode{config::TlsConfig{.material = makeMaterial()}},
      kAlpns
  );
  ASSERT_TRUE(ctx);
  EXPECT_TRUE(ctx->getCert(kTestIdentity));
  EXPECT_EQ(ctx->getClientAuthMode(), fizz::server::ClientAuthMode::Optional);
  EXPECT_EQ(ctx->getSupportedAlpns(), kAlpns);
}

TEST(FizzContextBuilder, BuildsFromPemFiles) {
  auto material = makeMaterial();
  config::test::TempFile cert(material.certChainPem, ".pem");
  config::test::TempFile key(material.keyPem, ".pem");
  auto ctx = buildFizzServerContext(
      config::TlsMode{config::TlsConfig{.certFile = cert.path(), .keyFile = key.path()}},
      kAlpns
  );
  ASSERT_TRUE(ctx);
  EXPECT_TRUE(ctx->getCert(kTestIdentity));
  EXPECT_EQ(ctx->getClientAuthMode(), fizz::server::ClientAuthMode::Optional);
}

TEST(FizzContextBuilder, MissingPemFileErrorNamesPaths) {
  const config::TlsConfig tlsCfg{
      .certFile = "/nonexistent/moqx-cert.pem",
      .keyFile = "/nonexistent/moqx-key.pem",
  };
  try {
    buildFizzServerContext(config::TlsMode{tlsCfg}, kAlpns);
    FAIL() << "expected an exception";
  } catch (const std::runtime_error& e) {
    EXPECT_THAT(e.what(), HasSubstr("/nonexistent/moqx-cert.pem"));
    EXPECT_THAT(e.what(), HasSubstr("/nonexistent/moqx-key.pem"));
  }
}

TEST(FizzContextBuilder, InsecureUsesBuiltInCert) {
  auto ctx = buildFizzServerContext(config::TlsMode{config::Insecure{}}, kAlpns);
  ASSERT_TRUE(ctx);
  EXPECT_EQ(ctx->getClientAuthMode(), fizz::server::ClientAuthMode::None);
}

TEST(MoqxQmuxRelayServer, ConstructsWithPkcs12Material) {
  config::ListenerConfig listener;
  listener.address = folly::SocketAddress("::1", 0);
  listener.tlsMode = config::TlsConfig{.material = makeMaterial()};
  listener.endpoint = "/moq";
  listener.moqtVersions = "16";
  listener.quicStack = config::QuicStack::ProxygenQmux;
  auto context = std::make_shared<MoqxRelayContext>(
      folly::F14FastMap<std::string, config::ServiceConfig>{
          {"default",
           config::ServiceConfig{
               .match = {{
                   .authority = config::ServiceConfig::MatchEntry::AnyAuthority{},
                   .path = config::ServiceConfig::MatchEntry::PrefixPath{"/"},
               }},
           }},
      },
      "test-relay"
  );
  folly::IOThreadPoolExecutor ioExecutor{1};
  EXPECT_NO_THROW({ MoqxQmuxRelayServer server(listener, context, &ioExecutor); });
}

} // namespace
} // namespace openmoq::moqx::tls::test
