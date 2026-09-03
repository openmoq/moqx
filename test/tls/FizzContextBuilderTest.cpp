/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "tls/FizzContextBuilder.h"

#include "CertTestUtils.h"

#include <gtest/gtest.h>

namespace openmoq::moqx::tls {
namespace {

using moqx::test::makeSelfSignedCertPem;
using moqx::test::TempDir;

// Single-cert listener config over a freshly written pair in `dir`.
config::ListenerTlsConfig fileConfig(const TempDir& dir) {
  writePair(dir, "a", makeSelfSignedCertPem("a.example.com"));
  config::ListenerTlsConfig cfg;
  cfg.tls.certFile = dir.path() + "/a.pem";
  cfg.tls.keyFile = dir.path() + "/a.key";
  return cfg;
}

TEST(FizzContextBuilder, ConfiguredTicketSeedsAccepted) {
  TempDir dir;
  auto cfg = fileConfig(dir);
  // Two seeds: 32-byte minimum and a longer one; first-encrypts semantics are
  // fizz's, this exercises the plumbing and the accepted-length path.
  cfg.ticketSeeds = {std::string(32, 'x'), std::string(48, 'y')};

  auto ctx = buildFizzServerContext(config::TlsMode{cfg}, {.alpns = {"moq-00"}});
  ASSERT_NE(ctx, nullptr);
  EXPECT_NE(ctx->getTicketCipher(), nullptr);
}

TEST(FizzContextBuilder, ShortTicketSeedThrows) {
  TempDir dir;
  auto cfg = fileConfig(dir);
  cfg.ticketSeeds = {std::string(16, 'x')};

  // fizz rejects sub-32-byte secrets wholesale; surfacing that as an error
  // beats a silently secretless ticket cipher (no tickets, no resumption).
  EXPECT_THROW(
      buildFizzServerContext(config::TlsMode{cfg}, {.alpns = {"moq-00"}}),
      std::runtime_error
  );
}

TEST(FizzContextBuilder, NoSeedsStillBuildsTicketCipher) {
  TempDir dir;
  auto cfg = fileConfig(dir);

  auto ctx = buildFizzServerContext(config::TlsMode{cfg}, {.alpns = {"moq-00"}});
  ASSERT_NE(ctx, nullptr);
  EXPECT_NE(ctx->getTicketCipher(), nullptr);
}

TEST(FizzContextBuilder, ListenersSharingCertDirShareManager) {
  TempDir dir;
  writePair(dir, "a", makeSelfSignedCertPem("a.example.com"));
  config::ListenerTlsConfig cfg;
  cfg.certDir = config::CertDirConfig{dir.path(), std::chrono::seconds(0)};

  auto first = makeCertManager(cfg);
  auto second = makeCertManager(cfg);
  // Same cert source, one manager: one scan, one rescan thread, one key set.
  EXPECT_EQ(first.get(), second.get());

  TempDir otherDir;
  writePair(otherDir, "b", makeSelfSignedCertPem("b.example.com"));
  config::ListenerTlsConfig otherCfg;
  otherCfg.certDir = config::CertDirConfig{otherDir.path(), std::chrono::seconds(0)};
  EXPECT_NE(makeCertManager(otherCfg).get(), first.get());
}

} // namespace
} // namespace openmoq::moqx::tls
