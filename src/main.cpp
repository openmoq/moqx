/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "MoqxRelayContext.h"
#include "MoqxServerFactory.h"
#include "admin/AdminServer.h"
#include "admin/BuiltinRoutes.h"
#include "admin/CachePurgeHandler.h"
#include "admin/ConfigHandler.h"
#include "admin/MetricsHandler.h"
#include "admin/StateHandler.h"
#include "auth/AuthTokenIssuer.h"
#include "bpf/QuicReuseportSteering.h"
#include "config/loader/ConfigInit.h"
#include "logging/LogSetup.h"
#include "stats/StatsRegistry.h"

#include <gflags/gflags.h>

#include <csignal>

#include <chrono>
#include <cstdint>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <thread>
#include <vector>

#include <folly/executors/IOThreadPoolExecutor.h>
#include <folly/executors/thread_factory/NamedThreadFactory.h>
#include <folly/init/Init.h>
#include <folly/io/async/AsyncSignalHandler.h>
#include <folly/logging/Init.h>
#include <folly/logging/xlog.h>

#include "LoggingMultiFlag.h"

#include <iostream>
#include <string_view>

// Default folly logging config: root at INFO, async sink with WARN+ flushed
// synchronously so we don't lose warnings/errors on crash. Overridable at
// runtime via --logging=... or FOLLY_LOGGING= env var (folly::Init wires both).
FOLLY_INIT_LOGGING_CONFIG(".=INFO;default:async=true,sync_level=WARN");

DEFINE_string(config, "", "Path to config file (required)");
DEFINE_bool(strict_config, false, "Reject unknown config fields");
DEFINE_string(auth_service, "", "issue-cat-token: service name to read auth key from --config");
DEFINE_string(auth_key_id, "", "issue-cat-token: HMAC key id; defaults to first configured key");
DEFINE_string(auth_secret, "", "issue-cat-token: HMAC secret; bypasses --config key lookup");
DEFINE_string(
    auth_actions,
    "client_setup,publish_namespace,publish",
    "issue-cat-token: comma-separated CAT4MOQ actions"
);
DEFINE_string(auth_namespace, "", "issue-cat-token: slash-separated track namespace");
DEFINE_string(auth_track, "", "issue-cat-token: optional track name");
DEFINE_int32(auth_ttl_seconds, 3600, "issue-cat-token: token lifetime in seconds");
DEFINE_string(
    auth_output,
    "base64-prefix",
    "issue-cat-token: output encoding: base64-prefix, base64, hex, or raw"
);

using namespace openmoq::moqx;

namespace {

namespace cfg = config;

constexpr std::string_view kServeCommand = "serve";
constexpr std::string_view kIssueCatTokenCommand = "issue-cat-token";

std::string base64Encode(std::string_view bytes) {
  static constexpr char kAlphabet[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
  std::string out;
  out.reserve(((bytes.size() + 2) / 3) * 4);
  for (std::size_t i = 0; i < bytes.size(); i += 3) {
    const auto b0 = static_cast<uint8_t>(bytes[i]);
    const auto b1 = i + 1 < bytes.size() ? static_cast<uint8_t>(bytes[i + 1]) : uint8_t{0};
    const auto b2 = i + 2 < bytes.size() ? static_cast<uint8_t>(bytes[i + 2]) : uint8_t{0};
    out.push_back(kAlphabet[b0 >> 2]);
    out.push_back(kAlphabet[((b0 & 0x03) << 4) | (b1 >> 4)]);
    out.push_back(i + 1 < bytes.size() ? kAlphabet[((b1 & 0x0f) << 2) | (b2 >> 6)] : '=');
    out.push_back(i + 2 < bytes.size() ? kAlphabet[b2 & 0x3f] : '=');
  }
  return out;
}

std::string hexEncode(std::string_view bytes) {
  static constexpr char kHex[] = "0123456789abcdef";
  std::string out;
  out.reserve(bytes.size() * 2);
  for (auto c : bytes) {
    const auto b = static_cast<uint8_t>(c);
    out.push_back(kHex[b >> 4]);
    out.push_back(kHex[b & 0x0f]);
  }
  return out;
}

auth::IssueTokenOptions makeIssueTokenOptions(const cfg::ResolvedConfig* resolvedConfig) {
  std::string keyID = FLAGS_auth_key_id;
  std::string secret = FLAGS_auth_secret;
  if (secret.empty()) {
    if (resolvedConfig == nullptr) {
      throw std::invalid_argument(
          "--auth-secret is required unless --config and --auth-service select a configured key"
      );
    }
    if (FLAGS_auth_service.empty()) {
      throw std::invalid_argument("--auth-service is required when issuing from --config");
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
      std::cout << "base64:" << base64Encode(token.tokenValue) << '\n';
    } else if (FLAGS_auth_output == "base64") {
      std::cout << base64Encode(token.tokenValue) << '\n';
    } else if (FLAGS_auth_output == "hex") {
      std::cout << hexEncode(token.tokenValue) << '\n';
    } else if (FLAGS_auth_output == "raw") {
      std::cout.write(
          token.tokenValue.data(),
          static_cast<std::streamsize>(token.tokenValue.size())
      );
    } else {
      throw std::invalid_argument("--auth-output must be base64-prefix, base64, hex, or raw");
    }
  } catch (const std::exception& e) {
    std::cerr << "Error: " << e.what() << '\n';
    google::ShowUsageWithFlags(programName);
    return 1;
  }
  return 0;
}

class ShutdownSignalHandler : public folly::AsyncSignalHandler {
public:
  explicit ShutdownSignalHandler(folly::EventBase* evb) : AsyncSignalHandler(evb) {
    registerSignalHandler(SIGTERM);
    registerSignalHandler(SIGINT);
  }

  void signalReceived(int signum) noexcept override {
    XLOG(INFO) << "Received signal " << signum << ", shutting down";
    getEventBase()->terminateLoopSoon();
  }
};

} // namespace

int main(int argc, char* argv[]) {

  // === 1. Parse command-line flags/config ===
  // CLI args, config files, env vars
  google::SetUsageMessage(
      "moqx MoQ relay server\n\n"
      "Subcommands:\n"
      "  serve                Start the relay (default)\n" +
      cfg::configSubcommandUsage() +
      "  issue-cat-token      Issue a Catapult CAT4MOQ CWT for publishers\n"
      "\nUsage: moqx [subcommand] --config <path>"
  );
  // Combine repeated --logging / --log-handler into one composite before
  // folly::Init — see docs/logging.md.
  combineLoggingArgs(argc, argv);
  folly::Init init(&argc, &argv, true);

  std::string_view subcommand = kServeCommand;
  if (argc > 1) {
    subcommand = argv[1];
  }

  if (subcommand == kIssueCatTokenCommand) {
    return runIssueCatTokenCommand(argv[0]);
  }

  auto result = cfg::handleConfigSubcommand(subcommand, FLAGS_config, FLAGS_strict_config, argv[0]);
  if (result.hasError()) {
    return result.error();
  }

  for (const auto& w : result.value().warnings) {
    XLOG(WARNING) << w;
  }
  const auto& config = result.value().config;

  if (subcommand != kServeCommand) {
    std::cerr << "Unknown subcommand: " << subcommand << "\n";
    google::ShowUsageWithFlags(argv[0]);
    return 1;
  }

  // === 2. Set up logging/observability ===
  // folly logging is initialized by folly::Init above using the
  // FOLLY_INIT_LOGGING_CONFIG default at file scope. Override with
  // --logging=<config> or the FOLLY_LOGGING env var.
  auto mlogResult = logging::setupMLog(config);
  if (!mlogResult) {
    return mlogResult.error();
  }
  auto mlog = std::move(*mlogResult);

  auto qlogResult = logging::setupQLog(config);
  if (!qlogResult) {
    return qlogResult.error();
  }
  const cfg::QLogConfig* qlogConfig = *qlogResult;

  // === 3. Set up signal handling ===
  folly::EventBase evb;
  ShutdownSignalHandler signalHandler(&evb);

  // === 4. Initialize resources ===
  quicReuseportSetEnabled(config.mvfstBpfSteering);

  auto ioExecutor = std::make_unique<folly::IOThreadPoolExecutor>(
      config.threads,
      std::make_shared<folly::NamedThreadFactory>("moqx-io")
  );

  // === 5. Initialize dependencies ===
  // TODO: TBD

  // === 6. Initialize services ===
  // Construct and configure the application's own services
  // (MoqxRelayContext, MoqxRelayServer, etc.)
  auto context = std::make_shared<MoqxRelayContext>(
      config.services,
      config.relayID,
      config.useRelayThread,
      config.useLocalForwarders
  );

  // === 6a. Stats registry ===
  auto statsRegistry = std::make_shared<stats::StatsRegistry>();

  std::vector<std::shared_ptr<moxygen::MoQServerBase>> servers;
  try {
    for (const auto& listenerCfg : config.listeners) {
      servers.emplace_back(makeRelayServer(
          listenerCfg,
          context,
          ioExecutor.get(),
          statsRegistry,
          mlog.factory,
          qlogConfig
      ));
    }
  } catch (const std::exception& e) {
    // Listener setup (e.g. TLS cert loading) can throw. Report cleanly and exit
    // non-zero rather than letting it reach std::terminate / SIGABRT (#173).
    std::cerr << "Error: failed to initialize listener: " << e.what() << "\n";
    return 1;
  }

  if (!servers.empty()) {
    context->setCacheEvb(ioExecutor->getAllEventBases()[0].get());
    context->initThreadStatsCollectors(*ioExecutor);
  }

  // === 7. Start health checks / admin endpoints ===
  admin::AdminServer adminServer;
  admin::registerBuiltinRoutes(adminServer);
  admin::registerMetricsRoute(adminServer, statsRegistry);
  admin::registerCachePurgeRoute(adminServer, context);
  admin::registerStateRoute(adminServer, context);
  admin::registerConfigRoute(adminServer, std::make_shared<const cfg::Config>(config));

  // === 8. Start serving ===
  for (auto& server : servers) {
    // addr ignored — each server binds its own listenerCfg address
    server->start(folly::SocketAddress{});
  }

  // Start admin after servers so every stats collector is registered before /metrics can read them.
  if (config.admin) {
    if (!adminServer.start(*config.admin)) {
      XLOG(FATAL) << "Failed to start admin server on " << config.admin->address.describe();
    }

    XLOG(INFO) << "Admin server listening on " << config.admin->address.describe();
  }

  if (!servers.empty()) {
    context->initUpstreams(ioExecutor->getAllEventBases()[0].get());
  }

  evb.loopForever();

  // Hard shutdown watchdog: if teardown hangs, force-exit after 10 seconds.
  std::thread([relayID = config.relayID] {
    std::this_thread::sleep_for(std::chrono::seconds(10));
    XLOG(ERR) << "Shutdown timed out after 10s (relay_id=" << relayID << "), forcing exit";
    std::_Exit(1);
  }).detach();

  // ============================================
  // Teardown (reverse order, on signal/shutdown)
  // ============================================

  // === 9. Stop accepting new connections ===
  // Signal upstreams to stop — cancels reconnect backoff so worker EVBs can
  // exit cleanly before servers drain them.
  context->stop();
  // TODO: close listeners

  // === 10. Drain in-flight requests ===
  // TODO: wait with timeout for active work to finish
  // Timeout on drain -- don't wait forever for graceful shutdown

  // === 11. Shut down dependencies ===
  // TODO: TBD

  // === 12. Flush telemetry/logs ===
  // TODO: ensure observability data is sent

  // === 13. Clean up resources ===
  // Stop admin last — allows a final metrics scrape during drain.
  adminServer.stop();

  // Stop servers before joining the IO pool — stop() needs worker EVBs alive,
  // and lingering shared_ptr refs could delay ~MoqxRelayServer past that point.
  for (auto& server : servers) {
    server->stop();
  }
  servers.clear();
  ioExecutor.reset();

  // Join mlog write executor last: sessions are destroyed by the teardown above
  // (their outputLogs() calls schedule writes here), so we must not join until
  // after the IO pool drains.
  if (mlog.executor) {
    mlog.executor->join();
  }

  // === 14. Exit with appropriate code ===
  return 0;
}
