/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include <folly/coro/BlockingWait.h>
#include <folly/coro/Sleep.h>
#include <folly/init/Init.h>
#include <folly/io/async/EventBase.h>
#include <moxygen/MoQClient.h>
#include <moxygen/MoQFramer.h>
#include <moxygen/MoQRelaySession.h>
#include <moxygen/MoQVersions.h>
#include <moxygen/events/MoQFollyExecutorImpl.h>
#include <moxygen/util/InsecureVerifierDangerousDoNotUseInProduction.h>

#include <csignal>
#include <iostream>

using namespace moxygen;

namespace {
volatile std::sig_atomic_t stopping = 0;
void stop(int) {
  stopping = 1;
}

folly::coro::Task<void>
advertise(std::shared_ptr<MoQExecutor> exec, std::string url, std::string name) {
  constexpr uint64_t hopID = 100;
  MoQClient client(
      exec,
      proxygen::URL(url),
      MoQRelaySession::createRelaySessionFactory(),
      std::make_shared<test::InsecureVerifierDangerousDoNotUseInProduction>()
  );
  client.addSetupParameter(SetupParameter(
      static_cast<uint64_t>(SetupKey::RELAY_HOPS),
      encodeRelayHopID(hopID, kVersionDraft18).value()
  ));
  co_await client.setupMoQSession(
      std::chrono::seconds(5),
      std::chrono::seconds(60),
      std::make_shared<Publisher>(),
      nullptr,
      quic::TransportSettings(),
      getMoqtProtocols("18", true)
  );
  PublishNamespace pub;
  pub.trackNamespace = TrackNamespace(name, "/");
  pub.params.insertParam(Parameter(
      static_cast<uint64_t>(TrackRequestParamKey::HOP_PATH),
      encodeRelayHopPath({hopID}, kVersionDraft18).value()
  ));
  auto result = co_await client.moqSession_->publishNamespace(std::move(pub));
  if (result.hasError()) {
    throw std::runtime_error(result.error().reasonPhrase);
  }
  std::cout << "namespace accepted" << std::endl;
  while (!stopping && !client.moqSession_->isClosed()) {
    co_await folly::coro::sleep(std::chrono::milliseconds(100));
  }
  result.value()->publishNamespaceDone();
  client.moqSession_->close(SessionCloseErrorCode::NO_ERROR);
}
} // namespace

int main(int argc, char** argv) {
  folly::Init init(&argc, &argv, false);
  if (argc != 3) {
    return 2;
  }
  std::signal(SIGINT, stop);
  std::signal(SIGTERM, stop);
  folly::EventBase evb;
  auto exec = std::make_shared<MoQFollyExecutorImpl>(&evb);
  try {
    folly::coro::blockingWait(
        folly::coro::co_withExecutor(exec.get(), advertise(exec, argv[1], argv[2])),
        &evb
    );
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
