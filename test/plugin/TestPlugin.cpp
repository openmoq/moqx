/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "plugin/TestPlugin.h"

#include <folly/coro/DetachOnCancel.h>

namespace openmoq::moqx::plugin::test {

std::shared_ptr<Script>& currentScript() {
  static std::shared_ptr<Script> script = std::make_shared<Script>();
  return script;
}

namespace {

// Returns early on cancellation unless the script ignores it.
folly::coro::Task<void> waitGate(Script& script, folly::coro::Baton& gate) {
  if (script.ignoreCancellation) {
    co_await gate;
  } else {
    co_await folly::coro::co_awaitTry(folly::coro::detachOnCancel(
        folly::coro::co_invoke([&gate]() -> folly::coro::Task<void> { co_await gate; })
    ));
  }
}

const char* kindName(RequestKind kind) {
  switch (kind) {
  case RequestKind::Subscribe:
    return "subscribe";
  case RequestKind::Fetch:
    return "fetch";
  case RequestKind::TrackStatus:
    return "track_status";
  case RequestKind::Publish:
    return "publish";
  case RequestKind::PublishNamespace:
    return "publish_namespace";
  case RequestKind::SubscribeNamespace:
    return "subscribe_namespace";
  case RequestKind::SubscribeTracks:
    return "subscribe_tracks";
  }
  return "?";
}

class TestInstance : public ServiceInstance {
public:
  TestInstance(std::shared_ptr<Script> script, std::string name, std::string service)
      : script_(std::move(script)), name_(std::move(name)), service_(std::move(service)) {}

  HookSet hooks() const override { return script_->hooks; }

  folly::coro::Task<void> start() override {
    script_->record("start:" + name_ + "@" + service_);
    co_return;
  }
  folly::coro::Task<void> stop() override {
    script_->record("stop:" + name_ + "@" + service_);
    co_return;
  }

  folly::coro::Task<Result<std::shared_ptr<SessionState>>> admit(const SetupView& setup) override {
    script_->record("admit:" + name_ + "@" + std::string(setup.session.service));
    if (script_->recordParam) {
      if (auto value = setup.params.uintParam(*script_->recordParam)) {
        script_->record("param:" + name_ + "=" + std::to_string(*value));
      }
    }
    if (script_->suspendAdmit) {
      co_await waitGate(*script_, script_->admitGate);
      auto token = co_await folly::coro::co_current_cancellation_token;
      if (token.isCancellationRequested() && !script_->ignoreCancellation) {
        co_return fail(Error::Kind::Internal, "cancelled");
      }
    }
    if (auto errors = script_->admitErrors.rlock(); errors->contains(name_)) {
      co_return folly::makeUnexpected(errors->at(name_));
    }
    co_return std::make_shared<TestState>(name_);
  }

  folly::coro::Task<void> sessionEnded(std::shared_ptr<SessionState> state) override {
    auto* typed = dynamic_cast<TestState*>(state.get());
    script_->record("ended:" + name_ + ":" + (typed ? typed->id : "?"));
    co_return;
  }

  folly::coro::Task<Result<folly::Unit>>
  authorize(const RequestView& request, const SessionState* state) override {
    auto* typed = dynamic_cast<const TestState*>(state);
    script_->authorized.wlock()->push_back(
        name_ + ":" + kindName(request.kind) + ":" + std::string(request.track.value_or("")) + ":" +
        (request.peer ? "peer" : "-") + ":" + (typed ? typed->id : "null")
    );
    if (script_->suspendAuthorize) {
      co_await waitGate(*script_, script_->authorizeGate);
    }
    if (auto errors = script_->authorizeErrors.rlock(); errors->contains(name_)) {
      co_return folly::makeUnexpected(errors->at(name_));
    }
    co_return folly::unit;
  }

  folly::coro::Task<Result<Params>> setupParams(const UpstreamScope& scope) override {
    script_->record("upstream:" + name_ + "@" + std::string(scope.service));
    auto it = script_->upstreamParams.find(name_);
    co_return it == script_->upstreamParams.end() ? Params{} : it->second;
  }

  folly::coro::Task<folly::dynamic> stateSnapshot() override {
    if (script_->snapshotStalls) {
      co_await script_->snapshotGate;
    }
    if (auto throws = script_->snapshotThrows.rlock(); throws->contains(name_)) {
      throw std::runtime_error(throws->at(name_));
    }
    co_return folly::dynamic::object("id", name_)("service", service_);
  }

private:
  std::shared_ptr<Script> script_;
  std::string name_;
  std::string service_;
};

class TestRoot : public Root {
public:
  TestRoot(std::shared_ptr<Script> script, TestConfig config)
      : script_(std::move(script)), config_(std::move(config)),
        name_(config_.name.value_or("test")) {}

  folly::coro::Task<void> start() override {
    script_->record("start:" + name_);
    if (auto throws = script_->startThrows.rlock(); throws->contains(name_)) {
      throw std::runtime_error("start of " + name_ + " failed");
    }
    co_return;
  }
  folly::coro::Task<void> stop() override {
    script_->record("stop:" + name_);
    co_return;
  }

  bool declaresListenerParams() const override { return config_.listener_params.value_or(false); }

  folly::coro::Task<Result<Params>> setupParams(const ListenerScope& scope) override {
    script_->record("listener:" + name_ + "@" + std::string(scope.listener));
    auto it = script_->listenerParams.find(name_);
    co_return it == script_->listenerParams.end() ? Params{} : it->second;
  }

  Result<std::unique_ptr<ServiceInstance>> createService(const ServiceContext& ctx) override {
    if (auto errors = script_->createErrors.rlock(); errors->contains(name_)) {
      return folly::makeUnexpected(errors->at(name_));
    }
    return std::make_unique<TestInstance>(script_, name_, std::string(ctx.serviceName));
  }

private:
  std::shared_ptr<Script> script_;
  TestConfig config_;
  std::string name_;
};

} // namespace

std::shared_ptr<const PluginType> testType(std::string name) {
  return makePluginType<TestConfig>(
      std::move(name),
      Scopes{.process = true, .service = true},
      [](const TestConfig& config) -> Result<std::unique_ptr<Root>> {
        if (config.tag && *config.tag < 0) {
          return fail(Error::Kind::Internal, "tag must be >= 0");
        }
        return std::make_unique<TestRoot>(currentScript(), config);
      },
      [](TestConfig& config) {
        if (config.secret) {
          config.secret = "<redacted>";
        }
      }
  );
}

} // namespace openmoq::moqx::plugin::test
