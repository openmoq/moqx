/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

// moqx-issuer: standalone CLI that mints Catapult CAT4MOQ CWT tokens for a moqx
// relay. The relay only verifies tokens; issuing/signing lives here.

#include "auth/AuthTokenIssuer.h"
#include "config/loader/ConfigInit.h"

#include <folly/String.h>
#include <folly/base64.h>
#include <folly/init/Init.h>

#include <gflags/gflags.h>

#include <chrono>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

DEFINE_string(config, "", "Path to config file (for --auth_service key lookup)");
DEFINE_bool(strict_config, false, "Reject unknown config fields");
DEFINE_string(auth_service, "", "Service name to read the auth key from --config");
DEFINE_string(auth_key_id, "", "HMAC key id; defaults to first configured key");
DEFINE_string(auth_secret, "", "HMAC secret; bypasses --config key lookup");
DEFINE_string(
    auth_actions,
    "client_setup,publish_namespace,publish",
    "Comma-separated CAT4MOQ actions"
);
DEFINE_string(auth_namespace, "", "Slash-separated track namespace");
DEFINE_string(auth_track, "", "Optional track name");
DEFINE_int32(auth_ttl_seconds, 3600, "Token lifetime in seconds");
DEFINE_string(auth_output, "base64-prefix", "Output encoding: base64-prefix, base64, hex, or raw");

using namespace openmoq::moqx;

namespace {

namespace cfg = config;

constexpr std::string_view kServeCommand = "serve";

auth::IssueTokenOptions makeIssueTokenOptions(const cfg::ResolvedConfig* resolvedConfig) {
  std::string keyID = FLAGS_auth_key_id;
  std::string secret = FLAGS_auth_secret;
  if (secret.empty()) {
    if (resolvedConfig == nullptr) {
      throw std::invalid_argument(
          "--auth_secret is required unless --config and --auth_service select a configured key"
      );
    }
    if (FLAGS_auth_service.empty()) {
      throw std::invalid_argument("--auth_service is required when issuing from --config");
    }
    const auto& services = resolvedConfig->config.services;
    auto it = services.find(FLAGS_auth_service);
    if (it == services.end()) {
      throw std::invalid_argument("unknown auth service: " + FLAGS_auth_service);
    }
    auto key = auth::selectHmacKey(it->second.auth, FLAGS_auth_key_id);
    keyID = std::move(key.id);
    secret = std::move(key.secret);
  } else if (keyID.empty()) {
    keyID = "operator";
  }

  auth::IssueTokenOptions options{
      .keyID = std::move(keyID),
      .secret = std::move(secret),
      .actions = auth::parseActions(FLAGS_auth_actions),
      .ttl = std::chrono::seconds(FLAGS_auth_ttl_seconds),
  };
  if (!FLAGS_auth_namespace.empty()) {
    options.trackNamespace = auth::parseTrackNamespace(FLAGS_auth_namespace);
  }
  if (!FLAGS_auth_track.empty()) {
    options.trackName = FLAGS_auth_track;
  }
  return options;
}

int runIssueCatTokenCommand(const char* programName) {
  std::optional<cfg::ResolvedConfig> resolvedConfig;
  if (!FLAGS_config.empty()) {
    auto result =
        cfg::handleConfigSubcommand(kServeCommand, FLAGS_config, FLAGS_strict_config, programName);
    if (result.hasError()) {
      return result.error();
    }
    resolvedConfig = std::move(result.value());
  }

  try {
    const auto token = auth::issueToken(
        makeIssueTokenOptions(resolvedConfig.has_value() ? &resolvedConfig.value() : nullptr)
    );
    if (FLAGS_auth_output == "base64-prefix") {
      std::cout << "base64:" << folly::base64Encode(token.tokenValue) << '\n';
    } else if (FLAGS_auth_output == "base64") {
      std::cout << folly::base64Encode(token.tokenValue) << '\n';
    } else if (FLAGS_auth_output == "hex") {
      std::cout << folly::hexlify(folly::StringPiece(token.tokenValue)) << '\n';
    } else if (FLAGS_auth_output == "raw") {
      std::cout.write(
          token.tokenValue.data(),
          static_cast<std::streamsize>(token.tokenValue.size())
      );
    } else {
      throw std::invalid_argument("--auth_output must be base64-prefix, base64, hex, or raw");
    }
  } catch (const std::exception& e) {
    std::cerr << "Error: " << e.what() << '\n';
    google::ShowUsageWithFlags(programName);
    return 1;
  }
  return 0;
}

} // namespace

int main(int argc, char* argv[]) {
  google::SetUsageMessage("moqx-issuer — mint a Catapult CAT4MOQ CWT for publishers/subscribers\n\n"
                          "Provide a key via --auth_secret, or via --config with --auth_service.\n"
                          "Usage: moqx-issuer --auth_secret <secret> --auth_actions <a,b,c> "
                          "[--auth_namespace <ns>] [--auth_track <t>]");
  folly::Init init(&argc, &argv, true);
  return runIssueCatTokenCommand(argv[0]);
}
