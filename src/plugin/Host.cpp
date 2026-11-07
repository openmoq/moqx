/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "plugin/Host.h"

#include "plugin/PluginType.h"

#include <folly/coro/Collect.h>
#include <folly/coro/TimedWait.h>
#include <folly/coro/WithCancellation.h>
#include <folly/json/json.h>
#include <folly/logging/xlog.h>
#include <moxygen/MoQTypes.h>

#include <algorithm>

namespace openmoq::moqx::plugin {

// --- InflightTracker ---

bool InflightTracker::enter() {
  std::lock_guard lock(mutex_);
  if (closed_) {
    return false;
  }
  ++count_;
  return true;
}

void InflightTracker::exit() {
  std::vector<folly::Promise<folly::Unit>> waiters;
  {
    std::lock_guard lock(mutex_);
    if (--count_ == 0 && closed_) {
      waiters.swap(waiters_);
    }
  }
  for (auto& waiter : waiters) {
    waiter.setValue();
  }
}

void InflightTracker::close() {
  std::lock_guard lock(mutex_);
  closed_ = true;
}

folly::coro::Task<void> InflightTracker::drained() {
  folly::SemiFuture<folly::Unit> done = folly::makeSemiFuture();
  {
    std::lock_guard lock(mutex_);
    if (count_ > 0) {
      waiters_.emplace_back();
      done = waiters_.back().getSemiFuture();
    }
  }
  co_await std::move(done);
}

InflightGuard::InflightGuard(std::shared_ptr<InflightTracker> tracker) {
  if (tracker && tracker->enter()) {
    tracker_ = std::move(tracker);
  }
}

InflightGuard& InflightGuard::operator=(InflightGuard&& other) noexcept {
  if (this != &other) {
    if (tracker_) {
      tracker_->exit();
    }
    tracker_ = std::move(other.tracker_);
  }
  return *this;
}

InflightGuard::~InflightGuard() {
  if (tracker_) {
    tracker_->exit();
  }
}

// --- ServiceChain ---

ServiceChain::ServiceChain(
    std::string service,
    std::vector<ChainMember> members,
    std::chrono::milliseconds admissionTimeout,
    std::chrono::milliseconds authorizeTimeout,
    folly::Executor::KeepAlive<> relayExecutor
)
    : service_(std::move(service)), members_(std::move(members)),
      admissionTimeout_(admissionTimeout), authorizeTimeout_(authorizeTimeout),
      relayExecutor_(std::move(relayExecutor)) {}

bool ServiceChain::any(Hook hook) const {
  return std::any_of(members_.begin(), members_.end(), [hook](const auto& m) {
    return m.hooks.has(hook);
  });
}

// --- PluginSession ---

namespace {

Error internal(std::string reason) {
  return Error{Error::Kind::Internal, std::move(reason)};
}

std::string describe(const folly::exception_wrapper& ew) {
  return ew.what().toStdString();
}

// Awaits a started check for at most `budget`. A check past the budget keeps
// running detached; its result is dropped.
folly::coro::Task<Result<folly::Unit>> awaitWithin(
    folly::SemiFuture<Result<folly::Unit>> check,
    std::chrono::milliseconds budget,
    folly::CancellationSource cancel,
    std::string_view what
) {
  if (check.isReady()) {
    co_return std::move(check).value();
  }
  auto result =
      co_await folly::coro::co_awaitTry(folly::coro::timed_wait(std::move(check), budget));
  if (result.hasException()) {
    co_return folly::makeUnexpected(internal(describe(result.exception())));
  }
  if (!*result) {
    cancel.requestCancellation();
    co_return folly::makeUnexpected(internal(std::string(what) + " timed out"));
  }
  co_return std::move(**result);
}

} // namespace

PluginSession::PluginSession(
    std::shared_ptr<const ServiceChain> chain,
    std::string authority,
    std::string path,
    uint64_t version,
    folly::Executor::KeepAlive<> executor,
    InflightGuard hostGuard
)
    : chain_(std::move(chain)), authority_(std::move(authority)), path_(std::move(path)),
      executor_(std::move(executor)), hostGuard_(std::move(hostGuard)),
      checks_(std::make_shared<InflightTracker>()), states_(chain_->members().size()) {
  view_ = SessionView{
      .service = chain_->service(),
      .authority = authority_,
      .path = path_,
      .version = version,
  };
}

folly::coro::Task<Result<folly::Unit>>
PluginSession::admit(const void* params, std::shared_ptr<const void> keepAlive) {
  InflightGuard guard(checks_);
  if (!guard || ended_) {
    co_return folly::makeUnexpected(internal("session ended"));
  }
  folly::CancellationSource timeout;
  auto token = folly::cancellation_token_merge(cancel_.getToken(), timeout.getToken());
  auto check = folly::coro::co_withExecutor(
                   executor_,
                   folly::coro::co_withCancellation(
                       token,
                       runAdmit(shared_from_this(), params, std::move(keepAlive), std::move(guard))
                   )
  )
                   .startInlineUnsafe();
  co_return co_await awaitWithin(
      std::move(check),
      chain_->admissionTimeout(),
      std::move(timeout),
      "admission"
  );
}

// The frame holds `self`, `keepAlive` and `guard` until the chain ends,
// including past the admission budget.
folly::coro::Task<Result<folly::Unit>> PluginSession::runAdmit(
    [[maybe_unused]] std::shared_ptr<PluginSession> self,
    const void* params,
    [[maybe_unused]] std::shared_ptr<const void> keepAlive,
    [[maybe_unused]] InflightGuard guard
) {
  const auto& members = chain_->members();
  for (size_t i = 0; i < members.size(); ++i) {
    const auto& member = members[i];
    if (!member.hooks.has(Hook::Admit)) {
      continue;
    }
    auto token = co_await folly::coro::co_current_cancellation_token;
    if (token.isCancellationRequested()) {
      co_return folly::makeUnexpected(internal("admission cancelled"));
    }
    SetupView setup{.session = view_, .params = ParamsView(params)};
    auto result = co_await folly::coro::co_awaitTry(member.instance->admit(setup));
    if (result.hasException()) {
      XLOG(ERR) << "plugin '" << member.id << "' admit threw: " << describe(result.exception());
      co_return folly::makeUnexpected(internal("plugin '" + member.id + "' failed"));
    }
    if (result->hasError()) {
      co_return folly::makeUnexpected(std::move(result->error()));
    }
    states_[i] = std::move(result->value());
  }
  co_return folly::unit;
}

PluginSession::StartedCheck
PluginSession::startAuthorize(RequestArgs args, folly::CancellationToken requestToken) {
  StartedCheck started;
  InflightGuard guard(checks_);
  if (!guard || ended_) {
    started.result =
        folly::makeSemiFuture<Result<folly::Unit>>(folly::makeUnexpected(internal("session ended"))
        );
    return started;
  }
  auto token = folly::cancellation_token_merge(
      cancel_.getToken(),
      std::move(requestToken),
      started.timeout.getToken()
  );
  started.result = folly::coro::co_withExecutor(
                       executor_,
                       folly::coro::co_withCancellation(
                           std::move(token),
                           runAuthorize(shared_from_this(), std::move(args), std::move(guard))
                       )
  )
                       .startInlineUnsafe();
  return started;
}

folly::coro::Task<Result<folly::Unit>>
PluginSession::authorize(RequestArgs args, folly::CancellationToken requestToken) {
  co_return co_await awaitAuthorize(startAuthorize(std::move(args), std::move(requestToken)));
}

folly::coro::Task<Result<folly::Unit>> PluginSession::awaitAuthorize(StartedCheck check) {
  co_return co_await awaitWithin(
      std::move(check.result),
      chain_->authorizeTimeout(),
      std::move(check.timeout),
      "authorization"
  );
}

// The frame holds `self`, `args.keepAlive` and `guard` until the chain ends.
folly::coro::Task<Result<folly::Unit>> PluginSession::runAuthorize(
    [[maybe_unused]] std::shared_ptr<PluginSession> self,
    RequestArgs args,
    [[maybe_unused]] InflightGuard guard
) {
  const auto& members = chain_->members();
  for (size_t i = 0; i < members.size(); ++i) {
    const auto& member = members[i];
    if (!member.hooks.has(Hook::Authorize)) {
      continue;
    }
    auto token = co_await folly::coro::co_current_cancellation_token;
    if (token.isCancellationRequested()) {
      co_return folly::makeUnexpected(internal("request cancelled"));
    }
    RequestView request{
        .kind = args.kind,
        .ns = args.ns,
        .track = args.track,
        .params = ParamsView(args.params),
        .session = view_,
        .peer = args.peer,
    };
    const SessionState* state = states_[i] ? states_[i]->get() : nullptr;
    auto result = co_await folly::coro::co_awaitTry(member.instance->authorize(request, state));
    if (result.hasException()) {
      XLOG(ERR) << "plugin '" << member.id << "' authorize threw: " << describe(result.exception());
      co_return folly::makeUnexpected(internal("plugin '" + member.id + "' failed"));
    }
    if (result->hasError()) {
      co_return folly::makeUnexpected(std::move(result->error()));
    }
  }
  // A check that resumes after the session ended must not reach the relay.
  auto token = co_await folly::coro::co_current_cancellation_token;
  if (token.isCancellationRequested() || ended_) {
    co_return folly::makeUnexpected(internal("request cancelled"));
  }
  co_return folly::unit;
}

void PluginSession::end() {
  if (ended_) {
    return;
  }
  ended_ = true;
  checks_->close();
  cancel_.requestCancellation();
  // The record goes with the session; the task holds it from here on.
  folly::coro::co_withExecutor(executor_, runSessionEnded(shared_from_this())).start();
}

folly::coro::Task<void>
PluginSession::runSessionEnded([[maybe_unused]] std::shared_ptr<PluginSession> self) {
  co_await checks_->drained();
  const auto& members = chain_->members();
  for (size_t i = 0; i < members.size(); ++i) {
    if (!states_[i] || !members[i].hooks.has(Hook::SessionEnded)) {
      continue;
    }
    auto result =
        co_await folly::coro::co_awaitTry(members[i].instance->sessionEnded(std::move(*states_[i]))
        );
    if (result.hasException()) {
      XLOG(ERR) << "plugin '" << members[i].id
                << "' sessionEnded threw: " << describe(result.exception());
    }
  }
  states_.clear();
  // Releasing the host guard last lets stop() proceed.
  hostGuard_ = InflightGuard();
}

// --- Parameters ---

namespace {

bool isHostKey(uint64_t key) {
  using moxygen::SetupKey;
  for (auto reserved :
       {SetupKey::PATH,
        SetupKey::MAX_REQUEST_ID,
        SetupKey::AUTHORIZATION_TOKEN,
        SetupKey::MAX_AUTH_TOKEN_CACHE_SIZE,
        SetupKey::AUTHORITY,
        SetupKey::MOQT_IMPLEMENTATION,
        SetupKey::HOP_ID,
        SetupKey::RELAY_COST}) {
    if (key == folly::to_underlying(reserved)) {
      return true;
    }
  }
  return false;
}

} // namespace

folly::Expected<folly::Unit, std::string>
mergeParams(Params& into, const Params& from, std::string_view owner) {
  for (const auto& param : from) {
    auto keyText = std::to_string(param.key);
    if (isHostKey(param.key)) {
      return folly::makeUnexpected(
          "plugin '" + std::string(owner) + "' declares SETUP parameter " + keyText +
          ", which the host writes"
      );
    }
    for (const auto& existing : into) {
      if (existing.key == param.key) {
        return folly::makeUnexpected(
            "plugin '" + std::string(owner) + "' declares SETUP parameter " + keyText +
            ", already declared by another plugin"
        );
      }
    }
    into.push_back(param);
  }
  return folly::unit;
}

// --- PluginHost ---

folly::Expected<std::shared_ptr<PluginHost>, std::string>
PluginHost::create(const config::Config& config) {
  auto host = std::make_shared<PluginHost>();
  auto addRoot = [&](const config::PluginEntryConfig& entry, Scope scope, const std::string& service
                 ) -> folly::Expected<folly::Unit, std::string> {
    auto root = entry.pluginType->create(entry.config.get());
    if (root.hasError()) {
      return folly::makeUnexpected("plugin '" + entry.id + "': " + root.error().reason);
    }
    host->roots_.push_back(
        RootEntry{entry, scope, service, std::shared_ptr<Root>(std::move(root.value()))}
    );
    return folly::unit;
  };
  for (const auto& entry : config.plugins) {
    if (auto res = addRoot(entry, Scope::Process, ""); res.hasError()) {
      return folly::makeUnexpected(res.error());
    }
  }
  for (const auto& [name, svc] : config.services) {
    host->serviceConfigs_.emplace(name, svc.plugins);
    host->timeouts_.emplace(name, std::make_pair(svc.admissionTimeout, svc.authorizeTimeout));
  }
  for (const auto& [name, entries] : host->serviceConfigs_) {
    for (const auto& entry : entries) {
      if (auto res = addRoot(entry, Scope::Service, name); res.hasError()) {
        return folly::makeUnexpected(res.error());
      }
    }
  }
  return host;
}

folly::coro::Task<folly::Expected<folly::Unit, std::string>>
PluginHost::startRoots(folly::CancellationToken token) {
  for (auto& entry : roots_) {
    if (token.isCancellationRequested()) {
      co_await stopRoots();
      co_return folly::makeUnexpected(std::string("startup cancelled"));
    }
    auto result = co_await folly::coro::co_awaitTry(
        folly::coro::co_withCancellation(token, entry.root->start())
    );
    if (result.hasException()) {
      auto error = "plugin '" + entry.config.id + "' start failed: " + describe(result.exception());
      co_await stopRoots();
      co_return folly::makeUnexpected(std::move(error));
    }
    entry.started = true;
  }
  co_return folly::unit;
}

folly::coro::Task<folly::Expected<Params, std::string>>
PluginHost::listenerParams(std::string_view listener) {
  Params params;
  for (auto& entry : roots_) {
    if (entry.scope != Scope::Process || !entry.root->declaresListenerParams()) {
      continue;
    }
    auto result =
        co_await folly::coro::co_awaitTry(entry.root->setupParams(ListenerScope{listener}));
    if (result.hasException()) {
      co_return folly::makeUnexpected(
          "plugin '" + entry.config.id + "' setupParams failed: " + describe(result.exception())
      );
    }
    if (result->hasError()) {
      co_return folly::makeUnexpected(
          "plugin '" + entry.config.id + "' setupParams failed: " + result->error().reason
      );
    }
    if (auto merged = mergeParams(params, result->value(), entry.config.id); merged.hasError()) {
      co_return folly::makeUnexpected(merged.error());
    }
  }
  co_return params;
}

folly::Expected<folly::Unit, std::string> PluginHost::createServices(const ExecutorFor& executorFor
) {
  for (const auto& [name, svcEntries] : serviceConfigs_) {
    auto exec = executorFor(name);
    ServiceContext ctx{.serviceName = name, .relayExecutor = exec};
    struct Ranked {
      int64_t rank;
      size_t order;
      ChainMember member;
    };
    std::vector<Ranked> processMembers;
    std::vector<Ranked> serviceMembers;
    ServiceEntry service{.service = name};
    size_t order = 0;
    for (auto& root : roots_) {
      if (root.scope == Scope::Service && root.service != name) {
        continue;
      }
      auto instance = root.root->createService(ctx);
      if (instance.hasError()) {
        return folly::makeUnexpected(
            "plugin '" + root.config.id + "' for service '" + name + "': " + instance.error().reason
        );
      }
      std::shared_ptr<ServiceInstance> shared = std::move(instance.value());
      service.lifecycle.push_back(shared);
      service.lifecycleIds.push_back(root.config.id);
      auto& target = root.scope == Scope::Process ? processMembers : serviceMembers;
      target.push_back(Ranked{
          root.config.rank,
          order++,
          ChainMember{root.config.id, shared, shared->hooks()},
      });
    }
    auto byRank = [](const Ranked& a, const Ranked& b) {
      return std::tie(a.rank, a.order) < std::tie(b.rank, b.order);
    };
    std::sort(processMembers.begin(), processMembers.end(), byRank);
    std::sort(serviceMembers.begin(), serviceMembers.end(), byRank);
    std::vector<ChainMember> members;
    for (auto* group : {&processMembers, &serviceMembers}) {
      for (auto& ranked : *group) {
        members.push_back(std::move(ranked.member));
      }
    }
    auto [admission, authorize] = timeouts_.at(name);
    service.chain = std::make_shared<const ServiceChain>(
        name,
        std::move(members),
        admission,
        authorize,
        std::move(exec)
    );
    services_.emplace(name, std::move(service));
  }
  return folly::unit;
}

folly::coro::Task<folly::Expected<folly::Unit, std::string>>
PluginHost::startServices(folly::CancellationToken token) {
  for (auto& [name, service] : services_) {
    for (size_t i = 0; i < service.lifecycle.size(); ++i) {
      if (token.isCancellationRequested()) {
        co_await stopServices();
        co_return folly::makeUnexpected(std::string("startup cancelled"));
      }
      auto result = co_await folly::coro::co_awaitTry(folly::coro::co_withExecutor(
          service.chain->relayExecutor(),
          folly::coro::co_withCancellation(token, service.lifecycle[i]->start())
      ));
      if (result.hasException()) {
        auto error = "plugin '" + service.lifecycleIds[i] + "' start for service '" + name +
                     "' failed: " + describe(result.exception());
        co_await stopServices();
        co_return folly::makeUnexpected(std::move(error));
      }
      service.started = i + 1;
    }
  }
  co_return folly::unit;
}

std::shared_ptr<const ServiceChain> PluginHost::chain(std::string_view service) const {
  auto it = services_.find(service);
  if (it == services_.end() || it->second.chain->members().empty()) {
    return nullptr;
  }
  return it->second.chain;
}

std::shared_ptr<PluginSession> PluginHost::newSession(
    std::string_view service,
    std::string authority,
    std::string path,
    uint64_t version,
    folly::Executor::KeepAlive<> executor
) {
  auto serviceChain = chain(service);
  if (!serviceChain) {
    return nullptr;
  }
  InflightGuard guard(tracker_);
  if (!guard) {
    return nullptr;
  }
  return std::make_shared<PluginSession>(
      std::move(serviceChain),
      std::move(authority),
      std::move(path),
      version,
      std::move(executor),
      std::move(guard)
  );
}

folly::coro::Task<folly::Expected<Params, std::string>>
PluginHost::upstreamParams(std::string service, std::string url) {
  Params params;
  auto serviceChain = chain(service);
  if (!serviceChain) {
    co_return params;
  }
  InflightGuard guard(tracker_);
  if (!guard) {
    co_return folly::makeUnexpected(std::string("shutting down"));
  }
  for (const auto& member : serviceChain->members()) {
    if (!member.hooks.has(Hook::UpstreamSetupParams)) {
      continue;
    }
    auto result =
        co_await folly::coro::co_awaitTry(member.instance->setupParams(UpstreamScope{service, url})
        );
    if (result.hasException()) {
      co_return folly::makeUnexpected(
          "plugin '" + member.id + "' setupParams failed: " + describe(result.exception())
      );
    }
    if (result->hasError()) {
      co_return folly::makeUnexpected(
          "plugin '" + member.id + "' setupParams failed: " + result->error().reason
      );
    }
    if (auto merged = mergeParams(params, result->value(), member.id); merged.hasError()) {
      co_return folly::makeUnexpected(merged.error());
    }
  }
  co_return params;
}

folly::coro::Task<std::vector<PluginSnapshot>>
PluginHost::snapshots(std::string service, std::chrono::milliseconds budget) {
  std::vector<PluginSnapshot> out;
  auto serviceChain = chain(service);
  if (!serviceChain) {
    co_return out;
  }
  for (const auto& member : serviceChain->members()) {
    if (!member.hooks.has(Hook::StateSnapshot)) {
      continue;
    }
    InflightGuard guard(tracker_);
    if (!guard) {
      out.push_back(PluginSnapshot{member.id, "", "shutting down"});
      continue;
    }
    auto snapshot = folly::coro::co_withExecutor(
                        serviceChain->relayExecutor(),
                        folly::coro::co_invoke(
                            [instance = member.instance,
                             guard = std::move(guard)]() -> folly::coro::Task<folly::dynamic> {
                              co_return co_await instance->stateSnapshot();
                            }
                        )
    ).start();
    auto result =
        co_await folly::coro::co_awaitTry(folly::coro::timed_wait(std::move(snapshot), budget));
    if (result.hasException()) {
      out.push_back(PluginSnapshot{member.id, "", describe(result.exception())});
    } else if (!*result) {
      out.push_back(PluginSnapshot{member.id, "", "snapshot timed out"});
    } else {
      // A plugin value can fail to serialize (NaN, a non-string key).
      try {
        out.push_back(PluginSnapshot{member.id, folly::toJson(**result), std::nullopt});
      } catch (const std::exception& ex) {
        out.push_back(PluginSnapshot{member.id, "", ex.what()});
      }
    }
  }
  co_return out;
}

folly::coro::Task<void> PluginHost::stop() {
  tracker_->close();
  co_await tracker_->drained();
  co_await stopServices();
  co_await stopRoots();
}

folly::coro::Task<void> PluginHost::stopServices() {
  for (auto it = services_.rbegin(); it != services_.rend(); ++it) {
    auto& service = it->second;
    while (service.started > 0) {
      --service.started;
      auto result = co_await folly::coro::co_awaitTry(folly::coro::co_withExecutor(
          service.chain->relayExecutor(),
          service.lifecycle[service.started]->stop()
      ));
      if (result.hasException()) {
        XLOG(ERR) << "plugin '" << service.lifecycleIds[service.started] << "' stop for service '"
                  << service.service << "' failed: " << describe(result.exception());
      }
    }
  }
}

folly::coro::Task<void> PluginHost::stopRoots() {
  for (auto it = roots_.rbegin(); it != roots_.rend(); ++it) {
    if (!it->started) {
      continue;
    }
    it->started = false;
    auto result = co_await folly::coro::co_awaitTry(it->root->stop());
    if (result.hasException()) {
      XLOG(ERR) << "plugin '" << it->config.id << "' stop failed: " << describe(result.exception());
    }
  }
}

} // namespace openmoq::moqx::plugin
