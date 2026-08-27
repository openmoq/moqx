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
