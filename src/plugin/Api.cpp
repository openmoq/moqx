/*
 * Copyright (c) OpenMOQ contributors.
 * This source code is licensed under the Apache 2.0 license found in the
 * LICENSE file in the root directory of this source tree.
 */

#include "plugin/Api.h"

#include <moxygen/MoQTypes.h>

namespace openmoq::moqx::plugin {

namespace {

const moxygen::Parameters& params(const void* p) {
  return *static_cast<const moxygen::Parameters*>(p);
}

Error notImplemented(std::string_view hook) {
  return Error{Error::Kind::Internal, std::string(hook) + " is not implemented"};
}

} // namespace

std::vector<AuthToken> ParamsView::authTokens(uint64_t tokenType) const {
  std::vector<AuthToken> out;
  for (const auto& param : params(params_)) {
    if (param.key == folly::to_underlying(moxygen::TrackRequestParamKey::AUTHORIZATION_TOKEN) &&
        param.asAuthToken.tokenType == tokenType) {
      out.push_back(AuthToken{param.asAuthToken.tokenType, param.asAuthToken.tokenValue});
    }
  }
  return out;
}

std::optional<uint64_t> ParamsView::uintParam(uint64_t key) const {
  if (const auto* param = params(params_).getFirstParam(key)) {
    return param->asUint64;
  }
  return std::nullopt;
}

std::optional<std::string_view> ParamsView::bytesParam(uint64_t key) const {
  if (const auto* param = params(params_).getFirstParam(key)) {
    return std::string_view(param->asString);
  }
  return std::nullopt;
}

folly::coro::Task<Result<std::shared_ptr<SessionState>>> ServiceInstance::admit(const SetupView&) {
  co_return folly::makeUnexpected(notImplemented("admit"));
}

folly::coro::Task<void> ServiceInstance::sessionEnded(std::shared_ptr<SessionState>) {
  co_return;
}

folly::coro::Task<Result<folly::Unit>>
ServiceInstance::authorize(const RequestView&, const SessionState*) {
  co_return folly::makeUnexpected(notImplemented("authorize"));
}

folly::coro::Task<Result<Params>> ServiceInstance::setupParams(const UpstreamScope&) {
  co_return Params{};
}

folly::coro::Task<folly::dynamic> ServiceInstance::stateSnapshot() {
  co_return folly::dynamic::object();
}

folly::coro::Task<Result<Params>> Root::setupParams(const ListenerScope&) {
  co_return Params{};
}

} // namespace openmoq::moqx::plugin
