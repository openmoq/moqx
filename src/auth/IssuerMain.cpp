/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

// moqx-issuer: standalone CLI that mints C4M draft-02 CWT tokens for a moqx
// relay. The relay only verifies tokens; issuing/signing lives here.

#include "auth/AuthTokenIssuer.h"
#include "auth/Es256Key.h"
#include "config/loader/ConfigInit.h"

#include <folly/String.h>
#include <folly/base64.h>
#include <folly/init/Init.h>

#include <gflags/gflags.h>

#include <algorithm>
#include <chrono>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>

DEFINE_string(config, "", "Path to config file (for --auth_service key lookup)");
DEFINE_bool(strict_config, false, "Reject unknown config fields");
DEFINE_string(auth_service, "", "Service name to read the auth key from --config");
DEFINE_string(
    auth_key_id,
    "",
    "Signing key id; defaults to first configured key of the selected algorithm"
);
DEFINE_string(auth_secret, "", "HMAC secret; bypasses --config key lookup");
DEFINE_string(
    auth_private_key_file,
    "",
    "PEM P-256 private key for ES256 signing; excludes --auth_secret"
);
DEFINE_string(
    auth_actions,
    "setup,publish_namespace,publish",
    "Comma-separated C4M draft-02 actions"
);
DEFINE_string(auth_namespace, "", "Slash-separated track namespace");
DEFINE_string(auth_track, "", "Optional track name");
DEFINE_string(auth_issuer, "", "Optional CWT iss claim");
DEFINE_string(auth_audience, "", "Optional CWT aud claim; comma-separated for several audiences");
DEFINE_bool(auth_replay_protect, false, "Add catreplay=1 with a random cti (single connection)");
DEFINE_int32(auth_ttl_seconds, 3600, "Token lifetime in seconds");
DEFINE_string(auth_output, "base64-prefix", "Output encoding: base64-prefix, base64, hex, or raw");

using namespace openmoq::moqx;

namespace {

namespace cfg = config;

constexpr std::string_view kServeCommand = "serve";

const cfg::AuthConfig& selectedAuth(const cfg::ResolvedConfig* resolved) {
  if (resolved == nullptr || FLAGS_auth_service.empty()) {
    throw std::invalid_argument(
        "--config and --auth_service are required to select a configured key"
    );
  }
  const auto& services = resolved->config.services;
  const auto it = services.find(FLAGS_auth_service);
  if (it == services.end()) {
    throw std::invalid_argument("unknown auth service: " + FLAGS_auth_service);
  }
  if (!it->second.auth.enabled) {
    throw std::invalid_argument("service auth is not enabled");
  }
  return it->second.auth;
}

auth::IssueTokenOptions makeIssueTokenOptions(const cfg::ResolvedConfig* resolvedConfig) {
  std::string keyID = FLAGS_auth_key_id;
  std::string secret = FLAGS_auth_secret;
  if (!FLAGS_auth_private_key_file.empty()) {
    if (!secret.empty()) {
      throw std::invalid_argument("--auth_secret and --auth_private_key_file are mutually exclusive"
      );
    }
    if (resolvedConfig) {
      const auto& authConfig = selectedAuth(resolvedConfig);
      if (authConfig.es256Keys.empty()) {
        throw std::invalid_argument("selected service has no ES256 keys");
      }
      if (keyID.empty()) {
        keyID = authConfig.es256Keys.front().id;
      }
      if (std::none_of(
              authConfig.es256Keys.begin(),
              authConfig.es256Keys.end(),
              [&](const auto& key) { return key.id == keyID; }
          )) {
        throw std::invalid_argument("selected service has no ES256 key with id: " + keyID);
      }
    } else if (keyID.empty()) {
      keyID = "operator";
    }
  } else if (secret.empty()) {
    auto key = auth::selectHmacKey(selectedAuth(resolvedConfig), FLAGS_auth_key_id);
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
      .privateKeyFile = FLAGS_auth_private_key_file,
  };
  if (!FLAGS_auth_namespace.empty()) {
    options.trackNamespace = auth::parseTrackNamespace(FLAGS_auth_namespace);
  }
  options.replayProtected = FLAGS_auth_replay_protect;
  if (!FLAGS_auth_issuer.empty()) {
    options.issuer = FLAGS_auth_issuer;
  }
  for (std::size_t start = 0;
       !FLAGS_auth_audience.empty() && start <= FLAGS_auth_audience.size();) {
    const auto comma = FLAGS_auth_audience.find(',', start);
    const auto end = comma == std::string::npos ? FLAGS_auth_audience.size() : comma;
    if (end > start) {
      options.audiences.push_back(FLAGS_auth_audience.substr(start, end - start));
    }
    if (comma == std::string::npos) {
      break;
    }
    start = comma + 1;
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
    const auto options =
        makeIssueTokenOptions(resolvedConfig.has_value() ? &resolvedConfig.value() : nullptr);
    const auto token = auth::issueToken(options);
    if (resolvedConfig && !FLAGS_auth_private_key_file.empty()) {
      const auto& keys = selectedAuth(&*resolvedConfig).es256Keys;
      const auto key = std::find_if(keys.begin(), keys.end(), [&](const auto& entry) {
        return entry.id == options.keyID;
      });
      auto publicKey = auth::loadEs256PublicKeyFile(key->publicKeyFile);
      // Only the signature is being checked, so accept the claims this token was issued with.
      auth::ClaimPolicy policy;
      policy.audiences = options.audiences;
      try {
        (void)auth::verifyC4mToken(token.tokenValue, *publicKey, options.now, policy);
      } catch (const auth::C4mError& error) {
        if (error.code() == auth::AuthError::BadSignature) {
          throw std::invalid_argument("ES256 private key does not match the selected public key");
        }
        throw std::invalid_argument(std::string("issued token is invalid: ") + error.what());
      }
    }
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
    // gflags usage includes current flag values, including --auth_secret.
    std::cerr << "Use --help for options.\n";
    return 1;
  }
  return 0;
}

} // namespace

int main(int argc, char* argv[]) {
  google::SetUsageMessage(
      "moqx-issuer — mint a C4M draft-02 CWT for publishers/subscribers\n\n"
      "Provide --auth_secret, --auth_private_key_file, or --config with --auth_service.\n"
      "Usage: moqx-issuer --auth_secret <secret> --auth_actions <a,b,c> "
      "[--auth_namespace <ns>] [--auth_track <t>]"
  );
  folly::Init init(&argc, &argv, true);
  return runIssueCatTokenCommand(argv[0]);
}
