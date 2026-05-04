/*
 * Copyright (c) Synamedia
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include "SwitchTypes.h"
#include <moxygen/MoQTypes.h>
#include <moxygen/relay/MoQForwarder.h>
#include <folly/coro/Task.h>
#include <memory>

namespace openmoq::moqx {

class MoqxRelay;
class MoqxSession;

class SwitchHandler {
 public:
  SwitchHandler(
      std::shared_ptr<MoqxSession> session,
      moxygen::Switch sw,
      MoqxRelay& relay);

  folly::coro::Task<void> run();

 private:
  std::shared_ptr<MoqxSession> session_;
  moxygen::Switch sw_;
  MoqxRelay& relay_;

  folly::coro::Task<void> sendErrorPublishDone(
      moxygen::PublishDoneStatusCode statusCode,
      std::string reason);
};

} // namespace openmoq::moqx
